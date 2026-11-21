/* What the server and the player share besides the card's state: where the files are, whole
 * files, the player's lock, the memory a picture needs, the clock, and how a video is opened. */
#include "videos.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>

#define FILE_MAX (8u << 20) /* file_get's most: nothing the app reads whole is near it */
#define FRAMES_HELD 10      /* frames a decode holds at once, at most (too_large) */

char card_dir[256] = "/mnt/mmc", state_dir[272] = "/mnt/mmc/.videos", tmp_dir[256] = "/tmp";

void paths_init(void) {
    const char *card = getenv("VIDEOS_CARD"), *tmp = getenv("VIDEOS_TMP");
    if (card && *card && strlen(card) < sizeof card_dir) strcpy(card_dir, card);
    if (tmp && *tmp && strlen(tmp) < sizeof tmp_dir) strcpy(tmp_dir, tmp);
    snprintf(state_dir, sizeof state_dir, "%s/.videos", card_dir);
}

char *file_get(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size_t room = 4096, len = 0;
    char *s = malloc(room);
    ssize_t k = 0;
    /* read to the end, not to st_size: /proc's files say 0 */
    while (s && (k = read(fd, s + len, room - 1 - len)) > 0) {
        len += (size_t)k;
        if (len + 1 < room) continue;
        char *more = room < FILE_MAX ? realloc(s, room *= 2) : 0;
        if (!more) free(s);
        s = more;
    }
    close(fd);
    if (!s || k < 0) return free(s), (char *)0;
    s[len] = 0;
    if (n) *n = len;
    return s;
}

/* the folder holding path, synced: the rename itself reaches the card */
static void dir_sync(const char *path) {
    char dir[PATH_N];
    const char *slash = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}

int file_put(const char *path, const void *data, size_t n) {
    char tmp[PATH_N];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    size_t done = 0;
    ssize_t k;
    while (done < n && (k = write(fd, (const char *)data + done, n - done)) > 0) done += (size_t)k;
    int ok = done == n && !fsync(fd);
    if (close(fd)) ok = 0;
    if (ok && !rename(tmp, path)) return dir_sync(path), 1;
    unlink(tmp);
    return 0;
}

int lock_take(int exclusive) {
    char path[PATH_N];
    snprintf(path, sizeof path, "%s/q2-videos.lock", tmp_dir);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    /* -1 from the exclusive wait is a signal's: the player was told to end before it began */
    if (fd >= 0 && flock(fd, exclusive ? LOCK_EX : LOCK_SH | LOCK_NB)) close(fd), fd = -1;
    return fd;
}

unsigned mem_available(void) {
    char *info = file_get("/proc/meminfo", 0), *m = info ? strstr(info, "MemAvailable:") : 0;
    unsigned kb = m ? (unsigned)atoi(m + 13) : 0;
    free(info);
    return kb;
}

/* ponytail: FRAMES_HELD is q2-pod's patch/video.c's, counted on a PC with ffmpeg as the decoder:
 * 4 frames' worth (one reference, no B-frames) to 10 (three references and B-frames, or HEVC).
 * A video that starts and then stalls the Q2 means raising it; one refused that would play,
 * lowering it. */
int too_large(int w, int h, int bits, unsigned avail_kb) {
    return w > 0 && h > 0 && avail_kb && /* /1024 first: no overflow */
           (long long)w * h / 1024 * bits * FRAMES_HELD / 8 > avail_kb;
}

int pix_bits(int format) {
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(format);
    return d ? av_get_padded_bits_per_pixel(d) : 12;
}

long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}

AVFormatContext *input_open(const char *path, int (*stop)(void *), void *arg, int *err) {
    char url[PATH_N + 8];
    AVDictionary *opts = 0;
    AVFormatContext *ic = avformat_alloc_context();
    int r = AVERROR(ENOMEM);
    if (ic) {
        ic->interrupt_callback = (AVIOInterruptCB){ stop, arg };
        snprintf(url, sizeof url, "file:%s", path);
        /* a file from anyone's card: only the containers named, and nothing but files. No check
         * can show the second line missing: none of FFmpeg 4.2's readers named opens another
         * resource by itself (mov's external tracks are off by default). It stays for one that
         * would. */
        av_dict_set(&opts, "format_whitelist", FORMATS, 0);
        av_dict_set(&opts, "protocol_whitelist", "file", 0);
        if ((r = avformat_open_input(&ic, url, 0, &opts)) < 0) ic = 0; /* a failed open freed it */
        av_dict_free(&opts);
    }
    if (err) *err = r;
    return ic;
}

int rotation(AVStream *st) {
    const AVDictionaryEntry *t = av_dict_get(st->metadata, "rotate", 0, 0);
    return t ? (int)((strtol(t->value, 0, 10) % 360 + 360) % 360) : 0;
}
