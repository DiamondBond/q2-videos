/* The Q2's sound output: ALSA, with what Q2 Pod's own video helper learned about it (q2-pod
 * patch/video.c sound_open and writer; docs/internals.md, Videos, says why each is there).
 *
 * Q2 Pod stops its player and hands the output over, but the player (hciplayer) lets go of the
 * device a moment later, muting the headphone DAC as it goes, and bluealsa, one client a PCM,
 * can take longer: so the open is retried, 2 s on the jack and 5 s elsewhere, and bluealsa, which
 * claims its PCM at set_params and not at open, is retried there while it says busy. On the jack
 * the DAC is then taken out of DSD mode and unmuted, and muted again at the close, so nothing is
 * heard until Q2 Pod powers it down.
 *
 * All of it is called from the engine's audio thread alone: alsa-lib 1.1.6 deadlocks in its own
 * locking (LIBASOUND_THREAD_SAFE=0, set in player_main), so no two threads may be in it. */
#include "player.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DAC "/dev/shanling_dac"
#define DAC_PCM 0xc0044d1bu  /* hciplayer sets it to 0 for each PCM track: undoes a DSD one */
#define DAC_MUTE 0xc0044d1fu /* hciplayer sets it on pause and close, clears it on start */

static snd_pcm_t *pcm;
static int jack; /* the headphone DAC, plughw:0,0: its mute is ours while we have it */
static snd_pcm_uframes_t buffer; /* ALSA's ring, frames */

static void dac_set(unsigned long request, int v) {
    int fd = open(DAC, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, request, &v);
    close(fd);
}

static int alsa_open(const char *dev, int patient, const volatile int *quit) {
    char usb[16];
    int err = -1, tries = !patient ? 1 : !strcmp(dev, "plughw:0,0") ? 20 : 50;
    snd_pcm_uframes_t period;
    if (!strcmp(dev, "usb")) {
        char *cards = file_get("/proc/asound/cards", 0);
        dev = usb_device(cards, usb);
        free(cards);
    }
    jack = !strcmp(dev, "plughw:0,0");
    for (int i = 0;
         i < tries && !*quit && (err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)); ++i)
        pcm = 0, usleep(100000);
    if (err) return pcm = 0, 0;
    for (int i = 0;
         (err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                   RATE, 1, 200000)) == -EBUSY &&
         !jack && i < tries && !*quit;
         ++i)
        usleep(100000);
    if (err) return snd_pcm_close(pcm), pcm = 0, 0;
    if (snd_pcm_get_params(pcm, &buffer, &period)) buffer = RATE / 5;
    if (jack) dac_set(DAC_PCM, 0), dac_set(DAC_MUTE, 0);
    /* Above the decoders: when the picture is too much for the Q2 it is the picture that falls
     * behind, never the sound. Under the kernel's IRQ threads (50). */
    pthread_setschedparam(pthread_self(), SCHED_FIFO,
                          &(struct sched_param){ .sched_priority = 40 });
    return 1;
}

static long alsa_write(const short *s, long frames) {
    int recovered = 0;
    for (long sent = 0; sent < frames;) {
        long r = snd_pcm_writei(pcm, s + 2 * sent, (snd_pcm_uframes_t)(frames - sent));
        if (r < 0) { /* an underrun, a suspend: mended once; a second failure is a lost device */
            if (recovered || snd_pcm_recover(pcm, (int)r, 1)) return -1;
            recovered = 1;
        } else if (!r)
            usleep(1000);
        else
            sent += r, recovered = 0;
    }
    return frames;
}

/* Not yet heard: ALSA's delay, floored at what its ring still holds. Nothing is heard while it
 * holds all that was written (its first fill; Bluetooth adds the link's own delay), and after a
 * drop bluealsa answers the link's delay alone. A delay it can't say is none: it ran dry. */
static long alsa_unheard(long long written) {
    snd_pcm_sframes_t delay, avail = snd_pcm_avail(pcm);
    long ring = avail >= 0 ? (long)buffer - (long)avail : 0;
    if (snd_pcm_delay(pcm, &delay) || delay < 0) delay = 0;
    if (delay < ring) delay = ring;
    return delay < written ? delay : (long)written;
}

static void alsa_drop(void) { snd_pcm_drop(pcm), snd_pcm_prepare(pcm); }

static void alsa_close(void) {
    if (!pcm) return;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = 0;
    if (jack) dac_set(DAC_MUTE, 1);
}

const sink_t sink_alsa = { alsa_open, alsa_write, alsa_unheard, alsa_drop, alsa_close };
