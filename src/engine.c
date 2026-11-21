/* The engine: a video demuxed, decoded and kept in step with its sound.
 *
 * Four threads. The reader opens the file and demuxes it into two bounded queues of packets; the
 * video thread decodes, scales and turns each picture onto a page, three of them at most ahead;
 * the audio thread decodes, resamples and writes the sound to the output, and what the output
 * says it has played is the clock; the main thread (player.c) asks eng_poll for the picture due
 * and draws it. One mutex guards everything they share and one condition wakes whoever waits, so
 * there is no order of locks to get wrong: a thread takes the lock to look and to hand over, and
 * decodes, reads, scales and writes without it.
 *
 * A seek bumps the serial: packets and pictures carry the serial they were read for and older
 * ones are dropped wherever they are met, so nothing has to be chased through the threads. Every
 * wait ends on quit, and eng_close waits a second at most for the threads: one still in a read
 * of the card or in ALSA then is left there, and the process ends by _exit.
 *
 * The clock is the sound's, whatever plays: while the picture runs the audio thread always
 * writes something, the track's samples or silence where there are none (no track, a gap, its
 * end before the picture's), and with no output that takes it, into one that counts time
 * instead. So there is one clock and one way to read it.
 *
 * For the checks the clock can be the script's (clock_virtual): time then moves only when the
 * main thread waits and every other thread has done all it can, so a run plays as fast as it
 * decodes and the same script always shows the same frames. */
#include "player.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define PICS 3                /* pictures decoded ahead: 1.4 MB */
#define QUEUE_BYTES (5 << 20) /* packets held, both queues: the reader waits above it */
#define ENOUGH 50             /* packets of a stream that are enough ahead: a second or two */
#define CHUNK 512             /* frames a write: the clock's step, about 11 ms */
#define RING (RATE / 5)       /* the silent output holds 200 ms, as ALSA's buffer does */
#define HIST (RATE / 2)       /* frames kept to write again after a pause */
#define STALL_MS 3000         /* a write held this long: the output takes no sound */
#define TOL 50000             /* us a sound frame's time may be off its place and still join on */
#define OVERLAP_MAX 5000000   /* sound this far before its place is cut; more is a jump back */
#define ECUES 64              /* the file's own subtitle cues kept */
#define NONE INT64_MIN

typedef struct node {
    struct node *next;
    AVPacket *pkt; /* 0: the stream's end */
    int serial;
} node_t;
typedef struct {
    node_t *head, **tail;
    int n;
    long long bytes;
} pq_t;

typedef struct {
    pix *px;
    long long pts, dur; /* us */
    int serial;
    geom_t g;
} pic_t;

/* A thread of the engine's, as the script's clock sees it: busy, or waiting for a change it has
 * seen the latest of (seen), perhaps for a time (until). */
typedef struct {
    pthread_t th;
    int on, busy;
    unsigned seen;
    long long until;
} worker_t;

/* An audio thread's own. One that hangs in its output is abandoned with all of this, and a new
 * one plays on silent: the old one's is never touched again. */
typedef struct {
    worker_t w;
    int device;                 /* it plays to the output asked for; 0: to the silent one */
    int alsa;                   /* that output is ALSA and open */
    int abandoned;              /* it hangs in its output: when it comes back it only ends */
    volatile unsigned write_at; /* now_ms | 1 when it went into a write, 0 out of it */
    long long n0, nw; /* the silent output: when what it holds began to play, and how much */
    int serial, track,
        opened;             /* the seek and the sound track it is on; its decoder was asked for */
    int tl;                 /* its timeline: 0 not begun, 1 begun, 2 begins at its first frame */
    long long base, frames; /* what is written ends at base + frames / RATE */
    long long written;      /* frames in the output since it was opened or dropped */
    short hist[HIST * 2]; /* the last written, before the volume: a pause plays the unheard again */
    int hist_n, hist_at;
    short rep[HIST * 4]; /* those, waiting to be written again */
    int rep_n, rep_at;
    long long pad; /* frames of silence to write first */
    short *pend;   /* a decoded frame's samples, resampled, not written yet */
    int pend_n, pend_at;
    unsigned pend_room;
    SwrContext *swr; /* to 48 kHz stereo, from what the decoder last gave: */
    int fmt, rate;
    uint64_t layout;
    int fed, drained, held, lost;
    long long start;   /* a thread that takes over starts its timeline here, or NONE */
    int zc, zn, zlast; /* the tone's count */
} au_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond, idle; /* idle: a thread went to wait (the script's clock listens) */
    unsigned kicks;            /* changes so far */
    int notify;                /* the main thread has something to look at */
    int on, stuck;
    volatile int quit;
    worker_t reader, video;
    au_t *au;

    /* what was asked for */
    char path[PATH_N], device[32];
    int serial; /* counts the seeks: what was read for an earlier one is dropped */
    struct {
        int pending, how;
        long long ms, from; /* from: where it was, us, for a seek ahead to get well past */
    } seek;
    long long target_ms; /* where the last seek went: the place until the clock is there */
    int paused, fill, retry;
    volatile int gain;
    int want_audio, aswitch; /* the nth sound track, and a count of the changes asked for */
    int want_sub, sub_nth;   /* the subtitle stream to decode, -1 none; at the start, the nth */

    /* the file */
    int state;
    char why[160];
    media_t m;
    AVFormatContext *ic;
    AVCodecContext *vdec;
    int vi, ai, still;  /* the streams playing, -1 none; still: the picture is a cover */
    int aswitched;      /* the changes of sound track the reader has made */
    long long start_us; /* the file's first timestamp */
    long long jump_us;  /* timestamps further apart than this are a jump, not a gap */
    long long frame_us; /* a picture's time on the screen when its frame doesn't say */
    int rot;
    AVRational sar;
    int rserial;       /* the serial the reader's place is for */
    long long skip_us; /* an exact seek: what is before this is decoded and dropped */
    int skip_serial;
    int eof;
    pq_t vq, aq;

    /* pictures: the video thread fills pic[(ridx + count) % PICS], the main thread takes pic[ridx]
     */
    pic_t pic[PICS];
    int ridx, count;
    int first_serial; /* the seek whose first picture is decoded, at first_us */
    long long first_us;
    int shown_serial, picture; /* the main thread's: the seek its picture is of */
    long long shown_us, shown_dur;
    geom_t geom;
    int vdone, adone;
    long long aend_us; /* where the sound track ended */

    /* the clock, as the audio thread last left it */
    struct {
        int serial, live; /* live: the output is playing, so time runs on from stamp */
        long long heard, end, stamp;
    } clk;
    int lost, silent;

    struct {
        long long start, end; /* ms; end 0: an empty slot */
        char text[LINE_N];
    } cue[ECUES];
    int ncue;

    long long frames; /* the checks': sound sent out, its tone, the ladder's level */
    int tone, level;
    unsigned mem_kb;
    int fault, checked; /* checked: a script runs this, and reads the tone */
} E = { .lock = PTHREAD_MUTEX_INITIALIZER };
enum { FAULT_NONE, FAULT_STALL, FAULT_LOST, FAULT_GONE };

/* ---- Time, and waking the main thread ------------------------------------------------------ */

static int virt;
static volatile int vnow; /* an int, read whole by any thread: a script's run is minutes */
static int wake[2] = { -1, -1 };

long long now_ms(void) { return virt ? vnow : mono_ms(); }

/* from a second in at least: no time of a run is 0, which the player reads as never */
void clock_virtual(int from) { virt = 1, vnow = from > 1000 ? from : 1000; }

int wake_fd(void) {
    if (wake[0] < 0 && pipe2(wake, O_CLOEXEC | O_NONBLOCK)) wake[0] = wake[1] = -1;
    return wake[0];
}

void wake_main(void) {
    int e = errno;
    if (wake[1] >= 0 && write(wake[1], "", 1) < 0) { /* full: it is awake already */
    }
    errno = e;
}

void wake_drain(void) {
    char b[64];
    while (wake[0] >= 0 && read(wake[0], b, sizeof b) > 0) {}
}

static void lock(void) { pthread_mutex_lock(&E.lock); }
static void unlock(void) { pthread_mutex_unlock(&E.lock); }
/* something changed: whoever waits looks again */
static void kick(void) {
    ++E.kicks;
    pthread_cond_broadcast(&E.cond);
}
static void notify(void) {
    if (!E.notify) E.notify = 1, wake_main();
}

/* A thread's wait, the lock held: until the next change, or until the clock reaches until (0:
 * no time). It comes back for any change, so the caller looks again. A time below zero is none
 * a clock gives: taken as no time, where pthread_cond_timedwait would refuse it with the lock
 * still held and the caller spin on it. */
static void wait_for(worker_t *w, long long until) {
    if (until < 0) until = 0;
    w->busy = 0, w->seen = E.kicks, w->until = until;
    pthread_cond_signal(&E.idle);
    if (until && !virt) {
        struct timespec ts = { (time_t)(until / 1000), (long)(until % 1000) * 1000000 };
        pthread_cond_timedwait(&E.cond, &E.lock, &ts);
    } else /* the script's clock: its move is a change */
        pthread_cond_wait(&E.cond, &E.lock);
    w->busy = 1, w->until = 0;
}

/* Every thread waits, and has looked since the last change. */
static int settled(void) {
    worker_t *w[3] = { &E.reader, &E.video, E.au ? &E.au->w : 0 };
    for (int i = 0; i < 3; ++i)
        if (E.on && w[i] && w[i]->on && (w[i]->busy || w[i]->seen != E.kicks)) return 0;
    return 1;
}

int clock_wait(long long until) {
    lock();
    for (;;) {
        while (!settled()) pthread_cond_wait(&E.idle, &E.lock);
        if (E.notify) return unlock(), 0;
        if (vnow >= until) break;
        long long t = until;
        worker_t *w[3] = { &E.reader, &E.video, E.au ? &E.au->w : 0 };
        for (int i = 0; i < 3; ++i) /* a thread waiting for a time gets it, no later */
            if (E.on && w[i] && w[i]->on && w[i]->until && w[i]->until < t) t = w[i]->until;
        vnow = t > vnow ? (int)t : vnow + 1;
        kick();
    }
    unlock();
    return 1;
}

/* ---- Queues -------------------------------------------------------------------------------- */

static void pq_put(pq_t *q, AVPacket *pkt, int serial) { /* pkt's data moves into the queue */
    node_t *n = calloc(1, sizeof *n);
    if (n && pkt && !(n->pkt = av_packet_alloc())) free(n), n = 0;
    if (!n) {
        if (pkt) av_packet_unref(pkt);
        return;
    }
    if (pkt) av_packet_move_ref(n->pkt, pkt), q->bytes += n->pkt->size + 256;
    n->serial = serial;
    if (!q->head) q->tail = &q->head;
    *q->tail = n, q->tail = &n->next;
    ++q->n;
}

static node_t *pq_get(pq_t *q) {
    node_t *n = q->head;
    if (!n) return 0;
    q->head = n->next, --q->n;
    if (n->pkt) q->bytes -= n->pkt->size + 256;
    return n;
}

static void node_free(node_t *n) {
    if (n->pkt) av_packet_free(&n->pkt);
    free(n);
}

static void pq_flush(pq_t *q) {
    for (node_t *n; (n = pq_get(q));) node_free(n);
}

/* The reader has read enough ahead: too many bytes, or each stream that plays has its second
 * or two. A stream with nothing coming (the cover of a song, a sound that stops early) never
 * has, so the bytes end it. */
static int capped(void) {
    if (E.vq.bytes + E.aq.bytes >= QUEUE_BYTES) return 1;
    return (E.vi < 0 || E.still || E.vq.n >= ENOUGH) && (E.ai < 0 || E.aq.n >= ENOUGH);
}

/* ---- The clock ----------------------------------------------------------------------------- */

/* The time being heard, us, the lock held: 0 when the sound hasn't begun at this place. While
 * the output plays, the time runs on from when the audio thread last asked it, but never past
 * what was written: an output that stops taking sound stops the picture too. */
static int clock_read(long long now, long long *us) {
    if (E.clk.serial != E.serial) return 0;
    long long t = E.clk.heard + (E.clk.live ? (now - E.clk.stamp) * 1000 : 0);
    *us = t > E.clk.end ? E.clk.end : t;
    return 1;
}

/* ---- The reader ---------------------------------------------------------------------------- */

static int interrupted(void *unused) { return E.quit; }

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.why, sizeof E.why, fmt, ap);
    va_end(ap);
    return 0;
}

/* "AV1", "DTS": a codec as the screen names it */
static const char *codec_name(enum AVCodecID id, char *out, size_t n) {
    snprintf(out, n, "%s", avcodec_get_name(id));
    for (char *p = out; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return out;
}

static void track_name(const AVStream *st, int nth, const char *more, char *out, size_t n) {
    const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", 0, 0),
                            *lang = av_dict_get(st->metadata, "language", 0, 0);
    char number[24];
    snprintf(number, sizeof number, "Track %d", nth + 1);
    snprintf(out, n, "%s%s",
             title && *title->value                               ? title->value
             : lang && *lang->value && strcmp(lang->value, "und") ? lang_name(lang->value)
                                                                  : number,
             more);
}

/* The file opened and looked into: 1 when it can be played, else E.why says why not. */
static int media_open(void) {
    char name[32];
    int err;
    AVFormatContext *ic = input_open(E.path, interrupted, 0, &err); /* ends when we do */
    if (!ic) /* nothing FFmpeg knows, or (EINVAL) something that isn't one of FORMATS */
        return fail(err == AVERROR_INVALIDDATA || err == AVERROR(EINVAL)
                        ? "It is not a video file the Q2 knows."
                        : "The file can't be read.");
    E.ic = ic;
    if (avformat_find_stream_info(ic, 0) < 0) return fail("The Q2 can't tell what is in the file.");
    E.start_us = ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0;
    /* A transport stream's timestamps may start again anywhere, so ten seconds apart is a jump
     * there; elsewhere a gap that long is a gap (a still frame, a silence), as ffplay has it. */
    E.jump_us = ic->iformat->flags & AVFMT_TS_DISCONT ? 10000000 : 3600000000LL;
    E.m.dur_ms = ic->duration != AV_NOPTS_VALUE && ic->duration > 0 ? ic->duration / 1000 : 0;

    /* the sound tracks there is a decoder for */
    int deaf = -1; /* one there is none for */
    for (unsigned i = 0; i < ic->nb_streams; ++i) {
        AVStream *st = ic->streams[i];
        st->discard = AVDISCARD_ALL; /* until it is the one playing */
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (!avcodec_find_decoder(st->codecpar->codec_id)) {
            deaf = (int)i;
            continue;
        }
        if (E.m.naudio == TRACKS) continue;
        track_name(st, E.m.naudio, "", E.m.audio[E.m.naudio].name, sizeof E.m.audio->name);
        E.m.audio[E.m.naudio].index = (int)i, E.m.audio[E.m.naudio++].ok = 1;
    }

    AVCodec *vc = 0;
    E.vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
    if (E.vi == AVERROR_DECODER_NOT_FOUND) {
        for (unsigned i = 0; i < ic->nb_streams; ++i)
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                return fail("The Q2 has no decoder for %s video.",
                            codec_name(ic->streams[i]->codecpar->codec_id, name, sizeof name));
    }
    if (E.vi < 0) E.vi = -1;
    if (E.vi < 0 && !E.m.naudio)
        return deaf < 0
                   ? fail("There is no picture or sound in it.")
                   : fail("The Q2 has no decoder for %s sound.",
                          codec_name(ic->streams[deaf]->codecpar->codec_id, name, sizeof name));
    if (deaf >= 0 && !E.m.naudio)
        snprintf(E.m.note, sizeof E.m.note, "No sound: the Q2 has no decoder for %s",
                 codec_name(ic->streams[deaf]->codecpar->codec_id, name, sizeof name));

    if (E.vi >= 0) {
        AVStream *st = ic->streams[E.vi];
        AVRational rate = av_guess_frame_rate(ic, st, 0);
        E.m.w = st->codecpar->width, E.m.h = st->codecpar->height;
        if (too_large(E.m.w, E.m.h, E.mem_kb ? E.mem_kb : mem_available()))
            return fail(
                "%d \xc3\x97 %d is too large for the Q2's memory. Convert it to 720p or smaller.",
                E.m.w, E.m.h);
        E.vdec = avcodec_alloc_context3(vc);
        if (!E.vdec || avcodec_parameters_to_context(E.vdec, st->codecpar) < 0)
            return fail("The Q2 is out of memory.");
        E.vdec->pkt_timebase = st->time_base;
        E.still = !!(st->disposition & AV_DISPOSITION_ATTACHED_PIC);
        /* the Q2's two cores; a cover is one packet, which frame threads would hold back */
        E.vdec->thread_count = E.still ? 1 : 2;
        E.vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(E.vdec, vc, 0) < 0)
            return fail("The Q2 can't decode this %s video.",
                        codec_name(st->codecpar->codec_id, name, sizeof name));
        E.rot = rotation(st);
        E.sar = av_guess_sample_aspect_ratio(ic, st, 0);
        E.frame_us = rate.num > 0 && rate.den > 0 ? av_rescale(1000000, rate.den, rate.num) : 40000;
        st->discard = AVDISCARD_DEFAULT;
        E.m.video = 1;
    }

    /* the sound: the one asked for, else the file's own choice */
    int best = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, E.vi, 0, 0);
    E.ai = -1, E.m.audio_cur = -1;
    for (int i = 0; i < E.m.naudio; ++i)
        if (E.m.audio[i].index == best || E.m.audio_cur < 0) E.m.audio_cur = i;
    if (E.want_audio >= 0 && E.want_audio < E.m.naudio) E.m.audio_cur = E.want_audio;
    if (E.m.audio_cur >= 0)
        E.ai = E.m.audio[E.m.audio_cur].index, ic->streams[E.ai]->discard = AVDISCARD_DEFAULT;
    E.m.sound = E.ai >= 0;

    /* subtitles: text ones can be shown, pictures (PGS, DVD) only listed */
    for (unsigned i = 0; i < ic->nb_streams && E.m.nsubs < TRACKS; ++i) {
        AVStream *st = ic->streams[i];
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        track_t *t = &E.m.subs[E.m.nsubs];
        t->index = (int)i;
        t->ok =
            d && d->props & AV_CODEC_PROP_TEXT_SUB && avcodec_find_decoder(st->codecpar->codec_id);
        if (t->ok && E.m.nsubs == E.sub_nth) E.want_sub = (int)i;
        track_name(st, E.m.nsubs++, t->ok ? "" : " (pictures)", t->name, sizeof t->name);
    }

    for (unsigned i = 0; i < ic->nb_chapters && E.m.nchapters < CHAPTERS; ++i) {
        const AVChapter *c = ic->chapters[i];
        const AVDictionaryEntry *title = av_dict_get(c->metadata, "title", 0, 0);
        long long at = (av_rescale_q(c->start, c->time_base, AV_TIME_BASE_Q) - E.start_us) / 1000;
        E.m.chapter[E.m.nchapters].start_ms = at < 0 ? 0 : at;
        if (title && *title->value)
            snprintf(E.m.chapter[E.m.nchapters].title, sizeof E.m.chapter->title, "%s",
                     title->value);
        else
            snprintf(E.m.chapter[E.m.nchapters].title, sizeof E.m.chapter->title, "Chapter %d",
                     E.m.nchapters + 1);
        ++E.m.nchapters;
    }
    return 1;
}

static void cue_add(long long start, long long end, const char *text) {
    if (!*text || end <= start) return;
    int at = E.ncue++ % ECUES;
    E.cue[at].start = start, E.cue[at].end = end;
    snprintf(E.cue[at].text, sizeof E.cue[at].text, "%s", text);
}

/* A packet of the subtitle stream through its decoder: the cue it holds, the lock not held. */
static void subtitle(AVCodecContext *dec, const AVStream *st, AVPacket *pkt, int serial) {
    AVSubtitle sub;
    char text[LINE_N] = "", line[LINE_N];
    int got = 0;
    if (pkt->pts == AV_NOPTS_VALUE || avcodec_decode_subtitle2(dec, &sub, &got, pkt) < 0 || !got)
        return;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect *r = sub.rects[i];
        if (r->type == SUBTITLE_ASS && r->ass)
            ass_plain(r->ass, line, sizeof line);
        else if (r->type == SUBTITLE_TEXT && r->text)
            snprintf(line, sizeof line, "%s", r->text), tags_strip(line);
        else
            continue;
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%s", len && *line ? "\n" : "", line);
    }
    long long start = (av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q) - E.start_us) / 1000 +
                      sub.start_display_time;
    /* how long: the decoder's word, else the packet's, else five seconds */
    long long shown = sub.end_display_time && sub.end_display_time != UINT32_MAX
                          ? (long long)sub.end_display_time - sub.start_display_time
                      : pkt->duration > 0
                          ? av_rescale_q(pkt->duration, st->time_base, (AVRational){ 1, 1000 })
                          : 5000;
    avsubtitle_free(&sub);
    lock();
    if (serial == E.serial) cue_add(start, start + shown, text), notify();
    unlock();
}

/* A transport stream's timestamps can start again midway (two recordings end to end, a
 * broadcast's clock wrapping round). From there on every packet's are moved by what they
 * jumped, as ffmpeg's own tools do, so the decoders and the clock see one run of time (the place
 * then waits at the length the file gave, which knows nothing of it). A jump is ten seconds on,
 * or a tenth back, from where the stream's next packet should be (*next); *off is what has been
 * added so far. */
static void joined(AVPacket *pkt, AVRational tb, long long *next, long long *off) {
    if (next && pkt->dts != AV_NOPTS_VALUE) {
        long long at = av_rescale_q(pkt->dts, tb, AV_TIME_BASE_Q) + *off, by = at - *next;
        if (*next != NONE && (by > E.jump_us || by < -100000)) *off -= by, at -= by;
        *next = at + av_rescale_q(pkt->duration, tb, AV_TIME_BASE_Q);
    }
    long long add = av_rescale_q(*off, AV_TIME_BASE_Q, tb);
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += add;
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += add;
}

static void *reader_main(void *unused) {
    worker_t *w = &E.reader;
    AVCodecContext *sdec = 0;
    AVPacket *pkt = av_packet_alloc();
    int si = -1, aswitch = 0, serial = 0, want_key, keyless = 0;
    long long ahead = NONE, ahead_to = 0; /* a seek ahead's: the place to get past, and its own */
    long long off = 0, vnext = NONE, anext = NONE; /* joined()'s */
    int ok = pkt && media_open();
    lock();
    E.state = ok ? ENG_READY : ENG_FAILED;
    E.vdone = E.vi < 0; /* no picture to wait for the end of */
    want_key = E.vi >= 0 && !E.still;
    kick(), notify();
    while (ok && !E.quit) {
        if (E.seek.pending) { /* to another place: what was read for this one goes */
            int how = E.seek.how;
            long long us = E.start_us + E.seek.ms * 1000, skip = how == SEEK_EXACT ? us : NONE;
            serial = E.serial, E.seek.pending = 0, E.eof = 0;
            pq_flush(&E.vq), pq_flush(&E.aq);
            memset(E.cue, 0, sizeof E.cue);
            unlock();
            /* the keyframe before it; before the first of them, the first */
            if (av_seek_frame(E.ic, -1, us, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(E.ic, -1, us, 0);
            if (sdec) avcodec_flush_buffers(sdec);
            lock();
            /* ahead, that keyframe must be at least half the way on from where it was (seen when
             * it is read): +10 s then lands 5 to 10 s on, or at the first keyframe after */
            ahead = how == SEEK_AHEAD && E.vi >= 0 && !E.still ? (E.seek.from + us) / 2 : NONE;
            ahead_to = us;
            E.rserial = serial, E.skip_us = skip, E.skip_serial = serial;
            want_key = E.vi >= 0 && !E.still, keyless = 0;
            off = 0, vnext = anext = NONE; /* the file's own timestamps again */
            kick();
            continue;
        }
        if (aswitch != E.aswitch) { /* another sound track: its packets from here on */
            aswitch = E.aswitch;
            if (E.ai >= 0) E.ic->streams[E.ai]->discard = AVDISCARD_ALL;
            E.m.audio_cur = E.want_audio, E.ai = E.m.audio[E.want_audio].index;
            E.ic->streams[E.ai]->discard = AVDISCARD_DEFAULT;
            anext = NONE;
            pq_flush(&E.aq);
            if (E.eof) pq_put(&E.aq, 0, serial); /* nothing more is read: it ends at once */
            E.adone = 0, E.aswitched = aswitch;
            kick();
            continue;
        }
        /* ponytail: likewise the file's own subtitles turned on midway show from the reader's
         * place on: a line already read past is not shown */
        if (si != E.want_sub) { /* other subtitles, or none */
            if (si >= 0) E.ic->streams[si]->discard = AVDISCARD_ALL;
            si = E.want_sub;
            memset(E.cue, 0, sizeof E.cue);
            unlock();
            avcodec_free_context(&sdec);
            if (si >= 0) {
                AVStream *st = E.ic->streams[si];
                AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
                if ((sdec = avcodec_alloc_context3(c))) {
                    avcodec_parameters_to_context(sdec, st->codecpar);
                    sdec->pkt_timebase = st->time_base;
                    if (avcodec_open2(sdec, c, 0) < 0) avcodec_free_context(&sdec);
                }
                st->discard = AVDISCARD_DEFAULT;
            }
            lock();
            continue;
        }
        if (E.eof || capped()) {
            wait_for(w, 0);
            continue;
        }
        unlock();
        int r = av_read_frame(E.ic, pkt);
        lock();
        if (r == AVERROR(EAGAIN)) continue;
        if (r < 0) { /* the end, or a tail that can't be read: the same */
            E.eof = 1;
            if (E.vi >= 0) pq_put(&E.vq, 0, serial);
            if (E.ai >= 0) pq_put(&E.aq, 0, serial);
            kick(), notify();
            continue;
        }
        if (E.ic->iformat->flags & AVFMT_TS_DISCONT)
            joined(pkt, E.ic->streams[pkt->stream_index]->time_base,
                   pkt->stream_index == E.vi   ? &vnext
                   : pkt->stream_index == E.ai ? &anext
                                               : 0,
                   &off);
        if (pkt->stream_index == E.vi) {
            /* after a seek the picture starts at a keyframe; a stream that marks none gets going
             * all the same */
            if (want_key && !(pkt->flags & AV_PKT_FLAG_KEY) && ++keyless < 300) {
                av_packet_unref(pkt);
                continue;
            }
            long long at = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            if (ahead != NONE && at != AV_NOPTS_VALUE &&
                av_rescale_q(at, E.ic->streams[E.vi]->time_base, AV_TIME_BASE_Q) <= ahead) {
                /* A seek ahead came out under half the way on, keyframes being far apart: the
                 * keyframe after the place, then, so that +10 s always moves on; and where there
                 * is none after, this one again with all before the place decoded and dropped. */
                ahead = NONE, keyless = 0;
                av_packet_unref(pkt);
                unlock();
                int exact = av_seek_frame(E.ic, -1, ahead_to, 0) < 0;
                if (exact) av_seek_frame(E.ic, -1, ahead_to, AVSEEK_FLAG_BACKWARD);
                lock();
                if (exact) E.skip_us = ahead_to, E.skip_serial = serial;
                continue;
            }
            ahead = NONE, want_key = 0;
            pq_put(&E.vq, pkt, serial), kick();
        } else if (ahead != NONE) /* nothing is taken before the picture's keyframe is known */
            av_packet_unref(pkt);
        else if (pkt->stream_index == E.ai)
            pq_put(&E.aq, pkt, serial), kick();
        else if (pkt->stream_index == si && sdec) {
            unlock();
            subtitle(sdec, E.ic->streams[si], pkt, serial);
            av_packet_unref(pkt);
            lock();
        } else
            av_packet_unref(pkt);
    }
    w->on = 0;
    pthread_cond_signal(&E.idle);
    unlock();
    avcodec_free_context(&sdec);
    av_packet_free(&pkt);
    return 0;
}

/* ---- The video thread ---------------------------------------------------------------------- */

static void *video_main(void *unused) {
    worker_t *w = &E.video;
    AVFrame *f = av_frame_alloc();
    struct SwsContext *sws = 0;
    pix *img = malloc(PAGE_PX * sizeof *img); /* a scaled picture, before its turn */
    geom_t g = { 0 };
    ladder_t lad = { 0 };
    int serial = -1, first = 1, count = 0, fw = 0, fh = 0, ffmt = -1, ffill = -1, laddered = 0;
    long long last = NONE;
    lock();
    while (!E.quit && f && img) {
        if (E.state != ENG_READY || E.vi < 0 || !E.vq.head) {
            wait_for(w, 0);
            continue;
        }
        node_t *n = pq_get(&E.vq);
        int now_serial = E.serial, end = !n->pkt;
        kick(); /* room for the reader */
        unlock();
        if (n->serial != now_serial) { /* read before a seek */
            node_free(n);
            lock();
            continue;
        }
        if (!laddered) ladder_init(&lad, E.m.w, E.m.h, now_ms()), laddered = 1;
        if (n->serial != serial) { /* the first of another place */
            avcodec_flush_buffers(E.vdec);
            serial = n->serial, first = 1, count = 0, last = NONE, lad.late = 0;
        }
        E.vdec->skip_loop_filter = lad.level >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
        E.vdec->skip_frame = lad.level >= 3   ? AVDISCARD_NONKEY
                             : lad.level == 2 ? AVDISCARD_NONREF
                                              : AVDISCARD_DEFAULT;
        avcodec_send_packet(E.vdec, n->pkt); /* a packet it can't take is a picture missing */
        node_free(n);
        int go = 1;
        while (go && avcodec_receive_frame(E.vdec, f) >= 0) {
            AVRational tb = E.ic->streams[E.vi]->time_base;
            long long pts = f->best_effort_timestamp != AV_NOPTS_VALUE
                                ? av_rescale_q(f->best_effort_timestamp, tb, AV_TIME_BASE_Q)
                            : last != NONE ? last
                                           : E.start_us;
            long long dur = f->pkt_duration > 0 ? av_rescale_q(f->pkt_duration, tb, AV_TIME_BASE_Q)
                                                : E.frame_us,
                      c = 0;
            last = pts + dur;
            lock();
            long long now = now_ms(), skip = E.skip_serial == serial ? E.skip_us : NONE;
            int timed = clock_read(now, &c) && !E.paused, fill_now = E.fill;
            go = serial == E.serial && !E.quit;
            unlock();
            if (!go || pts + dur <= skip) { /* a seek since, or before the place asked for */
                av_frame_unref(f);
                continue;
            }
            /* Late by more than its own time on the screen: dropped as it is, unscaled. Never
             * a place's first picture, and its first few say nothing of the Q2's speed. */
            int late = !first && timed && c > pts + dur && c - pts < E.jump_us;
            int level = ladder_frame(&lad, now, ++count > 8 && late, f->key_frame);
            if (late) {
                av_frame_unref(f);
                continue;
            }
            lock();
            E.level = level;
            while (E.count == PICS && serial == E.serial && !E.quit) wait_for(w, 0);
            go = serial == E.serial && !E.quit;
            pic_t *p = &E.pic[(E.ridx + E.count) % PICS]; /* not the main thread's until counted */
            unlock();
            if (!go) {
                av_frame_unref(f);
                continue;
            }
            if (!sws || f->width != fw || f->height != fh || f->format != ffmt ||
                fill_now != ffill) {
                AVRational sar = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : E.sar;
                fw = f->width, fh = f->height, ffmt = f->format, ffill = fill_now;
                geometry(fw, fh, sar.num, sar.den, E.rot, ffill, &g);
                sws = sws_getCachedContext(sws, g.sw, g.sh, ffmt, g.dw, g.dh, AV_PIX_FMT_BGRA,
                                           SWS_FAST_BILINEAR, 0, 0, 0);
                /* the colours as the file says they are coded, or as its size suggests: HD is
                 * BT.709, the rest BT.601 */
                int hd = f->colorspace == AVCOL_SPC_BT709 ||
                         (f->colorspace == AVCOL_SPC_UNSPECIFIED && fh >= 720);
                if (sws)
                    sws_setColorspaceDetails(
                        sws, sws_getCoefficients(hd ? SWS_CS_ITU709 : SWS_CS_ITU601),
                        f->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                        0, 1 << 16, 1 << 16);
            }
            const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(ffmt);
            if (sws && d) {
                /* the part shown: each plane from its own corner of it */
                const uint8_t *src[4] = { 0 };
                int step[4] = { 0 };
                av_image_fill_max_pixsteps(step, 0, d);
                for (int i = 0; i < 4 && f->data[i]; ++i) {
                    int chroma = (i == 1 || i == 2) && !(d->flags & AV_PIX_FMT_FLAG_RGB);
                    src[i] = f->data[i];
                    if (i && d->flags & AV_PIX_FMT_FLAG_PAL) continue; /* a palette isn't pixels */
                    src[i] += (g.sy >> (chroma ? d->log2_chroma_h : 0)) * f->linesize[i] +
                              (g.sx >> (chroma ? d->log2_chroma_w : 0)) * step[i];
                }
                uint8_t *dst[4] = { (uint8_t *)img };
                int line[4] = { g.dw * 4 };
                sws_scale(sws, src, f->linesize, 0, g.sh, dst, line);
                picture_place(p->px, img, g.dw, &g);
            }
            av_frame_unref(f);
            lock();
            if (serial == E.serial && sws && d) {
                p->pts = pts, p->dur = dur, p->serial = serial, p->g = g;
                ++E.count;
                if (first) E.first_serial = serial, E.first_us = pts, first = 0;
                kick(), notify();
            }
            unlock();
        }
        lock();
        if (end && serial == E.serial) E.vdone = 1, kick(), notify();
    }
    w->on = 0;
    pthread_cond_signal(&E.idle);
    unlock();
    sws_freeContext(sws);
    av_frame_free(&f);
    free(img);
    return 0;
}

/* ---- The output ---------------------------------------------------------------------------- */

/* The silent output: frames go into a ring that empties at RATE by the clock, and a write waits
 * for room as ALSA's does. So it keeps time, the script's too. */
static long null_write(au_t *a, long n) {
    lock();
    while (E.fault == FAULT_STALL && a->device && !E.quit && !a->abandoned) wait_for(&a->w, 0);
    if ((E.fault == FAULT_LOST || E.fault == FAULT_GONE) && a->device) return unlock(), -1;
    while (!E.quit && !a->abandoned) {
        /* in 64 bits: the Q2's long is 32, and a clock 12 hours from boot times 48 is past it */
        long long now = now_ms(), played = (now - a->n0) * (RATE / 1000);
        if (played >= a->nw) a->n0 = now, a->nw = 0, played = 0; /* it ran dry: from now */
        if (a->nw - played + n <= RING) break;
        wait_for(&a->w, a->n0 + (a->nw + n - RING + RATE / 1000 - 1) / (RATE / 1000));
    }
    a->nw += n;
    unlock();
    return n;
}

static long out_unheard(au_t *a) {
#ifndef HOST
    if (a->alsa) return alsa_unheard(a->written);
#endif
    long long left = a->nw - (now_ms() - a->n0) * (RATE / 1000);
    return left < 0 ? 0 : left > a->written ? (long)a->written : (long)left;
}

static void out_drop(au_t *a) {
#ifndef HOST
    if (a->alsa) alsa_drop();
#endif
    a->nw = 0;
}

/* the lock held: the clock as it stands after a write, or (held) stopped at what was heard */
static void publish(au_t *a, long unheard, int live) {
    if (a->serial != E.serial || !a->tl) return;
    int was = E.clk.serial == E.serial;
    E.clk.end = a->base + a->frames * 1000000 / RATE;
    E.clk.heard = E.clk.end - (long long)unheard * 1000000 / RATE;
    E.clk.live = live && unheard < a->written; /* nothing is heard until the output plays some */
    E.clk.stamp = now_ms(), E.clk.serial = a->serial;
    if (!was) notify();
}

/* Q2 Pod's switch files, an int each, 1 when on: mapped once they exist. */
static volatile int *flag_map(const char *name) {
    char path[PATH_N];
    snprintf(path, sizeof path, "%s/%s", tmp_dir, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    void *p = mmap(0, sizeof(int), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? 0 : p;
}

/* n frames (CHUNK at most) to the output: src's, or silence. Comes back with the lock held; 0
 * when the thread is to end. */
static int emit(au_t *a, const short *src, int n) {
    static volatile int *mono_on, *swap_on; /* looked for now and then until they appear */
    static unsigned tries;
    short buf[CHUNK * 2];
    if (!(tries++ % 100)) {
        if (!mono_on) mono_on = flag_map("q2mono");
        if (!swap_on) swap_on = flag_map("q2swap");
    }
    memset(buf, 0, sizeof buf);
    if (src) {
        int g = E.gain;
        memcpy(buf, src, (size_t)n * 4);
        if (g < 65536) scale(buf, 2 * (unsigned)n, g);
        if (mono_on && *mono_on > 0) mono(buf, (unsigned)n);
        if (swap_on && *swap_on > 0) swap_lr(buf, (unsigned)n);
    }
    a->write_at = (unsigned)now_ms() | 1;
#ifndef HOST
    long r = a->alsa ? alsa_write(buf, n) : null_write(a, n);
#else
    long r = null_write(a, n);
#endif
    a->write_at = 0;
    long unheard = 0;
    if (r >= 0) a->written += n, unheard = out_unheard(a);
    lock();
    if (a->abandoned || E.quit) return 0;
    if (r < 0) { /* the output is gone: nothing more until the main thread says */
        unlock();
#ifndef HOST
        if (a->alsa) alsa_close();
#endif
        lock();
        a->alsa = 0, a->lost = 1, a->written = 0, a->nw = 0;
        E.lost = 1;
        publish(a, 0, 0);
        notify();
        return 1;
    }
    a->frames += n;
    for (int i = 0; i < n; ++i) { /* kept as they came, to be written again after a pause */
        a->hist[2 * a->hist_at] = src ? src[2 * i] : 0,
                    a->hist[2 * a->hist_at + 1] = src ? src[2 * i + 1] : 0;
        a->hist_at = (a->hist_at + 1) % HIST;
    }
    a->hist_n = a->hist_n + n > HIST ? HIST : a->hist_n + n;
    if (src) E.frames += n;
    if (src && E.checked) { /* the checks': the pitch of what went out */
        for (int i = 0; i < n; ++i) {
            int up = src[2 * i] >= 0;
            a->zc += up != a->zlast, a->zlast = up;
        }
        if ((a->zn += n) >= RATE / 10) E.tone = a->zc * 10 * (RATE / 10) / a->zn, a->zc = a->zn = 0;
    }
    publish(a, unheard, 1);
    return 1;
}

/* A pause: what the output holds unheard is dropped, to be written again, so the sound stops
 * with the picture and nothing of it is lost. The lock held. */
static void hold(au_t *a) {
    a->held = 1;
    if (a->written) {
        unlock();
        long un = out_unheard(a);
        out_drop(a);
        lock();
        if (un > a->hist_n) un = a->hist_n;
        /* before what was already waiting to be written again */
        if (a->rep_n + un > HIST * 2) a->rep_n = (int)(HIST * 2 - un);
        memmove(a->rep + 2 * un, a->rep + 2 * a->rep_at, (size_t)a->rep_n * 4);
        a->hist_at = (int)((a->hist_at - un + HIST) % HIST);
        for (long i = 0; i < un; ++i)
            memcpy(a->rep + 2 * i, a->hist + 2 * ((a->hist_at + i) % HIST), 4);
        a->rep_at = 0, a->rep_n += (int)un, a->hist_n -= (int)un;
        a->frames -= un, a->written = 0;
    }
    publish(a, 0, 0);
    notify();
}

static AVCodecContext *audio_decoder(int stream) {
    AVStream *st = E.ic->streams[stream];
    AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *dec = avcodec_alloc_context3(c);
    if (!dec) return 0;
    avcodec_parameters_to_context(dec, st->codecpar);
    dec->pkt_timebase = st->time_base;
    if (avcodec_open2(dec, c, 0) < 0) avcodec_free_context(&dec);
    return dec;
}

/* A decoded frame into pend, as 48 kHz stereo, and onto the timeline: where its time says it
 * belongs against where the last one ended. The lock not held. */
static void take_frame(au_t *a, AVFrame *f, int stream) {
    uint64_t in =
        f->channel_layout && av_get_channel_layout_nb_channels(f->channel_layout) == f->channels
            ? f->channel_layout
            : (uint64_t)av_get_default_channel_layout(f->channels);
    if (!a->swr || a->fmt != f->format || a->rate != f->sample_rate || a->layout != in) {
        a->swr = swr_alloc_set_opts(a->swr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, RATE,
                                    (int64_t)in, f->format, f->sample_rate, 0, 0);
        if (!a->swr || swr_init(a->swr) < 0) {
            swr_free(&a->swr);
            return;
        }
        a->fmt = f->format, a->rate = f->sample_rate, a->layout = in;
    }
    int room = swr_get_out_samples(a->swr, f->nb_samples);
    if (room <= 0) return;
    av_fast_malloc(&a->pend, &a->pend_room, (size_t)room * 4);
    if (!a->pend) return;
    uint8_t *out[1] = { (uint8_t *)a->pend };
    int n = swr_convert(a->swr, out, room, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n <= 0) return;

    long long len = (long long)n * 1000000 / RATE;
    long long at = f->best_effort_timestamp != AV_NOPTS_VALUE
                       ? av_rescale_q(f->best_effort_timestamp, E.ic->streams[stream]->time_base,
                                      AV_TIME_BASE_Q)
                       : NONE;
    lock();
    long long skip = E.skip_serial == a->serial ? E.skip_us : NONE;
    int current = a->serial == E.serial;
    unlock();
    if (!current || (at != NONE && at + len <= skip)) return; /* before the place asked for */
    if (a->tl == 2) { /* the timeline begins with this frame */
        if (at != NONE) a->base = at;
        a->frames = 0, a->tl = 1;
    }
    if (at != NONE) {
        long long d = at - (a->base + a->frames * 1000000 / RATE);
        if (d > TOL && d <= E.jump_us) /* a gap, or a sound that starts after the picture */
            a->pad = d * RATE / 1000000;
        else if (d < -TOL && -d <= (a->frames ? OVERLAP_MAX : E.jump_us)) {
            /* before its place (after a seek, before the picture it starts with): that much goes */
            long long cut = -d * RATE / 1000000;
            if (cut >= n) return;
            a->pend_at = (int)cut, n -= (int)cut;
        } else if (d > TOL || d < -TOL)
            a->base = at, a->frames = 0; /* the timestamps jump: so does the clock */
        else
            a->base +=
                d / 8; /* a little off: lean towards it, so the clock keeps to the track's times */
    }
    a->pend_n = n;
}

/* The lock held: no picture is coming that the sound could start with. */
static int no_picture(void) {
    return E.vi < 0 || E.still || E.vdone || (capped() && !E.vq.head && !E.video.busy);
}

static void *audio_main(void *arg) {
    au_t *a = arg;
    AVCodecContext *dec = 0;
    AVFrame *f = av_frame_alloc();
    int stream = -1, open = 1;
#ifndef HOST
    if (a->device && *E.device) /* here, on the thread every call to it is made from */
        open = a->alsa = alsa_open(E.device, 1, &E.quit);
#endif
    lock();
    if (a->device && (!open || E.fault == FAULT_GONE)) a->device = 0, E.silent = 1, notify();
    while (!E.quit && !a->abandoned && f) {
        if (E.state != ENG_READY) {
            wait_for(&a->w, 0);
            continue;
        }
        if (a->serial != E.serial) { /* a seek: all of the old place goes */
            a->serial = E.serial;
            unlock();
            if (dec) avcodec_flush_buffers(dec);
            if (a->written) out_drop(a);
            lock();
            a->written = 0, a->tl = 0, a->frames = 0, a->pad = 0, a->start = NONE;
            a->pend_n = a->pend_at = a->rep_n = a->hist_n = a->fed = a->drained = 0;
            a->fmt = -1; /* and what the resampler held */
            continue;
        }
        if (!a->opened || a->track != E.aswitched) {
            /* The sound track's decoder, at the start and for another track: the timeline stays,
             * and the wait for the new track's first frame is silence.
             * ponytail: the new track's sound begins where the reader is, some two seconds on
             * from what is heard, so a change of track is that long a silence under a picture
             * that plays on; to hear it at once, seek to the place (the picture then waits for
             * its keyframe's run to be decoded again). */
            a->opened = 1, a->track = E.aswitched, stream = E.ai;
            unlock();
            avcodec_free_context(&dec);
            dec = stream >= 0 ? audio_decoder(stream) : 0;
            lock();
            a->pend_n = a->pend_at = a->fed = a->drained = 0, a->fmt = -1;
            continue;
        }
        if (a->lost) { /* until the main thread asks for the output again */
            if (!E.retry) {
                wait_for(&a->w, 0);
                continue;
            }
            E.retry = 0;
            int ok = E.fault == FAULT_NONE;
#ifndef HOST
            if (*E.device) {
                unlock();
                ok = a->alsa = alsa_open(E.device, 0, &E.quit);
                lock();
            }
#endif
            a->device = ok, a->lost = 0, E.lost = 0, E.silent = !ok;
            notify();
            continue;
        }
        if (E.paused) {
            if (!a->held) hold(a);
            wait_for(&a->w, 0);
            continue;
        }
        a->held = 0;
        if (!a->tl) {
            /* The sound starts with the place's first picture, so neither runs ahead (from the
             * very start, with the file itself, should its picture begin later): wait for it,
             * unless none is coming. Then it starts with its own first frame. */
            if (a->start != NONE)
                a->base = a->start, a->start = NONE, a->tl = 1;
            else if (E.seek.pending || E.rserial != a->serial) { /* the reader isn't there yet */
                wait_for(&a->w, 0);
                continue;
            } else if (E.first_serial == a->serial)
                a->base = !E.target_ms && E.start_us < E.first_us ? E.start_us : E.first_us,
                a->tl = 1;
            else if (!no_picture()) {
                wait_for(&a->w, 0);
                continue;
            } else
                a->base = E.start_us + E.target_ms * 1000, a->tl = dec ? 2 : 1;
            a->frames = 0;
        }
        if (a->rep_n) { /* what a pause left unheard */
            int n = a->rep_n < CHUNK ? a->rep_n : CHUNK;
            const short *s = a->rep + 2 * a->rep_at;
            a->rep_at += n, a->rep_n -= n;
            unlock();
            if (!emit(a, s, n)) break;
        } else if (a->pad) {
            int n = a->pad < CHUNK ? (int)a->pad : CHUNK;
            a->pad -= n;
            unlock();
            if (!emit(a, 0, n)) break;
        } else if (a->pend_n) {
            int n = a->pend_n < CHUNK ? a->pend_n : CHUNK;
            const short *s = a->pend + 2 * a->pend_at;
            a->pend_at += n, a->pend_n -= n;
            unlock();
            if (!emit(a, s, n)) break;
        } else if (dec && a->fed) { /* the next frame of the packet it has */
            unlock();
            a->pend_at = 0;
            int r = avcodec_receive_frame(dec, f);
            if (r >= 0) take_frame(a, f, stream), av_frame_unref(f);
            lock();
            if (r < 0) a->fed = 0;
            if (r == AVERROR_EOF) a->drained = 1;
        } else if (dec && !a->drained && E.aq.head) {
            node_t *n = pq_get(&E.aq);
            int current = n->serial == E.serial;
            kick(); /* room for the reader */
            unlock();
            if (current) avcodec_send_packet(dec, n->pkt), a->fed = 1;
            node_free(n);
            lock();
        } else if (!dec || a->drained || E.eof || capped()) {
            /* Nothing to play and nothing coming: silence, so the clock runs on for the picture.
             * The track is over when it is: its end is where the timeline stands. */
            if ((!dec || a->drained) && !E.adone && a->serial == E.serial)
                E.adone = 1, E.aend_us = a->tl == 1 ? a->base + a->frames * 1000000 / RATE : NONE,
                notify();
            a->tl = 1;
            unlock();
            if (!emit(a, 0, CHUNK)) break;
        } else
            wait_for(&a->w, 0);
    }
    a->w.on = 0;
    pthread_cond_signal(&E.idle);
    int ended = !a->abandoned; /* abandoned: the output is another's now, or nobody's */
    unlock();
#ifndef HOST
    if (ended && a->alsa) alsa_close();
#endif
    if (ended) {
        avcodec_free_context(&dec);
        swr_free(&a->swr);
        av_frame_free(&f);
        av_free(a->pend);
    }
    return 0;
}

/* ---- The main thread's side ---------------------------------------------------------------- */

static pthread_t spawn(void *(*run)(void *), void *arg) {
    pthread_t th;
    sigset_t set, old; /* signals are the main thread's: these never see one */
    sigemptyset(&set), sigaddset(&set, SIGTERM), sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    if (pthread_create(&th, 0, run, arg)) abort(); /* no thread, no player */
    pthread_sigmask(SIG_SETMASK, &old, 0);
    return th;
}

static au_t *audio_start(int device, long long start) {
    au_t *a = calloc(1, sizeof *a);
    if (!a) abort();
    a->w.on = a->w.busy = 1, a->device = device, a->start = start, a->serial = E.serial;
    E.au = a;
    a->w.th = spawn(audio_main, a);
    return a;
}

void eng_open(const char *path, long long start_ms, const char *device, int gain, int audio,
              int sub, int fill) {
    static int made;
    if (!made) {
        pthread_condattr_t attr; /* timed waits on the clock now_ms reads */
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&E.cond, &attr), pthread_cond_init(&E.idle, &attr);
        made = 1;
    }
    lock();
    snprintf(E.path, sizeof E.path, "%s", path);
    snprintf(E.device, sizeof E.device, "%s", device ? device : "");
    E.gain = gain, E.fill = fill, E.want_audio = audio, E.want_sub = -1, E.sub_nth = sub;
    E.state = ENG_OPENING, E.vi = E.ai = -1;
    E.first_serial = E.shown_serial = E.clk.serial = E.skip_serial = -1;
    E.target_ms = start_ms;
    if (start_ms > 0) E.seek.pending = 1, E.seek.how = SEEK_BACK, E.seek.ms = start_ms;
    for (int i = 0; i < PICS; ++i)
        if (!(E.pic[i].px = malloc(PAGE_PX * sizeof(pix)))) abort();
    E.on = 1;
    E.reader.on = E.reader.busy = E.video.on = E.video.busy = 1;
    E.reader.th = spawn(reader_main, 0);
    E.video.th = spawn(video_main, 0);
    audio_start(1, NONE);
    unlock();
}

const char *eng_why(void) { return E.why; }
const media_t *eng_media(void) { return &E.m; }

void eng_poll(snap_t *s, pix **cur) {
    lock();
    long long now = now_ms(), c = 0;
    au_t *a = E.au;
    memset(s, 0, sizeof *s);
    E.notify = 0;
    /* An output that has held one write for three seconds takes no sound: its thread is left
     * in it for good, and another plays on from here into the silent one. */
    if (a && a->write_at && (int)((unsigned)now - a->write_at) >= STALL_MS && !a->abandoned) {
        int ok = clock_read(now, &c);
        a->abandoned = 1, a->w.on = 0;
        E.stuck = E.silent = 1;
        audio_start(0, ok ? c : NONE);
        kick();
    }
    int ok = clock_read(now, &c);
    s->due_ms = -1;
    while (E.count) {
        pic_t *p = &E.pic[E.ridx];
        /* a place's first picture goes up at once, paused or not; those after it, in their time */
        if (E.shown_serial == E.serial) {
            if (E.paused || !ok) break;
            /* ponytail: a file that is no transport stream (joined() carries those), has no
             * sound to move the clock, and whose timestamps jump an hour or more waits that hour
             * here; follow the picture's time if such a file ever shows. */
            long long early = p->pts - c;
            if (early > 0) {
                s->due_ms = (early + 999) / 1000;
                break;
            }
            if (E.count > 1 && c >= E.pic[(E.ridx + 1) % PICS].pts && c - p->pts < E.jump_us) {
                E.ridx = (E.ridx + 1) % PICS, --E.count; /* the one after is due too */
                continue;
            }
        }
        pix *mine = *cur;
        *cur = p->px, p->px = mine;
        E.shown_serial = p->serial, E.shown_us = p->pts, E.shown_dur = p->dur, E.geom = p->g;
        E.ridx = (E.ridx + 1) % PICS, --E.count;
        E.picture = s->fresh = 1;
    }
    if (s->fresh) kick(); /* room for the video thread */
    int shown = E.shown_serial == E.serial;
    long long pos = ok      ? (c - E.start_us) / 1000
                    : shown ? (E.shown_us - E.start_us) / 1000
                            : E.target_ms;
    s->pos_ms = pos < 0 ? 0 : E.m.dur_ms && pos > E.m.dur_ms ? E.m.dur_ms : pos;
    s->pic_ms = shown ? (E.shown_us - E.start_us) / 1000 : -1;
    s->state = E.state, s->picture = E.picture, s->serial = E.serial, s->geom = E.geom;
    s->lost = E.lost, s->silent = E.silent, s->frames = E.frames, s->tone = E.tone;
    s->level = E.level, s->queued = E.vq.bytes + E.aq.bytes;
    /* the end: nothing left to read, decode, show or hear */
    s->ended = E.state == ENG_READY && E.eof && E.vdone && E.adone && !E.count && !E.seek.pending &&
               E.rserial == E.serial && ok && (E.aend_us == NONE || c >= E.aend_us) &&
               (!shown || c >= E.shown_us + E.shown_dur);
    if (E.state == ENG_READY && !s->ended && !E.paused && ok && s->due_ms < 0 && E.eof && E.vdone &&
        E.adone && !E.count) {
        /* only the last of it left to hear or to stay on the screen: look again when it has */
        long long left = E.aend_us != NONE && c < E.aend_us ? E.aend_us - c : 0;
        if (shown && E.shown_us + E.shown_dur - c > left) left = E.shown_us + E.shown_dur - c;
        s->due_ms = (left + 999) / 1000;
    }
    unlock();
}

void eng_seek(long long ms, int how) {
    lock();
    if (ms < 0) ms = 0;
    /* where it is: the clock's time, a paused seek's picture, or where a seek not yet there goes */
    if (!clock_read(now_ms(), &E.seek.from))
        E.seek.from = E.shown_serial == E.serial ? E.shown_us : E.start_us + E.target_ms * 1000;
    ++E.serial;
    E.seek.pending = 1, E.seek.how = how, E.seek.ms = ms;
    E.target_ms = ms;
    E.count = 0; /* the pictures waiting are of the old place */
    E.vdone = E.vi < 0, E.adone = 0;
    kick();
    unlock();
}

void eng_pause(int on) {
    lock();
    E.paused = on;
    kick();
    unlock();
}

void eng_audio(int nth) {
    lock();
    if (E.state == ENG_READY && nth >= 0 && nth < E.m.naudio && E.m.audio[nth].index != E.ai)
        E.want_audio = nth, ++E.aswitch, kick();
    unlock();
}

void eng_subs(int stream) {
    lock();
    E.want_sub = stream;
    kick();
    unlock();
}

void eng_fill(int on) {
    lock();
    E.fill = on;
    unlock();
}

void eng_gain(int gain) { E.gain = gain; }

void eng_output(void) {
    lock();
    E.retry = 1;
    kick();
    unlock();
}

int eng_cues(long long ms, char buf[][LINE_N], int max, long long *next) {
    int n = 0;
    long long change = -1;
    lock();
    for (int i = 0; i < ECUES; ++i) {
        long long edge = !E.cue[i].end         ? -1
                         : ms < E.cue[i].start ? E.cue[i].start
                         : ms < E.cue[i].end   ? E.cue[i].end
                                               : -1;
        if (edge >= 0 && (change < 0 || edge < change)) change = edge;
        if (E.cue[i].end && ms >= E.cue[i].start && ms < E.cue[i].end && n < max)
            strcpy(buf[n++], E.cue[i].text);
    }
    unlock();
    if (next) *next = change;
    return n;
}

void eng_test(unsigned mem_kb, const char *sink) {
    lock();
    E.checked = 1;
    if (mem_kb) E.mem_kb = mem_kb;
    if (sink)
        E.fault = !strcmp(sink, "stall")  ? FAULT_STALL
                  : !strcmp(sink, "lost") ? FAULT_LOST
                  : !strcmp(sink, "gone") ? FAULT_GONE
                                          : FAULT_NONE;
    kick();
    unlock();
}

int eng_close(void) {
    if (!E.on) return 1;
    lock();
    E.quit = 1;
    kick();
    au_t *a = E.au;
    int whole = !E.stuck;
    unlock();
    struct timespec
        by; /* a second for all of them: pthread_timedjoin_np counts on the wall clock */
    clock_gettime(CLOCK_REALTIME, &by);
    by.tv_sec += 1;
    if (pthread_timedjoin_np(E.reader.th, 0, &by)) whole = 0;
    if (pthread_timedjoin_np(E.video.th, 0, &by)) whole = 0;
    if (pthread_timedjoin_np(a->w.th, 0, &by)) whole = 0;
    if (!whole) return 0; /* a thread is still out there with all of this */
    pq_flush(&E.vq), pq_flush(&E.aq);
    avcodec_free_context(&E.vdec);
    if (E.ic) avformat_close_input(&E.ic);
    for (int i = 0; i < PICS; ++i) free(E.pic[i].px);
    free(a);
    /* as new, for the next video: all but the lock, the conditions and the checks' settings */
    pthread_mutex_t m = E.lock;
    pthread_cond_t c = E.cond, i = E.idle;
    unsigned mem = E.mem_kb;
    int fault = E.fault, checked = E.checked;
    memset(&E, 0, sizeof E);
    E.lock = m, E.cond = c, E.idle = i, E.mem_kb = mem, E.fault = fault, E.checked = checked;
    return 1;
}
