/* The worker: it probes each video (its length, its picture's size) and makes its thumbnail,
 * into the cache on the card, state_dir/cache: <key16>.jpg and the index. It is a process of its
 * own, the server's child, not a thread: the Q2's FFmpeg is 4.2.2, and a file that crashes one of
 * its decoders, or has the kernel kill what decodes it, then takes the worker alone; the server
 * notes which video it was on, answers on, and starts another worker past it. */
#include "serve.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

/* The containers a video's extension names (is_video), as libavformat calls their readers. A
 * file is opened as one of these or not at all: a playlist or a script named .mp4 would have
 * libavformat open whatever it lists. */
#define FORMATS "mov,matroska,avi,mpegts,mpeg,flv,asf,ogg,mpegvideo,m4v"
#define SIDE_MAX 16384              /* a picture's width or height at most: more is no video */
#define DUR_MAX (10000ll * 3600000) /* a length past 10,000 hours is no length */
#define PACKETS 1000                /* read for one picture at most, should none come */
#define ALLOC_MAX (32 << 20) /* one allocation of FFmpeg's at most: a 4K frame's plane is 8 MB */
#define DARK 20 /* a picture whose mean brightness is under this, of 255, is near black */
#define HEX "0123456789abcdef"

/* ---- The index ----------------------------------------------------------------------------- */

/* ponytail: the lines are looked through one by one, a row's and a video's each time; sort them
 * and bsearch if a library of tens of thousands of videos ever makes a list slow. */
static struct {
    thumb_t *v;
    int n, room;
    int dirty; /* it has changed since it was loaded or saved */
} ix;

static const char *cache_file(const char *name, char *path) {
    snprintf(path, PATH_N, "%s/cache/%s", state_dir, name);
    return path;
}

const char *thumb_file(unsigned long long key, char *path) {
    snprintf(path, PATH_N, "%s/cache/%016llx.jpg", state_dir, key);
    return path;
}

void index_free(void) {
    free(ix.v);
    memset(&ix, 0, sizeof ix);
}

const thumb_t *index_find(unsigned long long key) {
    for (int i = 0; i < ix.n; ++i)
        if (ix.v[i].key == key) return &ix.v[i];
    return 0;
}

void index_put(const thumb_t *t) {
    thumb_t *at = (thumb_t *)index_find(t->key);
    if (!at) {
        if (ix.n == ix.room) {
            int room = ix.room ? ix.room * 2 : 64;
            thumb_t *v = realloc(ix.v, (size_t)room * sizeof *v);
            if (!v) return; /* not noted: the video is probed again another time */
            ix.v = v, ix.room = room;
        }
        at = &ix.v[ix.n++];
    }
    *at = *t;
    ix.dirty = 1;
}

/* The decimal number at *s, at most max and followed by the byte after, into *v, *s moved past
 * both. 0 when that is not what is there. */
static int number(const char **s, char after, long long max, long long *v) {
    char *end;
    if (**s < '0' || **s > '9') return 0; /* strtoll would take blanks and a sign */
    errno = 0;
    *v = strtoll(*s, &end, 10);
    if (errno || *v > max || *end != after) return 0;
    *s = end + 1;
    return 1;
}

void index_load(void) {
    char path[PATH_N], *text = file_get(cache_file("index", path), 0), *end;
    index_free();
    /* only a line that ends is taken, as the marks': a torn one could pass for another */
    for (char *line = text; line && (end = strchr(line, '\n')); line = end + 1) {
        long long dur, w, h;
        *end = 0;
        if (strspn(line, HEX) != 16 || line[16] != '\t') continue;
        const char *s = line + 17;
        if (number(&s, '\t', DUR_MAX, &dur) && number(&s, 'x', SIDE_MAX, &w) &&
            number(&s, '\t', SIDE_MAX, &h) && (*s == T_PICTURE || *s == T_NONE) && !s[1])
            index_put(&(thumb_t){ strtoull(line, 0, 16), dur, (int)w, (int)h, *s });
    }
    free(text);
    ix.dirty = 0;
}

/* The cache's folder made, on a card that never ran the app. */
static void cache_dir(void) {
    char path[PATH_N];
    mkdir(state_dir, 0755);
    mkdir(cache_file("", path), 0755);
}

static int index_save(void) {
    char path[PATH_N], *text = 0;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    if (!f) return 0;
    for (int i = 0; i < ix.n; ++i)
        fprintf(f, "%016llx\t%lld\t%dx%d\t%c\n", ix.v[i].key, ix.v[i].dur_ms, ix.v[i].w, ix.v[i].h,
                ix.v[i].state);
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    ok = ok && file_put(cache_file("index", path), text, len);
    free(text);
    if (ok) ix.dirty = 0;
    return ok;
}

/* ---- A video's thumbnail ------------------------------------------------------------------- */

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}

/* libavformat asks this wherever it reads or waits: 1 once the video's time is up, and it lets
 * go with an error. */
static int late(void *end) { return now_ms() > *(long long *)end; }

/* The next picture the decoder gives from where the file stands, into frame. 1 when it gave one.
 * A packet the decoder refuses is passed over: the one after may be whole. */
static int picture(AVFormatContext *f, AVCodecContext *c, int stream, AVPacket *pkt, AVFrame *frame,
                   long long *end) {
    int got = 0;
    for (int n = 0; !got && n < PACKETS && !late(end) && av_read_frame(f, pkt) >= 0; ++n) {
        if (pkt->stream_index == stream && avcodec_send_packet(c, pkt) >= 0)
            got = avcodec_receive_frame(c, frame) >= 0;
        av_packet_unref(pkt);
    }
    /* the file's end: what the decoder still holds */
    if (!got && !late(end) && avcodec_send_packet(c, 0) >= 0)
        got = avcodec_receive_frame(c, frame) >= 0;
    return got && frame->width > 0 && frame->height > 0 && frame->width <= SIDE_MAX &&
           frame->height <= SIDE_MAX && frame->data[0];
}

/* in's middle square at THUMB_PX in out, in a JPEG's YUV. The square is of the picture as it is
 * shown: sar is how wide one of its pixels is, so a 720 x 576 picture of 16:15 pixels is 768
 * wide and loses 96 at each side, 90 of its own.
 * ponytail: the colours go over as BT.601 whatever the video says, so an HD picture's (BT.709)
 * reds and greens are a shade off in a 64 px cover; give swscale the frame's matrix if it shows. */
static int shrink(AVFrame *in, AVRational sar, AVFrame *out) {
    int w = in->width, h = in->height;
    double wide = sar.num > 0 && sar.den > 0 ? (double)sar.num / sar.den : 1;
    if (wide < 0.1 || wide > 10) wide = 1; /* no picture's: square pixels then */
    double side = w * wide < h ? w * wide : h;
    int cw = (int)(side / wide + 0.5), ch = (int)(side + 0.5);
    cw = cw < 1 ? 1 : cw > w ? w : cw, ch = ch < 1 ? 1 : ch > h ? h : ch;
    /* from a pixel that the colour planes, a half or a quarter the size, are cut at too */
    in->crop_left = (size_t)((w - cw) / 2 & ~3), in->crop_right = (size_t)(w - cw) - in->crop_left;
    in->crop_top = (size_t)((h - ch) / 2 & ~3), in->crop_bottom = (size_t)(h - ch) - in->crop_top;
    if (av_frame_apply_cropping(in, AV_FRAME_CROP_UNALIGNED) < 0) return 0;
    out->format = AV_PIX_FMT_YUVJ420P, out->width = out->height = THUMB_PX;
    if (av_frame_get_buffer(out, 32) < 0) return 0;
    struct SwsContext *s = sws_getContext(in->width, in->height, in->format, THUMB_PX, THUMB_PX,
                                          AV_PIX_FMT_YUVJ420P, SWS_AREA, 0, 0, 0);
    int ok = s && sws_scale(s, (const uint8_t *const *)in->data, in->linesize, 0, in->height,
                            out->data, out->linesize) == THUMB_PX;
    sws_freeContext(s);
    return ok;
}

/* An n x n plane turned a quarter turn clockwise, times times. */
static void turn(uint8_t *p, int stride, int n, int times) {
    static uint8_t was[THUMB_PX * THUMB_PX];
    for (; times > 0; --times) {
        for (int y = 0; y < n; ++y) memcpy(was + y * n, p + y * stride, (size_t)n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) p[y * stride + x] = was[(n - 1 - x) * n + y];
    }
}

/* 1 when the thumbnail's picture is near black: a fade, a title card's ground. */
static int dark(const AVFrame *out) {
    long sum = 0;
    for (int y = 0; y < THUMB_PX; ++y)
        for (int x = 0; x < THUMB_PX; ++x) sum += out->data[0][y * out->linesize[0] + x];
    return sum / (THUMB_PX * THUMB_PX) < DARK;
}

/* out as a JPEG into jpg (THUMB_BYTES of room), *n bytes, at the quality Q2 Pod makes its own
 * covers with (ffmpeg -q:v 3). */
static int jpeg(AVFrame *out, unsigned char *jpg, size_t *n) {
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : 0;
    AVPacket *pkt = av_packet_alloc();
    int ok = 0;
    if (c && pkt) {
        c->width = c->height = THUMB_PX;
        c->pix_fmt = AV_PIX_FMT_YUVJ420P;
        c->time_base = (AVRational){ 1, 25 };
        c->thread_count = 1;
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = out->quality = FF_QP2LAMBDA * 3;
        out->pts = 0;
        ok = avcodec_open2(c, codec, 0) >= 0 && avcodec_send_frame(c, out) >= 0 &&
             avcodec_receive_packet(c, pkt) >= 0 && pkt->size > 0 && pkt->size <= THUMB_BYTES;
        if (ok) memcpy(jpg, pkt->data, *n = (size_t)pkt->size);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    return ok;
}

/* The picture at at microseconds into the video (under 0: from where the file stands) as a
 * thumbnail's in out. The seek is to the keyframe before; one that fails leaves the file where
 * it stood, at its start for the first picture asked for. */
static int shot(AVFormatContext *f, AVCodecContext *c, int stream, long long at, AVPacket *pkt,
                AVFrame *frame, AVFrame *out, long long *end) {
    AVStream *st = f->streams[stream];
    AVDictionaryEntry *tag = av_dict_get(st->metadata, "rotate", 0, 0);
    if (at >= 0 && av_seek_frame(f, -1, at, AVSEEK_FLAG_BACKWARD) >= 0) avcodec_flush_buffers(c);
    if (!picture(f, c, stream, pkt, frame, end) ||
        !shrink(frame, av_guess_sample_aspect_ratio(f, st, frame), out))
        return 0;
    /* a phone's video held upright is stored on its side, with the turn that shows it (FFmpeg 4.2
     * gives it as this tag, in degrees clockwise) */
    long deg = tag ? strtol(tag->value, 0, 10) % 360 : 0;
    int times = (int)((deg + 360) % 360) / 90;
    turn(out->data[0], out->linesize[0], THUMB_PX, times);
    turn(out->data[1], out->linesize[1], THUMB_PX / 2, times);
    turn(out->data[2], out->linesize[2], THUMB_PX / 2, times);
    return 1;
}

int thumb_make(const char *path, int ms, thumb_t *t, unsigned char *jpg, size_t *n) {
    char url[PATH_N + 8];
    long long end = now_ms() + ms;
    AVFormatContext *f = avformat_alloc_context();
    AVDictionary *opts = 0;
    AVCodecContext *c = 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(), *out = av_frame_alloc(), *again = 0;
    int made = 0, stream = -1;

    t->dur_ms = 0, t->w = t->h = 0, t->state = T_NONE;
#ifdef HOST /* the checks' killer file: the worker dies on it, as in a decoder's fault */
    if (getenv("VIDEOS_CRASH") && strstr(path, getenv("VIDEOS_CRASH"))) abort();
#endif
    /* "file:" so that no name is read as a protocol's, and that protocol alone for whatever the
     * file names in turn */
    snprintf(url, sizeof url, "file:%s", path);
    av_dict_set(&opts, "format_whitelist", FORMATS, 0);
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    if (f) f->interrupt_callback = (AVIOInterruptCB){ late, &end };
    /* a failed open has freed f */
    int open = f && pkt && frame && out && avformat_open_input(&f, url, 0, &opts) >= 0;
    av_dict_free(&opts);
    if (open && avformat_find_stream_info(f, 0) >= 0) {
        if (f->duration > 0 && f->duration / 1000 <= DUR_MAX) t->dur_ms = f->duration / 1000;
        stream = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    }
    /* a song's cover is a video stream too, of one picture: not a video's */
    if (stream >= 0 && !(f->streams[stream]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        const AVCodecParameters *par = f->streams[stream]->codecpar;
        AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (par->width > 0 && par->height > 0 && par->width <= SIDE_MAX && par->height <= SIDE_MAX)
            t->w = par->width, t->h = par->height;
        if (t->w && codec && !too_large(t->w, t->h, mem_available()) &&
            (c = avcodec_alloc_context3(codec)) && avcodec_parameters_to_context(c, par) >= 0) {
            c->thread_count = 1;
            /* a picture that grows far past the size the file gave, and was let through on, is
             * not decoded. The room is for a decoder's own buffers, which this counts too:
             * MPEG-4's are 280 rows of the width and 128 more, however small the picture */
            c->max_pixels = (int64_t)(t->w + 512) * (t->h + 512);
            for (unsigned i = 0; i < f->nb_streams; ++i) /* the sound is not read from the card */
                if ((int)i != stream) f->streams[i]->discard = AVDISCARD_ALL;
            long long len = t->dur_ms * 1000, from = f->start_time;        /* microseconds */
            if (from < -DUR_MAX * 1000 || from > DUR_MAX * 1000) from = 0; /* unknown, or no time */
            made = avcodec_open2(c, codec, 0) >= 0 &&
                   shot(f, c, stream, len ? from + len * 15 / 100 : -1, pkt, frame, out, &end);
            if (made && len && dark(out) && (again = av_frame_alloc()) &&
                shot(f, c, stream, from + len * 30 / 100, pkt, frame, again, &end)) {
                AVFrame *first = out;
                out = again, again = first;
            }
        }
    }
    made = made && jpeg(out, jpg, n);
    if (made) t->state = T_PICTURE;
    avcodec_free_context(&c);
    av_frame_free(&again);
    av_frame_free(&out);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (open)
        avformat_close_input(&f);
    else
        avformat_free_context(f);
    return made;
}

/* ---- The worker ---------------------------------------------------------------------------- */

/* What the server said last on in: the level shown now, into level. -1 when the server is gone
 * (its end of the pipe shut). Each of its messages is REL_MAX + 1 bytes, written whole, so one
 * read is one message. */
static int heard(int in, char *level) {
    char msg[REL_MAX + 1];
    ssize_t k = -1;
    while (in >= 0 && (k = read(in, msg, sizeof msg)) == (ssize_t)sizeof msg)
        msg[REL_MAX] = 0, strcpy(level, msg);
    return k ? 0 : -1;
}

/* The lock that says no video plays, held shared (close() lets go): the worker never reads the
 * card or decodes beside the player, and looks again every second while it has the lock. -1
 * when the server went meanwhile. */
static int quiet(int in, char *level) {
    int fd;
    while ((fd = lock_take(0)) < 0) {
        struct pollfd p = { in, POLLIN, 0 };
        poll(&p, in >= 0, 1000);
        if (heard(in, level) < 0) return -1;
    }
    return fd;
}

static void tell(int out, const thumb_t *t) {
    if (out >= 0 && write(out, t, sizeof *t) != (ssize_t)sizeof *t) _exit(0); /* no server */
}

/* The cache tidied against all, the library's videos. A line of the index goes when its video is
 * no longer there (or has changed: its key is another), and its thumbnail with it; so does the
 * line of a thumbnail that is gone, for it to be made again; and so does any file of the cache
 * that is not a line's thumbnail (the .new of a write that was killed). */
static void tidy(const listing_t *all) {
    char path[PATH_N];
    unsigned long long *key = malloc((size_t)(all->n + 1) * sizeof *key);
    char *there = calloc((size_t)ix.n + 1, 1); /* the lines whose thumbnail is in the cache */
    DIR *d = key && there ? opendir(cache_file("", path)) : 0;
    struct dirent *e;
    for (int i = 0; key && i < all->n; ++i)
        key[i] = cache_key(all->v[i].name, all->v[i].size, all->v[i].mtime);
    while (d && (e = readdir(d))) {
        const thumb_t *t = 0;
        if (strlen(e->d_name) == 20 && strspn(e->d_name, HEX) == 16 &&
            !strcmp(e->d_name + 16, ".jpg"))
            t = index_find(strtoull(e->d_name, 0, 16));
        if (t && t->state == T_PICTURE)
            there[t - ix.v] = 1;
        else if (strcmp(e->d_name, "index") && strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            unlink(cache_file(e->d_name, path));
    }
    if (d) closedir(d);
    /* from the last: the line that takes a dropped one's place has been looked at */
    for (int i = ix.n - 1; key && there && i >= 0; --i) {
        int k = 0;
        while (k < all->n && key[k] != ix.v[i].key) ++k;
        if (k < all->n && (ix.v[i].state != T_PICTURE || there[i])) continue;
        if (there[i]) unlink(thumb_file(ix.v[i].key, path));
        ix.v[i] = ix.v[--ix.n];
        ix.dirty = 1;
    }
    free(key);
    free(there);
    if (ix.dirty) index_save(); /* and what the server noted of a worker that died */
}

/* The video at rel, when the index has no line for it: probed, its thumbnail into the cache, its
 * line into the index, and the server told before (so it knows which video a worker died on) and
 * after. -1 when the server is gone. */
static int one(const char *root, const char *rel, const entry_t *e, char *level, int in, int out) {
    static unsigned char jpg[THUMB_BYTES];
    char path[PATH_N];
    size_t n = 0;
    thumb_t t;
    memset(&t, 0, sizeof t); /* its padding too: it goes down a pipe */
    t.key = cache_key(rel, e->size, e->mtime), t.state = T_TRYING;
    if (index_find(t.key) || !lib_path(root, rel, path, sizeof path)) return 0;
    int lock = quiet(in, level);
    if (lock < 0) return -1;
    tell(out, &t);
    /* a thumbnail the card won't take can't be shown either */
    if (thumb_make(path, THUMB_MS, &t, jpg, &n) && !file_put(thumb_file(t.key, path), jpg, n))
        t.state = T_NONE;
    index_put(&t);
    /* ponytail: the index is written whole for each video, which over a first round of n videos
     * is n * n / 2 lines (70 MB for 2,000 videos); append to it, and write it whole only in
     * tidy(), if a library that size ever makes the round slow. */
    index_save();
    tell(out, &t);
    close(lock);
    return heard(in, level);
}

int worker_pass(char *level, int in, int out) {
    char root[PATH_N], was[REL_MAX + 1], rel[REL_MAX + 1];
    listing_t here, all;
    int r = 0, lock = quiet(in, level);
    if (lock < 0) return -1;
    snprintf(was, sizeof was, "%s", level);
    memset(&here, 0, sizeof here), memset(&all, 0, sizeof all);
    /* a card without the folder, or pulled, is not an empty library: its cache is left alone */
    if (lib_root(root, sizeof root) && lib_all(root, &all)) {
        cache_dir();
        tidy(&all);
        lib_list(root, level, SORT_NAME, &here);
    }
    close(lock);
    for (int i = here.dirs; !r && i < here.n; ++i) /* the level shown first, then the rest */
        if (snprintf(rel, sizeof rel, "%s%s%s", was, *was ? "/" : "", here.v[i].name) <
            (int)sizeof rel)
            r = one(root, rel, &here.v[i], level, in, out) < 0 ? -1 : strcmp(level, was) != 0;
    for (int i = 0; !r && i < all.n; ++i)
        r = one(root, all.v[i].name, &all.v[i], level, in, out) < 0 ? -1 : strcmp(level, was) != 0;
    lib_free(&here), lib_free(&all);
    return r;
}

void worker_main(const char *first, int in, int out) {
    char level[REL_MAX + 1], why[80];
    setpriority(PRIO_PROCESS, 0, 19); /* playback, the list and everything else come first */
    fcntl(in, F_SETFL, O_NONBLOCK);
    if (!av_ok(why, sizeof why)) _exit(WORKER_NO_AV);
    av_log_set_level(AV_LOG_QUIET);
    av_max_alloc(ALLOC_MAX);
    snprintf(level, sizeof level, "%s", first);
    while (worker_pass(level, in, out) > 0) {}
    _exit(0); /* not exit(): nothing of the server's is flushed or run a second time */
}
