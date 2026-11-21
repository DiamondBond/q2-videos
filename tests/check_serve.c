/* Videos' host checks of the page's server (make check, under ASan and UBSan): Q2SPOT/1's framing,
 * each level's rows for libraries made in build/, the actions, the covers and the cache, the
 * thumbnails of the test clips and of files that are no videos; then the server itself, started
 * as Q2 Pod's launcher starts it and spoken to over its socket. `check_serve CMD...` runs that
 * last part alone against another build of the server: the Q2's own, under qemu (make
 * qemu-check). The clips are tools/clips.sh's ($VIDEOS_CLIPS), and FFmpeg 4.2's ffmpeg
 * ($VIDEOS_FFMPEG) makes two more here. */
#include "serve.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>

#define CARD "build/serve-card"
#define TMP "build/serve-tmp"
#define V CARD "/Videos/"
#define ART TMP "/q2-videos.art"
#define CACHE CARD "/.videos/cache"
#define CAPS "playlist progress pages library art window urls sort field screen volume"
#define CHECK(c) ((c) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c), abort()))
#define SAME(text, want)                                                                           \
    (strcmp(text, want)                                                                            \
         ? (fprintf(stderr, "%s:%d: \"%s\", not \"%s\"\n", __FILE__, __LINE__, text, want),        \
            abort())                                                                               \
         : (void)0)

static const char *clips, *ffmpeg; /* where the test clips are, and FFmpeg 4.2's ffmpeg */

static void sh(const char *fmt, ...) {
    char cmd[4 * PATH_N];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd)) fprintf(stderr, "failed: %s\n", cmd), abort();
}
/* a file of size bytes (a hole: nothing is written), its folders made, changed at mtime */
static void file(const char *path, long long size, long long mtime) {
    sh("mkdir -p \"$(dirname '%s')\"", path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct timespec t[2] = { { mtime, 0 }, { mtime, 0 } };
    CHECK(fd >= 0 && !ftruncate(fd, size) && !futimens(fd, t) && !close(fd));
}
static void text(const char *path, const char *s) { CHECK(file_put(path, s, strlen(s))); }
static int same_file(const char *a, const char *b) {
    size_t n = 0, m = 1;
    char *x = file_get(a, &n), *y = file_get(b, &m);
    int same = x && y && n == m && !memcmp(x, y, n);
    free(x), free(y);
    return same;
}
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}
/* the next of a row of numbers that is the same each run, from an unsigned seed in scope */
#define RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)
static void nap(int ms) { usleep((useconds_t)ms * 1000); }
/* a video's key in the cache, from the file as it is now */
static unsigned long long key_of(const char *root, const char *rel) {
    char path[PATH_N];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    CHECK(!stat(path, &st));
    return cache_key(rel, st.st_size, st.st_mtime);
}

/* ---- A reply, read as Q2 Pod reads it ------------------------------------------------------ */

static struct {
    char raw[REPLY_MAX + 2], cut[REPLY_MAX + 2]; /* as it came, and cut into its fields */
    size_t len;
    char *f[4 + 6 * ROWS_MAX];
    int rows;
} rp;
#define STATUS rp.f[1]
#define MSG rp.f[2]
#define CURSOR rp.f[3]
enum { KIND, URI, TITLE, SUB, CTX, POS };
#define ROW(i, k) rp.f[4 + 6 * (i) + (k)]

static int hex(char c) {
    return c >= '0' && c <= '9'                 ? c - '0'
           : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                                                : -1;
}
/* Q2 Pod's own reader (patch/spotify.c spot_reply): the header's 4 fields, then each row's 6,
 * unescaped in place. The rows' count; -1 for a reply it would refuse. */
static int spot_reply(char *buf, unsigned len, char **f, int max) {
    char *s = buf + 9, *end = buf + len;
    int k = 0;
    if (len < 9 || memcmp(buf, "Q2SPOT/1\n", 9) || strlen(buf) != len) return -1;
    while (s < end) {
        int width = k ? 6 : 4, got = 0;
        if (k + width > 4 + 6 * max) return -1;
        for (char c = 0; c != '\n';) {
            char *o = s;
            if (got == width) return -1;
            f[k + got++] = s;
            for (; s < end && *s != '\t' && *s != '\n'; ++o) {
                int v = *s != '%' ? (unsigned char)*s
                        : s + 2 < end && hex(s[1]) >= 0 && hex(s[2]) >= 0
                            ? hex(s[1]) << 4 | hex(s[2])
                            : 0;
                if (!v) return -1;
                s += *s == '%' ? 3 : 1;
                *o = (char)v;
            }
            c = s < end ? *s++ : '\n';
            *o = 0;
        }
        if (got != width) return -1;
        k += width;
    }
    return k ? (k - 4) / 6 : -1;
}

static const char *sock; /* the server's socket, when the server is another process */
static unsigned ids;

/* req's n bytes to the server; its reply into rp.raw. 0 when none came. */
static int exchange(const char *req, size_t n) {
    rp.len = 0;
    if (!sock) return rp.len = serve_reply(req, n, rp.raw), 1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ssize_t k = -1;
    if (!connect(fd, (struct sockaddr *)&addr, sizeof addr) &&
        send(fd, req, n, MSG_NOSIGNAL) == (ssize_t)n && !shutdown(fd, SHUT_WR))
        while (rp.len <= REPLY_MAX && (k = read(fd, rp.raw + rp.len, REPLY_MAX + 1 - rp.len)) > 0)
            rp.len += (size_t)k;
    close(fd);
    return !k;
}

/* The request "<id>\t<line>" asked (line: the op and its arguments, tabs between, escaped),
 * and its reply read as an action's (no rows allowed) or a level's. The rows' count. */
static int ask_as(int action, const char *fmt, ...) {
    char req[REQ_MAX + 64], id[16], line[REQ_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(id, sizeof id, "%u", ++ids);
    int n = snprintf(req, sizeof req, "Q2SPOT/1\n%s\t%s\n", id, line);
    CHECK(exchange(req, (size_t)n) && rp.len <= REPLY_MAX);
    rp.raw[rp.len] = 0;
    memcpy(rp.cut, rp.raw, rp.len + 1);
    rp.rows = spot_reply(rp.cut, (unsigned)rp.len, rp.f, action ? 0 : ROWS_MAX);
    CHECK(rp.rows >= 0);
    SAME(rp.f[0], id);
    return rp.rows;
}
#define ask(...) ask_as(0, __VA_ARGS__)
#define act(...) ask_as(1, __VA_ARGS__)

/* any bytes at all as a request: the reply's status */
static const char *status_of(const char *req, size_t n) {
    CHECK(exchange(req, n) && rp.len <= REPLY_MAX);
    rp.raw[rp.len] = 0;
    memcpy(rp.cut, rp.raw, rp.len + 1);
    CHECK((rp.rows = spot_reply(rp.cut, (unsigned)rp.len, rp.f, ROWS_MAX)) >= 0);
    return STATUS;
}
static const char *status_text(const char *req) { return status_of(req, strlen(req)); }

static void row_is(int i, const char *kind, const char *uri, const char *title, const char *sub,
                   const char *ctx, const char *pos) {
    CHECK(i < rp.rows);
    SAME(ROW(i, KIND), kind), SAME(ROW(i, URI), uri), SAME(ROW(i, TITLE), title);
    SAME(ROW(i, SUB), sub), SAME(ROW(i, CTX), ctx), SAME(ROW(i, POS), pos);
}
static const char *art_path(const char *uri, const char *ctx) {
    static char path[PATH_N];
    snprintf(path, sizeof path, "%s/q2-videos.art/%08x.jpg", tmp_dir, art_key(uri, ctx));
    return path;
}
/* The reply's covers, as Q2 Pod counts them each time it has asked (addon.c art_rows): the rows
 * whose art file is there. */
static int row_arts(void) {
    int n = 0;
    for (int i = 0; i < rp.rows; ++i) n += !access(art_path(ROW(i, URI), ROW(i, CTX)), 0);
    return n;
}
/* A live reply's: its message is the plain word, as sent. */
static int covers(void) {
    CHECK(strstr(rp.raw, "\tok\trefresh\t\n"));
    return row_arts();
}
static long long size_of(const char *path) {
    struct stat st;
    CHECK(!stat(path, &st));
    return st.st_size;
}
/* how many files the art folder holds */
static int art_count(void) {
    char cmd[PATH_N], line[32] = "";
    snprintf(cmd, sizeof cmd, "ls -A '%s/q2-videos.art' 2>/dev/null | wc -l", tmp_dir);
    FILE *p = popen(cmd, "r");
    CHECK(p && fgets(line, sizeof line, p) && !pclose(p));
    return atoi(line);
}

/* ---- A JPEG, looked at --------------------------------------------------------------------- */

static unsigned be16(const unsigned char *p) { return p[0] << 8 | p[1]; }
/* Q2 Pod's own look at a cover (patch/coverflow.c picture_size): its size from the frame header
 * among its first 64 segments. 0 when it finds none: no cover for Q2 Pod. */
static int picture_size(const char *path, int *w, int *h) {
    unsigned char b[24];
    FILE *f = fopen(path, "rb");
    int got = f && fread(b, 1, sizeof b, f) == sizeof b, kind = 0;
    if (got && b[0] == 0xff && b[1] == 0xd8)
        for (long at = 2, n = 0;
             n < 64 && !fseek(f, at, 0) && fread(b, 1, 9, f) == 9 && b[0] == 0xff && b[1] != 0xda;
             ++n, at += b[1] == 0xff ? 1 : 2 + be16(b + 2))
            if (b[1] >> 4 == 0xc && b[1] != 0xc4 && b[1] != 0xc8 && b[1] != 0xcc) {
                kind = b[1], *h = (int)be16(b + 5), *w = (int)be16(b + 7);
                break;
            }
    if (f) fclose(f);
    return kind;
}

typedef struct {
    int w, h, luma;        /* its size, and its mean brightness of 255 */
    int red_left, red_top; /* whether the middle of its left edge, and of its top, is red */
} look_t;

/* The JPEG at path decoded whole, by libavcodec: 0 when it isn't one. */
static int look(const char *path, look_t *k) {
    size_t n = 0;
    char *data = file_get(path, &n);
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int ok = data && n && c && pkt && f && av_new_packet(pkt, (int)n) >= 0;
    if (ok) memcpy(pkt->data, data, n);
    ok = ok && avcodec_open2(c, codec, 0) >= 0 && avcodec_send_packet(c, pkt) >= 0 &&
         avcodec_receive_frame(c, f) >= 0;
    if (ok) {
        long sum = 0;
        int sub = f->format == AV_PIX_FMT_YUVJ420P; /* else 4:4:4, the tiles' */
        CHECK(sub || f->format == AV_PIX_FMT_YUVJ444P);
        for (int y = 0; y < f->height; ++y)
            for (int x = 0; x < f->width; ++x) sum += f->data[0][y * f->linesize[0] + x];
        k->w = f->width, k->h = f->height, k->luma = (int)(sum / (f->width * f->height));
        /* red: Cr well above the middle, Cb below it */
#define RED(x, y)                                                                                  \
    (f->data[2][((y) >> sub) * f->linesize[2] + ((x) >> sub)] > 180 &&                             \
     f->data[1][((y) >> sub) * f->linesize[1] + ((x) >> sub)] < 120)
        k->red_left = RED(3, f->height / 2), k->red_top = RED(f->width / 2, 3);
    }
    av_frame_free(&f), av_packet_free(&pkt), avcodec_free_context(&c), free(data);
    return ok;
}

/* The cover at path is one Q2 Pod shows: a JPEG it can size, THUMB_PX square, that decodes, of
 * a thumbnail's bytes at most; with nothing left beside it by the write. */
static void cover_ok(const char *path, look_t *k) {
    char tmp[PATH_N];
    struct stat st;
    int w = 0, h = 0;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    CHECK(!stat(path, &st) && st.st_size > 100 && st.st_size <= THUMB_BYTES && access(tmp, 0));
    CHECK(picture_size(path, &w, &h) && w == THUMB_PX && h == THUMB_PX);
    CHECK(look(path, k) && k->w == THUMB_PX && k->h == THUMB_PX);
}

/* ---- Framing ------------------------------------------------------------------------------- */

static void check_framing(void) {
    char out[64], all[256], back[800];
    CHECK(q2_escape("plain", out, sizeof out, 255) == 5 && !memcmp(out, "plain", 5));
    CHECK(q2_escape("", out, sizeof out, 255) == 0 && q2_escape("", out, 0, 255) == 0);
    CHECK(q2_escape("a%b\tc\nd\re", out, sizeof out, 255) == 17 &&
          !memcmp(out, "a%25b%09c%0Ad%0De", 17));
    CHECK(q2_escape("Привет · 日本", out, sizeof out, 255) == 22); /* UTF-8 goes as it is */
    CHECK(q2_escape("abc", out, 3, 255) == 3 && q2_escape("abc", out, 2, 255) == -1);
    CHECK(q2_escape("ab%", out, 5, 255) == 5 && q2_escape("ab%", out, 4, 255) == -1);
    /* a cut is before a character, never inside one: é is 2 bytes, 日 is 3 */
    CHECK(q2_escape("abcdef", out, sizeof out, 4) == 4 && !memcmp(out, "abcd", 4));
    CHECK(q2_escape("abé", out, sizeof out, 3) == 2 && q2_escape("abé", out, sizeof out, 4) == 4);
    CHECK(q2_escape("a日b", out, sizeof out, 2) == 1 && q2_escape("a日b", out, sizeof out, 3) == 1);
    CHECK(q2_escape("a日b", out, sizeof out, 4) == 4 && q2_escape("日", out, sizeof out, 0) == 0);
    CHECK(q2_escape("%%%%", out, sizeof out, 2) == 6 &&
          !memcmp(out, "%25%25", 6)); /* cut, then escaped */

    strcpy(out, "a%25b%09c%0Ad%0de%7E");
    CHECK(q2_unescape(out)), SAME(out, "a%b\tc\nd\re~"); /* either case, and any byte */
    strcpy(out, "");
    CHECK(q2_unescape(out)), SAME(out, "");
    static const char *const bad[] = { "%",   "a%",  "%2",  "a%2",   "%zz",
                                       "%2g", "%g2", "%00", "ok%00", "%%25" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i)
        strcpy(out, bad[i]), CHECK(!q2_unescape(out));
    for (int c = 1; c < 256; ++c) all[c - 1] = (char)c; /* every byte there and back */
    all[255] = 0;
    int n = q2_escape(all, back, sizeof back - 1, sizeof all);
    CHECK(n == 255 + 2 * 4);
    back[n] = 0;
    CHECK(!strpbrk(back, "\t\n\r") && q2_unescape(back) && !strcmp(back, all));

    /* requests that are none: "bad", under the id 0 */
    char big[REQ_MAX + 2];
    static const char *const none[] = { "",
                                        "Q2SPOT/1",
                                        "Q2SPOT/1\n",
                                        "Q2SPOT/1\n\n",
                                        "Q2SPOT/2\n1\tmenu\n",
                                        "1\tmenu\n",
                                        "q2spot/1\n1\tmenu\n",
                                        "Q2SPOT/1\n1\n",
                                        "Q2SPOT/1\n1",
                                        "Q2SPOT/1\n\tmenu\n",
                                        "Q2SPOT/1\nx\tmenu\n",
                                        "Q2SPOT/1\n-1\tmenu\n",
                                        "Q2SPOT/1\n1 \tmenu\n",
                                        "Q2SPOT/1\n12345678901\tmenu\n",
                                        "Q2SPOT/1\n1\tme%nu\n",
                                        "Q2SPOT/1\n1\tmenu\t%0\n",
                                        "Q2SPOT/1\n1\tdir\ta%00b\n",
                                        "Q2SPOT/1\n1\tdir\ta\t1\tb\n",
                                        "Q2SPOT/1\r\n1\tmenu\r\n" };
    for (size_t i = 0; i < sizeof none / sizeof *none; ++i) {
        SAME(status_text(none[i]), "bad"), SAME(rp.f[0], "0");
        CHECK(!rp.rows && !*CURSOR);
    }
    SAME(status_of("Q2SPOT/1\n1\tmenu\0x\n", 18), "bad"); /* a NUL in it */
    memset(big, 'x', sizeof big);
    memcpy(big, "Q2SPOT/1\n7\tsettings\n", 20);
    SAME(status_of(big, REQ_MAX + 1), "bad"), SAME(rp.f[0], "0"); /* a byte too long */
    SAME(status_of(big, REQ_MAX), "ok"), SAME(rp.f[0], "7"); /* what follows its line is not read */
    SAME(status_text("Q2SPOT/1\n4294967295\tsettings"), "ok"),
        SAME(rp.f[0], "4294967295"); /* no line end */

    /* requests of ours with the wrong arguments: "bad", under their id; other ops: "unsupported" */
    static const char *const wrong[] = { "menu\tx",        "menu\t-1",      "menu\t1234567890",
                                         "menu\t1\t2",     "dir",           "dir\t",
                                         "dir\t..",        "dir\t../Other", "dir\t/etc",
                                         "dir\ta//b",      "dir\ta/",       "dir\t.hidden",
                                         "dir\ta\tx",      "settings\tx",   "set",
                                         "set\tnext\t1",   "set\tvolume",   "mark",
                                         "mark\t../x.mp4", "mark\ta.mp4\tb" };
    for (size_t i = 0; i < sizeof wrong / sizeof *wrong; ++i) {
        CHECK(!act("%s", wrong[i]));
        SAME(STATUS, "bad");
        CHECK(*MSG);
    }
    static const char *const other[] = { "play", "stop", "caps", "search\tx", "MENU", "menus", "" };
    for (size_t i = 0; i < sizeof other / sizeof *other; ++i)
        CHECK(!act("%s", other[i])), SAME(STATUS, "unsupported");
}

/* ---- Levels -------------------------------------------------------------------------------- */

static marks_t m; /* 24 kB: not the stack's */

static void check_levels(void) {
    const long long MB = 1 << 20;

    /* Q2 Pod without screen rows (no file at all on the oldest): the notice, and nothing else */
    for (int i = 0; i < 3; ++i) {
        CHECK(ask("menu") == 1), SAME(STATUS, "ok"), SAME(MSG, ""), SAME(CURSOR, "");
        row_is(0, "info", "", "Update Q2 Pod to play videos",
               "This app needs Q2 Pod 1.9.0 or later", "", "");
        text(TMP "/q2-addon.caps", i ? "playlist art pages screens xscreen\n" : "");
    }
    CHECK(ask("dir\tSeries") == 1), SAME(ROW(0, KIND), "info");
    text(TMP "/q2-addon.caps", "screen");
    CHECK(ask("menu") == 2);
    text(TMP "/q2-addon.caps",
         "playlist  pages\tscreen\r\nvolume\n"); /* any blanks between the words */
    CHECK(ask("menu") == 2);

    /* no Videos folder, an empty one, one with no video in it: how to get some, and Settings */
    for (int i = 0; i < 3; ++i) {
        CHECK(ask("menu") == 2), SAME(STATUS, "ok"), SAME(CURSOR, "");
        CHECK(covers() == 0); /* live, and no covers without the art word */
        row_is(0, "info", "", "No videos", "Copy videos into the Videos folder on the card", "",
               "");
        row_is(1, "list", "settings", "Settings", "", "", "");
        if (!i) sh("mkdir -p " V "Empty/Emptier");
        if (i)
            file(V "notes.txt", 1, 0), file(V ".hidden.mp4", 1, 0), file(V "Docs/readme.pdf", 1, 0);
    }
    CHECK(!act("dir\tNope")), SAME(STATUS, "unavailable"), SAME(MSG, "Folder not found");
    CHECK(!act("dir\tnotes.txt")), SAME(STATUS, "unavailable");
    CHECK(ask("dir\tEmpty") == 1), SAME(STATUS, "ok"),
        row_is(0, "info", "", "No videos", "", "", "");

    /* a library: folders first, with their counts through their subfolders, then the videos, in
     * natural order; Settings last */
    file(V "Episode 10.mkv", 350 * MB, 1000), file(V "episode 2.mkv", 1200 * MB, 3000);
    file(V "Episode 1.MP4", 12 << 10, 2000),
        file(V "100% Real\xe2\x80\xa6 (2019).mp4", 5 * MB, 500);
    file(V "Series/Season 1/a.mkv", MB, 0), file(V "Series/Season 1/b.avi", MB, 0);
    file(V "Series/Season 2/c.webm", MB, 0), file(V "Series/Season 2/.thumbs/x.mp4", 1, 0);
    file(V "Movies/Фильм.mkv", 5120 * MB, 4000), file(V "Movies/Film.srt", 1, 0);
    CHECK(ask("menu") == 7), SAME(STATUS, "ok"), SAME(CURSOR, "");
    CHECK(covers() == 0);
    row_is(0, "list", "dir", "Movies", "1 video", "Movies", "");
    row_is(1, "list", "dir", "Series", "3 videos", "Series", "");
    /* a video has no subtitle until the worker has read its length */
    row_is(2, "screen", "play", "100% Real\xe2\x80\xa6 (2019)", "",
           "100% Real\xe2\x80\xa6 (2019).mp4", "mark");
    row_is(3, "screen", "play", "Episode 1", "", "Episode 1.MP4", "mark");
    row_is(4, "screen", "play", "episode 2", "", "episode 2.mkv", "mark");
    row_is(5, "screen", "play", "Episode 10", "", "Episode 10.mkv", "mark");
    row_is(6, "list", "settings", "Settings", "", "", "");
    CHECK(
        strstr(rp.raw, "\t100%25 Real")); /* its % went out escaped, in the title and the context */
    CHECK(ask("dir\tSeries") == 2);
    row_is(0, "list", "dir", "Season 1", "2 videos", "Series/Season 1", "");
    row_is(1, "list", "dir", "Season 2", "1 video", "Series/Season 2", "");
    CHECK(ask("dir\tSeries/Season 1") ==
          2); /* no Continue Watching and no Settings below the top */
    row_is(0, "screen", "play", "a", "", "Series/Season 1/a.mkv", "mark");
    row_is(1, "screen", "play", "b", "", "Series/Season 1/b.avi", "mark");
    CHECK(ask("dir\tMovies") == 1),
        row_is(0, "screen", "play", "Фильм", "", "Movies/Фильм.mkv", "mark");
    /* an escaped argument is read back: the video with a % in its name is found */
    CHECK(!act("mark\t100%%25 Real\xe2\x80\xa6 (2019).mp4")), SAME(MSG, "Marked as watched");
    unlink(CARD "/.videos/marks");
    CHECK(!act("dir\tSeries/Season 3")), SAME(STATUS, "unavailable");

    /* Sort: Recently Added is the videos by when each came to the card (its creation time, not
     * the date it has: a copied file keeps that), the last first; the folders stay as they were.
     * One is put there anew: the last to come, and not the last changed. */
    struct statx sx;
    nap(20); /* past a tick of the file system's clock */
    unlink(V "Episode 1.MP4"), file(V "Episode 1.MP4", 12 << 10, 2000);
    CHECK(!act("set\tsort")), SAME(STATUS, "ok"), SAME(MSG, "");
    CHECK(ask("menu") == 7);
    SAME(ROW(0, TITLE), "Movies"), SAME(ROW(1, TITLE), "Series");
    if (!syscall(SYS_statx, AT_FDCWD, V "Episode 1.MP4", 0, STATX_BTIME, &sx) &&
        sx.stx_mask & STATX_BTIME) {
        SAME(ROW(2, TITLE), "Episode 1"), SAME(ROW(3, CTX), "100% Real\xe2\x80\xa6 (2019).mp4");
        SAME(ROW(4, TITLE), "episode 2"), SAME(ROW(5, TITLE), "Episode 10");
    }
    /* a card that keeps no creation time: by when each was last changed */
    setenv("VIDEOS_NO_BTIME", "1", 1);
    CHECK(ask("menu") == 7);
    SAME(ROW(2, TITLE), "episode 2"), SAME(ROW(3, TITLE), "Episode 1");
    SAME(ROW(4, TITLE), "Episode 10"), SAME(ROW(5, CTX), "100% Real\xe2\x80\xa6 (2019).mp4");
    unsetenv("VIDEOS_NO_BTIME");
    CHECK(!act("set\tsort"));
    CHECK(ask("menu") == 7), SAME(ROW(5, TITLE), "Episode 10");

    /* a video's subtitle by its state. Probed: its length, from the index */
    index_put(
        &(thumb_t){ key_of(CARD "/Videos", "Episode 10.mkv"), 6120000, 1920, 1080, T_PICTURE });
    index_put(&(thumb_t){ key_of(CARD "/Videos", "episode 2.mkv"), 45000, 0, 0, T_NONE });
    index_put(&(thumb_t){ key_of(CARD "/Videos", "Episode 1.MP4"), 0, 0, 0,
                          T_NONE }); /* no length read */
    index_put(
        &(thumb_t){ key_of(CARD "/Videos", "Movies/Фильм.mkv"), 1440000, 640, 360, T_PICTURE });
    CHECK(ask("menu") == 7);
    SAME(ROW(3, SUB), ""), SAME(ROW(4, SUB), "45 s"), SAME(ROW(5, SUB), "1 h 42 min");
    /* in progress: what is left, of the length the player marked or else the worker read */
    mark_t *k = marks_put(&m, "Episode 10.mkv", 350 * MB);
    k->pos_ms = 600000, k->dur_ms = 1980000;
    marks_put(&m, "episode 2.mkv", 1200 * MB)->pos_ms = 44000;
    k = marks_put(&m, "Episode 1.MP4", 12 << 10); /* watched, whatever its place */
    k->pos_ms = 5000, k->flags = MARK_WATCHED;
    marks_put(&m, "100% Real\xe2\x80\xa6 (2019).mp4", 5 * MB); /* a mark never begun: as unmarked */
    marks_put(&m, "gone.mp4", 1)->pos_ms = 100;                /* a video no longer there */
    CHECK(marks_save(&m));
    CHECK(ask("menu") == 8);
    /* Continue Watching: the unfinished video played last, first of all; what is left of it,
     * then its name */
    row_is(0, "screen", "play", "Continue Watching", "Less than a minute left \xc2\xb7 episode 2",
           "episode 2.mkv", "mark");
    SAME(ROW(3, SUB), ""), SAME(ROW(4, SUB), "Watched");
    SAME(ROW(5, SUB), "Less than a minute left"), SAME(ROW(6, SUB), "23 min left");
    row_is(7, "list", "settings", "Settings", "", "", "");
    CHECK(ask("dir\tSeries") == 2); /* at the top alone */
    /* one in a folder, played later; the length is the worker's when the mark has none */
    marks_put(&m, "Movies/Фильм.mkv", 5120 * MB)->pos_ms = 60000;
    CHECK(marks_save(&m) && ask("menu") == 8);
    row_is(0, "screen", "play", "Continue Watching", "23 min left \xc2\xb7 Фильм",
           "Movies/Фильм.mkv", "mark");
    CHECK(ask("dir\tMovies") == 1), SAME(ROW(0, SUB), "23 min left");
    /* a mark of another size is another video's: the row is as unmarked, and nothing to continue */
    file(V "Movies/Фильм.mkv", 5121 * MB, 4000);
    CHECK(ask("dir\tMovies") == 1), SAME(ROW(0, SUB), "");
    CHECK(ask("menu") == 8), SAME(ROW(0, SUB), "Less than a minute left \xc2\xb7 episode 2");
    /* what is shown is cut at 255 bytes, never inside a character (q2_escape, above): here the
     * 255th would be a letter's first half */
    char name[300] = V, rel[300];
    for (int i = 0; i < 124; ++i) strcat(name, "и");
    strcat(name, ".mp4"), snprintf(rel, sizeof rel, "%s", name + strlen(V));
    file(name, MB, 0);
    k = marks_put(&m, rel, MB);
    k->pos_ms = 1000, k->dur_ms = 3600000;
    CHECK(marks_save(&m) && ask("menu") == 9);
    CHECK(strlen(ROW(0, SUB)) == TEXT_MAX - 1 && strlen(ROW(0, TITLE)) == 17 &&
          !strcmp(ROW(0, CTX), rel));
    CHECK(!memcmp(ROW(0, SUB), "1 h left \xc2\xb7 \xd0\xb8", 14) &&
          strlen(ROW(8 - 1, TITLE)) == 248);
    unlink(name);
    marks_free(&m);
    unlink(CARD "/.videos/marks");
    index_free();
}

/* A long folder: paged by cursor, each row once and in order; a short one is live. */
static void check_paging(void) {
    char path[PATH_N], want[64];
    for (int n = 47; n <= 49; ++n) { /* 48 rows are live; 49 are paged */
        snprintf(path, sizeof path, V "Many/v%d.mp4", n), file(path, 1, 0);
        if (n == 47)
            for (int i = 1; i < 47; ++i)
                snprintf(path, sizeof path, V "Many/v%d.mp4", i), file(path, 1, 0);
        CHECK(ask("dir\tMany") == n), SAME(MSG, n > 48 ? "pages" : "refresh"), SAME(CURSOR, "");
    }
    snprintf(path, sizeof path, V "Many/v50.mp4"), file(path, 1, 0);
    CHECK(ask("dir\tMany") == 50), SAME(MSG, "pages"),
        SAME(CURSOR, ""); /* a full page, and no more */

    for (int i = 51; i <= 120; ++i)
        snprintf(path, sizeof path, V "Many/v%d.mp4", i), file(path, 1, 0);
    sh("mkdir -p " V "Many/Sub && cp " V "Many/v1.mp4 " V "Many/Sub/"); /* and a folder: 121 rows */
    int seen = 0;
    for (char cursor[16] = ""; !seen || *cursor; snprintf(cursor, sizeof cursor, "%s", CURSOR)) {
        int n = *cursor ? ask("dir\tMany\t%s", cursor) : ask("dir\tMany");
        SAME(STATUS, "ok"), SAME(MSG, "pages");
        CHECK(n == (seen < 100 ? 50 : 21));
        for (int i = 0; i < n; ++i, ++seen) {
            snprintf(want, sizeof want, seen ? "Many/v%d.mp4" : "Many/Sub", seen);
            SAME(ROW(i, CTX), want);
        }
        snprintf(want, sizeof want, "%d", seen);
        SAME(CURSOR, seen < 121 ? want : "");
    }
    CHECK(seen == 121);
    /* from any row; past the end there is nothing */
    CHECK(ask("dir\tMany\t119") == 2), SAME(ROW(0, CTX), "Many/v119.mp4"), SAME(CURSOR, ""),
        SAME(MSG, "pages");
    CHECK(ask("dir\tMany\t0") == 50), SAME(CURSOR, "50");
    CHECK(ask("dir\tMany\t121") == 0 && ask("dir\tMany\t999999999") == 0), SAME(STATUS, "ok"),
        SAME(CURSOR, "");
    CHECK(ask("dir\tMany/Sub\t0") == 1), SAME(MSG, "refresh");
    CHECK(ask("dir\tMany/Sub\t1") == 0), SAME(MSG, "pages"); /* a page of a list is never live */
    /* the top pages too, Settings its last row */
    sh("mv " V "Many/v*.mp4 " V);
    CHECK(ask("menu") == 50), SAME(MSG, "pages"), SAME(CURSOR, "50"), SAME(ROW(0, TITLE), "Many");
    CHECK(ask("menu\t50") == 50 && ask("menu\t100") > 20), SAME(ROW(rp.rows - 1, URI), "settings"),
        SAME(CURSOR, "");
    sh("rm -rf " V "v*.mp4 " V "Many");
}

/* A context Q2 Pod could not send back is left out: CTX_MAX bytes escaped, each % three. */
static void check_contexts(void) {
    char path[PATH_N], deep[400], name[200];
    memset(deep, 0, sizeof deep), memset(name, 'n', sizeof name);
    strcpy(deep, "Deep/");
    for (int i = 0; i < 3; ++i)
        memset(deep + strlen(deep), 'd', 99), strcat(deep, "/"); /* 305 bytes */
    CHECK(strlen(deep) == 305);
    snprintf(path, sizeof path, V "%s%.*s.mp4", deep, CTX_MAX - 305 - 4, name),
        file(path, 1, 0); /* 390 */
    snprintf(path, sizeof path, V "%s%.*s.mkv", deep, CTX_MAX - 305 - 4 + 1, name),
        file(path, 1, 0); /* 391 */
    snprintf(path, sizeof path, V "%s%.*s.avi", deep, 400 - 305 - 4 + 1, name),
        file(path, 1, 0); /* 401 */
    snprintf(path, sizeof path, V "%s%%%.*s.mov", deep, CTX_MAX - 305 - 4 - 3, name),
        file(path, 1, 0); /* 388, 390 escaped */
    snprintf(path, sizeof path, V "%s%%%.*s.flv", deep, CTX_MAX - 305 - 4 - 2, name),
        file(path, 1, 0); /* 389, 391 escaped */
    snprintf(path, sizeof path, V "%s%.*s/in.mp4", deep, CTX_MAX - 305, name),
        file(path, 1, 0); /* a folder at 390 */
    snprintf(path, sizeof path, V "%s%.*s/in.mp4", deep, CTX_MAX - 305 + 1, name),
        file(path, 1, 0); /* and at 391 */
    deep[304] = 0;
    CHECK(ask("dir\t%s", deep) == 3);
    CHECK(strlen(ROW(0, CTX)) == CTX_MAX && !strcmp(ROW(0, KIND), "list"));
    CHECK(strlen(ROW(1, CTX)) == CTX_MAX - 2 && ROW(1, TITLE)[0] == '%' &&
          !strcmp(ROW(1, CTX) + CTX_MAX - 6, ".mov"));
    CHECK(strlen(ROW(2, CTX)) == CTX_MAX && !strcmp(ROW(2, CTX) + CTX_MAX - 4, ".mp4"));
    for (int i = 0; i < 3; ++i) CHECK(!memcmp(ROW(i, CTX), deep, 304));
    /* the folder at the limit opens, and its video, past it, has no row */
    CHECK(ask("dir\t%s", ROW(0, CTX)) == 1), SAME(ROW(0, TITLE), "No videos");
    sh("rm -rf " V "Deep");
}

/* ---- Actions ------------------------------------------------------------------------------- */

static void check_actions(void) {
    settings_t s;
    char *got;
    /* Settings: each row an action that steps its setting, the level live, the version last */
    unlink(CARD "/.videos/settings");
    CHECK(ask("settings") == 6), SAME(STATUS, "ok"), SAME(CURSOR, "");
    CHECK(covers() == 0);
    row_is(0, "action", "set", "Autoplay", "Off", "next", "");
    row_is(1, "action", "set", "Subtitles", "On", "subs", "");
    row_is(2, "action", "set", "Subtitle Size", "Medium", "subsize", "");
    row_is(3, "action", "set", "Fit to Screen", "Off", "fill", "");
    row_is(4, "action", "set", "Sort", "By Name", "sort", "");
    row_is(5, "info", "", "Version", VERSION, "", "");
    /* a press: "ok", no message (the row changes in place) and no rows; kept on the card */
    static const char *const key[] = { "next", "subs", "subsize", "fill", "sort" };
    static const char *const then[] = { "On", "Off", "Large", "On", "Recently Added" };
    for (int i = 0; i < 5; ++i) {
        CHECK(!act("set\t%s", key[i])), SAME(STATUS, "ok"), SAME(MSG, ""), SAME(CURSOR, "");
        CHECK(ask("settings") == 6), SAME(ROW(i, SUB), then[i]);
    }
    CHECK((got = file_get(CARD "/.videos/settings", 0)));
    SAME(got, "next=1\nsubs=0\nsubsize=2\nfill=1\nsort=1\n");
    free(got);
    CHECK(!act("set\tsubsize") && ask("settings") == 6), SAME(ROW(2, SUB), "Small");
    CHECK(!act("set\tsubsize") && ask("settings") == 6),
        SAME(ROW(2, SUB), "Medium"); /* round again */
    for (int i = 0; i < 5; ++i)
        if (i != 2) CHECK(!act("set\t%s", key[i]));
    settings_load(&s);
    CHECK(!s.next && s.subs == 1 && s.subsize == 1 && !s.fill && s.sort == SORT_NAME);
    CHECK(!act("set\tvolume")), SAME(STATUS, "bad"), SAME(MSG, "No such setting");

    /* held Play/Pause on a video: watched, with its toast; again: not */
    unlink(CARD "/.videos/marks");
    CHECK(!act("mark\tEpisode 1.MP4")), SAME(STATUS, "ok"), SAME(MSG, "Marked as watched"),
        SAME(CURSOR, "");
    CHECK((got = file_get(CARD "/.videos/marks", 0)));
    SAME(got, "0\t0\t1\t1\t-1\t-1\t12288\tEpisode 1.MP4\n");
    free(got);
    CHECK(ask("menu") == 7), SAME(ROW(3, SUB), "Watched");
    CHECK(!act("mark\tEpisode 1.MP4")), SAME(STATUS, "ok"), SAME(MSG, "Marked as unwatched");
    CHECK(ask("menu") == 7), SAME(ROW(3, SUB), "");
    /* one in progress: watched where its mark is (its tracks kept, and its turn among the
     * marks), so what is continued next is what was played before it */
    marks_load(&m);
    mark_t *k = marks_put(&m, "Episode 10.mkv", 350ll << 20);
    k->pos_ms = 600000, k->dur_ms = 1980000, k->audio = 2, k->sub = 1;
    marks_put(&m, "Series/Season 1/a.mkv", 1 << 20)->pos_ms = 9000;
    CHECK(marks_save(&m) && ask("menu") == 8), SAME(ROW(0, CTX), "Series/Season 1/a.mkv");
    CHECK(!act("mark\tSeries/Season 1/a.mkv")), SAME(MSG, "Marked as watched");
    CHECK(ask("menu") == 8), SAME(ROW(0, CTX), "Episode 10.mkv"), SAME(ROW(6, SUB), "23 min left");
    CHECK(!act("mark\tEpisode 10.mkv")), SAME(MSG, "Marked as watched");
    CHECK(ask("menu") == 7), SAME(ROW(5, SUB), "Watched");
    marks_load(&m);
    CHECK((k = marks_find(&m, "Episode 10.mkv", 350ll << 20)) && k->flags == MARK_WATCHED &&
          k->pos_ms == 600000);
    CHECK(k->seq == 2 && k->audio == 2 && k->sub == 1 && k->dur_ms == 1980000);
    CHECK(marks_find(&m, "Series/Season 1/a.mkv", 1 << 20)->seq == 3 &&
          marks_find(&m, "Episode 1.MP4", 12288)->seq == 1);
    CHECK(!act("mark\tEpisode 10.mkv")), SAME(MSG, "Marked as unwatched");
    marks_load(&m);
    CHECK((k = marks_find(&m, "Episode 10.mkv", 350ll << 20)) && !k->flags && k->pos_ms == 600000 &&
          k->seq == 2 && k->audio == 2);
    /* its place is kept: it is the one to continue again, with what was left of it */
    CHECK(ask("menu") == 8), SAME(ROW(0, CTX), "Episode 10.mkv"), SAME(ROW(6, SUB), "23 min left");
    /* one watched from no place: its length again, as the player read it */
    k->pos_ms = 0, k->flags = MARK_WATCHED;
    CHECK(marks_save(&m) && ask("menu") == 7), SAME(ROW(5, SUB), "Watched");
    CHECK(!act("mark\tEpisode 10.mkv")), SAME(MSG, "Marked as unwatched");
    CHECK(ask("menu") == 7), SAME(ROW(5, SUB), "33 min");
    marks_free(&m);
    /* what is no video of the library's */
    static const char *const no[] = { "gone.mp4", "notes.txt", "Series", "Series/Season 1",
                                      "Movies/Film.srt" };
    for (size_t i = 0; i < sizeof no / sizeof *no; ++i)
        CHECK(!act("mark\t%s", no[i])), SAME(STATUS, "unavailable"), SAME(MSG, "Video not found");

    /* a card that takes no write: said, not passed over */
    sh("mv " CARD "/.videos " CARD "/.videos.was && touch " CARD "/.videos");
    CHECK(!act("set\tnext")), SAME(STATUS, "error"), SAME(MSG, "Can't write to the card");
    CHECK(!act("mark\tEpisode 1.MP4")), SAME(STATUS, "error"), SAME(MSG, "Can't write to the card");
    sh("rm " CARD "/.videos && mv " CARD "/.videos.was " CARD "/.videos");
    unlink(CARD "/.videos/marks");
}

/* Requests with bytes changed and ends cut off, at random but the same each run: each has a
 * reply Q2 Pod can read, with one of the five statuses. */
static void check_bent_requests(void) {
    static const char *const good[] = { "Q2SPOT/1\n7\tmenu\n",
                                        "Q2SPOT/1\n8\tmenu\t5\n",
                                        "Q2SPOT/1\n9\tdir\tSeries\n",
                                        "Q2SPOT/1\n10\tdir\tSeries/Season 1\t1\n",
                                        "Q2SPOT/1\n11\tsettings\n",
                                        "Q2SPOT/1\n12\tset\tsort\n",
                                        "Q2SPOT/1\n13\tmark\tEpisode 1.MP4\n",
                                        "Q2SPOT/1\n14\tdir\t100%25 Real\n" };
    static const char with[] = "\t\n%/.\r0\x80"; /* and the NUL that ends it */
    unsigned seed = 1;
    char req[64];
    for (int round = 0; round < 4000; ++round) {
        const char *from = good[round % (sizeof good / sizeof *good)];
        size_t n = strlen(from);
        memcpy(req, from, n);
        for (int flips = 1 + (int)(RND() % 3); flips; --flips) {
            size_t at = RND() % n;
            req[at] = RND() % 2 ? with[RND() % sizeof with] : (char)RND();
        }
        const char *status = status_of(req, round % 7 == 6 ? RND() % (n + 1) : n);
        CHECK(!strcmp(status, "ok") || !strcmp(status, "bad") || !strcmp(status, "unsupported") ||
              !strcmp(status, "unavailable") || !strcmp(status, "error"));
        CHECK(strspn(rp.f[0], "0123456789") == strlen(rp.f[0]) && *rp.f[0]);
    }
    unlink(CARD "/.videos/settings"),
        unlink(CARD "/.videos/marks"); /* what a set or a mark among them left */
}

/* ---- Thumbnails ---------------------------------------------------------------------------- */

static unsigned char jpg[THUMB_BYTES];

/* thumb_make of path, its JPEG left at TMP/t.jpg and looked at */
static int thumb(const char *path, thumb_t *t, look_t *k) {
    size_t n = 0;
    memset(t, 0, sizeof *t), memset(k, 0, sizeof *k);
    long long began = now_ms();
    int made = thumb_make(path, THUMB_MS, t, jpg, &n);
    long long took = now_ms() - began;
    CHECK(took < THUMB_MS + 1500); /* never much past its time */
    /* none for good, unless its time ran out */
    CHECK(t->state == (made ? T_PICTURE : took > THUMB_MS ? T_LATER : T_NONE));
    if (made)
        CHECK(n > 100 && n <= THUMB_BYTES && file_put(TMP "/t.jpg", jpg, n)),
            cover_ok(TMP "/t.jpg", k);
    return made;
}

static void check_thumbs(void) {
    static const struct {
        const char *name;
        int dur, w, h; /* the clip's seconds and picture; w 0: it has none */
    } clip[] = { { "h264-aac.mp4", 30, 640, 360 },  { "two-audio-subs.mkv", 10, 640, 360 },
                 { "sidecar.mp4", 10, 640, 360 },   { "mpeg4.avi", 5, 640, 360 },
                 { "vp9-opus.webm", 5, 640, 360 },  { "no-audio.mp4", 5, 640, 360 },
                 { "audio-only.mp4", 5, 0, 0 },     { "rotated.mp4", 5, 640, 360 },
                 { "anamorphic.mp4", 5, 720, 576 }, { "big-1080p.mp4", 5, 1920, 1080 },
                 { "60fps.mp4", 5, 640, 360 },      { "ass.mkv", 10, 640, 360 },
                 { "offset.ts", 5, 640, 360 } }; /* one keyframe: nothing to seek to at 15% */
    char path[PATH_N], cmd[2 * PATH_N];
    thumb_t t;
    look_t k;
    for (size_t i = 0; i < sizeof clip / sizeof *clip; ++i) {
        snprintf(path, sizeof path, "%s/%s", clips, clip[i].name);
        CHECK(thumb(path, &t, &k) == (clip[i].w > 0));
        CHECK(t.w == clip[i].w && t.h == clip[i].h && llabs(t.dur_ms - clip[i].dur * 1000) < 200);
        if (!clip[i].w) continue;
        CHECK(k.luma > 60); /* the test picture: bright bars */
        /* its red bar is at the left (of the middle square, for the wider pictures; the 4:3 one
         * shows it whole), also in the video stored on its side: turned as it is shown */
        if (!strcmp(clip[i].name, "rotated.mp4") || !strcmp(clip[i].name, "anamorphic.mp4"))
            CHECK(k.red_left && !k.red_top);
    }

    /* a picture near black at 15% of the video is taken at 30% instead; a video black throughout
     * still has its (black) picture */
    sh("'%s' -v error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=10 "
       "-vf \"drawbox=enable='lt(t,2.5)':color=black:t=fill\" -c:v mpeg4 -g 12 -q:v 5 " TMP
       "/dark-start.avi",
       ffmpeg);
    CHECK(thumb(TMP "/dark-start.avi", &t, &k) && k.luma > 60 && t.w == 320 && t.h == 240);
    sh("'%s' -v error -y -f lavfi -i color=black:size=320x240:rate=25:duration=4 -c:v mpeg4 -q:v "
       "5 " TMP "/dark.mkv",
       ffmpeg);
    CHECK(thumb(TMP "/dark.mkv", &t, &k) && k.luma < 20);

    /* out of time: let go, and no picture for now (a slow card is not the file's doing) */
    snprintf(path, sizeof path, "%s/big-1080p.mp4", clips);
    size_t n = 0;
    CHECK(!thumb_make(path, 0, &t, jpg, &n) && t.state == T_LATER);

    /* files that are no videos, whatever their name: no picture, and no harm */
    static const char *const junk[] = { "empty.mp4", "one.mkv", "text.avi", "zeros.mp4",
                                        "cut.mp4",   "cut.mkv", "half.avi" };
    file(TMP "/empty.mp4", 0, 0), text(TMP "/one.mkv", "x"), file(TMP "/zeros.mp4", 3 << 20, 0);
    text(TMP "/text.avi", "RIFF this is not a film, it is a note that somebody renamed.\n");
    sh("head -c 3000 '%s/h264-aac.mp4' > " TMP "/cut.mp4",
       clips); /* its header alone, and not all of it */
    sh("head -c 700 '%s/ass.mkv' > " TMP "/cut.mkv", clips);
    sh("head -c 400000 '%s/mpeg4.avi' > " TMP "/half.avi",
       clips); /* no index: it still has pictures */
    for (size_t i = 0; i < sizeof junk / sizeof *junk; ++i) {
        snprintf(path, sizeof path, TMP "/%s", junk[i]);
        CHECK(thumb(path, &t, &k) == !strcmp(junk[i], "half.avi"));
    }
    CHECK(!thumb(TMP "/none.mp4", &t, &k) && !thumb(TMP, &t, &k)); /* no file; a folder */
    /* a playlist and a script by a video's name would have libavformat open what they list (a
     * file of the card's, an address): they are not opened as what they are */
    snprintf(cmd, sizeof cmd,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n#EXTINF:5,\n%s/"
             "60fps.mp4\n#EXT-X-ENDLIST\n",
             clips);
    text(TMP "/list.mp4", cmd);
    snprintf(cmd, sizeof cmd, "ffconcat version 1.0\nfile '%s/60fps.mp4'\n", clips);
    text(TMP "/script.mkv", cmd);
    CHECK(!thumb(TMP "/list.mp4", &t, &k) && !t.dur_ms && !thumb(TMP "/script.mkv", &t, &k) &&
          !t.dur_ms);
    /* a header that lies about the picture's size: the size the video itself says is the one
     * taken, and the one the bounds are held against */
    size_t len = 0;
    char *avi, *strf;
    snprintf(path, sizeof path, "%s/mpeg4.avi", clips);
    CHECK((avi = file_get(path, &len)) && (strf = memmem(avi, 4096, "strf", 4)));
    memcpy(strf + 12, "\x60\xea\x00\x00\x60\xea\x00\x00", 8); /* 60000 x 60000 */
    CHECK(file_put(TMP "/liar.avi", avi, len) && thumb(TMP "/liar.avi", &t, &k) && t.w == 640 &&
          t.h == 360);
    free(avi);
    /* a picture wider than any video's is not decoded; a strip of one that is, is */
    sh("'%s' -v error -y -f lavfi -i color=red:size=20000x16:rate=1:duration=2 -c:v mjpeg -q:v "
       "10 " TMP "/wide.avi",
       ffmpeg);
    CHECK(!thumb(TMP "/wide.avi", &t, &k) && !t.w && !t.h && t.dur_ms == 2000);
    sh("'%s' -v error -y -f lavfi -i testsrc2=size=16000x16:rate=1:duration=2 -c:v mjpeg -q:v "
       "10 " TMP "/strip.avi",
       ffmpeg);
    CHECK(thumb(TMP "/strip.avi", &t, &k) && t.w == 16000 && t.h == 16);

    /* clips with bytes changed and ends cut off, at random but the same each run: a picture or
     * none, in its time, and the check still standing */
    static const char *const prey[] = { "no-audio.mp4", "vp9-opus.webm", "mpeg4.avi" };
    unsigned seed = 20261122;
    for (size_t c = 0; c < sizeof prey / sizeof *prey; ++c) {
        snprintf(path, sizeof path, "%s/%s", clips, prey[c]);
        char *whole = file_get(path, &len), *bent = malloc(len);
        CHECK(whole && bent && len > 8192);
        for (int round = 0; round < 40; ++round) {
            memcpy(bent, whole, len);
            /* in its first bytes, its last (an MP4's index is there), or anywhere */
            size_t from = round % 3 == 1 ? len - 4096 : 0, span = round % 3 == 2 ? len : 4096;
            for (int flips = 1 + (int)(RND() % 24); flips; --flips) {
                size_t at = from + RND() % span;
                bent[at] ^= (char)(1 + RND() % 255);
            }
            CHECK(file_put(TMP "/bent.bin", bent, round % 5 == 4 ? 1 + RND() % len : len));
            thumb(TMP "/bent.bin", &t, &k);
        }
        free(whole), free(bent);
    }
}

/* ---- Covers and the cache ------------------------------------------------------------------ */

/* the cache's index: every line one of ours; how many, and how many say a thumbnail */
static int index_lines(const char *card, int *pictures) {
    char path[PATH_N], *text, *end;
    int n = 0;
    snprintf(path, sizeof path, "%s/.videos/cache/index", card);
    *pictures = 0;
    if (!(text = file_get(path, 0))) return 0;
    for (char *line = text; (end = strchr(line, '\n')); line = end + 1, ++n) {
        unsigned long long key;
        long long dur;
        int w, h, used = 0;
        char state, jpg[PATH_N];
        *end = 0;
        CHECK(strlen(line) >= 24 && line[16] == '\t');
        CHECK(sscanf(line, "%16llx\t%lld\t%dx%d\t%c%n", &key, &dur, &w, &h, &state, &used) == 5 &&
              !line[used]);
        CHECK(dur >= 0 && w >= 0 && h >= 0 && (state == T_PICTURE || state == T_NONE));
        snprintf(jpg, sizeof jpg, "%s/.videos/cache/%016llx.jpg", card, key);
        CHECK(!access(jpg, 0) == (state == T_PICTURE)); /* a thumbnail for each that says so */
        *pictures += state == T_PICTURE;
    }
    CHECK(!*strchr(text, 0)); /* and it ends with a line's end */
    free(text);
    return n;
}
static int cache_files(void) {
    char line[32] = "";
    FILE *p = popen("ls -A " CACHE " 2>/dev/null | wc -l", "r");
    CHECK(p && fgets(line, sizeof line, p) && !pclose(p));
    return atoi(line);
}

static void check_covers(void) {
    char level[REL_MAX + 1] = "", path[PATH_N], raw[sizeof rp.raw];
    look_t k;
    int pictures;
    /* the library: the files of before, and three clips */
    sh("cp -p '%s/h264-aac.mp4' '%s/rotated.mp4' " V " && cp -p '%s/audio-only.mp4' " V "Series/",
       clips, clips, clips);
    sh("mkdir -p " ART " && cp assets/tile-*.jpg " CARD "/.videos/");
    CHECK(ask("menu") == 9 && covers() == 0 &&
          !art_count()); /* Q2 Pod without covers: none are put */
    text(TMP "/q2-addon.caps", CAPS);

    /* the tiles at once: each folder's, and Settings'; a video has nothing before the worker */
    CHECK(ask("menu") == 9 && covers() == 3 && art_count() == 3);
    memcpy(raw, rp.raw, rp.len + 1);
    CHECK(same_file(art_path("dir", "Movies"), "assets/tile-folder.jpg") &&
          same_file(art_path("dir", "Series"), "assets/tile-folder.jpg"));
    CHECK(same_file(art_path("settings", ""), "assets/tile-settings.jpg"));
    cover_ok(art_path("dir", "Series"), &k), cover_ok(art_path("settings", ""), &k);
    CHECK(k.luma > 60 && k.luma < 130); /* a light glyph on a dark tile */
    CHECK(access(art_path("play", "h264-aac.mp4"), 0) &&
          access(art_path("play", "Episode 1.MP4"), 0));
    SAME(ROW(6, CTX), "h264-aac.mp4"), SAME(ROW(6, SUB), "");
    CHECK(ask("menu") == 9 &&
          !strcmp(rp.raw + 12, raw + 12)); /* asked again: the same bytes, past the id */

    /* the worker's round: a thumbnail for each video that has a picture, in the cache, and a
     * line in the index for every video */
    CHECK(worker_pass(level, -1, -1) == 0);
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);
    const thumb_t *t = index_find(key_of(CARD "/Videos", "h264-aac.mp4"));
    CHECK(t && t->state == T_PICTURE && t->w == 640 && t->h == 360 &&
          llabs(t->dur_ms - 30000) < 200);
    CHECK((t = index_find(key_of(CARD "/Videos", "Series/audio-only.mp4"))) && t->state == T_NONE &&
          t->dur_ms > 4800);
    CHECK((t = index_find(key_of(CARD "/Videos", "Episode 1.MP4"))) && t->state == T_NONE &&
          !t->dur_ms && !t->w);
    /* the next reply has them: the videos' covers, the "no picture" tile for a file that has
     * none, and the lengths */
    CHECK(ask("menu") == 9 && covers() == 9 && art_count() == 9);
    SAME(ROW(6, SUB), "30 s"), SAME(ROW(7, SUB), "5 s"), SAME(ROW(3, SUB), "");
    CHECK(same_file(art_path("play", "h264-aac.mp4"),
                    thumb_file(key_of(CARD "/Videos", "h264-aac.mp4"), path)));
    cover_ok(art_path("play", "h264-aac.mp4"), &k);
    CHECK(k.luma > 60);
    cover_ok(art_path("play", "rotated.mp4"), &k);
    CHECK(k.red_left && !k.red_top);
    CHECK(same_file(art_path("play", "Episode 1.MP4"), "assets/tile-nopicture.jpg"));
    cover_ok(art_path("play", "Episode 1.MP4"), &k);
    memcpy(raw, rp.raw, rp.len + 1);
    CHECK(ask("menu") == 9 && !strcmp(rp.raw + 12, raw + 12) && art_count() == 9);

    /* Continue Watching has its video's cover: the same key, counted for both rows */
    marks_put(&m, "rotated.mp4", size_of(V "rotated.mp4"))->pos_ms = 1000;
    CHECK(marks_save(&m) && ask("menu") == 10 && covers() == 10 && art_count() == 9);
    SAME(ROW(0, CTX), "rotated.mp4"), SAME(ROW(0, SUB), "Less than a minute left \xc2\xb7 rotated");
    marks_free(&m);
    unlink(CARD "/.videos/marks");

    /* another level: the art folder is its own, and the first one's again on the way back;
     * Settings, which has no covers, leaves it alone */
    CHECK(ask("dir\tSeries") == 3 && covers() == 3 && art_count() == 3);
    CHECK(same_file(art_path("dir", "Series/Season 1"), "assets/tile-folder.jpg") &&
          access(art_path("dir", "Series"), 0));
    CHECK(same_file(art_path("play", "Series/audio-only.mp4"), "assets/tile-nopicture.jpg"));
    CHECK(ask("settings") == 6 && covers() == 0 && art_count() == 3);
    CHECK(ask("menu") == 9 && covers() == 9 && art_count() == 9 &&
          !access(art_path("dir", "Series"), 0));

    /* A level longer than Q2 Pod holds at once: its More… row asks for the rows from the 500th
     * as a level of their own, and the art folder is then theirs; Back asks for the first again. */
    sh("for i in $(seq 100 619); do mkdir -p " V "Lots/d$i && : > " V
       "Lots/d$i/v.mp4 || exit 1; done");
    for (int off = 0; off < 500; off += 50) {
        CHECK((off ? ask("dir\tLots\t%d", off) : ask("dir\tLots")) == 50), SAME(MSG, "pages");
        CHECK(art_count() == off + 50);
    }
    SAME(CURSOR, "500");
    /* The level asked again with other rows in it (videos copied meanwhile, another Sort): the
     * covers are its rows' now, though none of the 500 kept is let go by a change of level. */
    sh("for i in 1 2 3; do mkdir -p " V "Lots/c$i && : > " V "Lots/c$i/v.mp4 || exit 1; done");
    CHECK(ask("dir\tLots") == 50 && row_arts() == 50 && art_count() == 50);
    SAME(ROW(0, TITLE), "c1"), SAME(ROW(3, TITLE), "d100");
    CHECK(ask("dir\tLots\t500") == 23 && art_count() == 23), SAME(CURSOR, "");
    CHECK(access(art_path("dir", "Lots/d100"), 0) &&
          same_file(art_path("dir", "Lots/d619"), "assets/tile-folder.jpg"));
    CHECK(ask("dir\tLots") == 50 && art_count() == 50 && !access(art_path("dir", "Lots/d100"), 0));
    sh("rm -rf " V "Lots");
    CHECK(ask("menu") == 9 && covers() == 9 && art_count() == 9);

    /* a video that changed is another video: a new key, its cover gone until the worker has
     * made the new one, and the old thumbnail tidied out of the cache */
    unsigned long long was = key_of(CARD "/Videos", "h264-aac.mp4");
    sh("touch -d @86400 " V "h264-aac.mp4");
    CHECK(key_of(CARD "/Videos", "h264-aac.mp4") != was);
    CHECK(ask("menu") == 9 && covers() == 8 && art_count() == 8 &&
          access(art_path("play", "h264-aac.mp4"), 0));
    SAME(ROW(6, SUB), "");
    CHECK(worker_pass(level, -1, -1) == 0 && access(thumb_file(was, path), 0));
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);
    CHECK(ask("menu") == 9 && covers() == 9 && !access(art_path("play", "h264-aac.mp4"), 0));
    sh("truncate -s +1 " V "rotated.mp4"); /* and by its size */
    CHECK(ask("menu") == 9 && covers() == 8 && worker_pass(level, -1, -1) == 0 &&
          ask("menu") == 9 && covers() == 9);
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);

    /* A cause that is not the file's and may pass: the video has the "no picture" tile while
     * this server runs and is not tried again in that time, but no line is saved for it, so
     * the next server tries it once more. Too little memory free for its frames (the host
     * build's hook: a Q2 with 20 MB free, a 1080p video)... */
    sh("cp -p '%s/big-1080p.mp4' " V, clips);
    unsigned long long big = key_of(CARD "/Videos", "big-1080p.mp4");
    setenv("VIDEOS_MEM_KB", "20000", 1);
    CHECK(ask("menu") == 10 && covers() == 9 && worker_pass(level, -1, -1) == 0);
    unsetenv("VIDEOS_MEM_KB");
    CHECK((t = index_find(big)) && t->state == T_LATER && t->w == 1920 && t->h == 1080);
    CHECK(ask("menu") == 10 && covers() == 10), SAME(ROW(3, CTX), "big-1080p.mp4");
    SAME(ROW(3, SUB), "5 s"); /* what was read of it is shown */
    CHECK(same_file(art_path("play", "big-1080p.mp4"), "assets/tile-nopicture.jpg"));
    CHECK(worker_pass(level, -1, -1) == 0 && index_find(big)->state == T_LATER);
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);
    index_free(), index_load(); /* the next server: its memory is free */
    CHECK(!index_find(big) && worker_pass(level, -1, -1) == 0);
    CHECK((t = index_find(big)) && t->state == T_PICTURE);
    CHECK(index_lines(CARD, &pictures) == 12 && pictures == 3 && cache_files() == 4);
    /* ...and a card that didn't take the thumbnail (a folder where it is written) */
    char block[PATH_N + 8];
    sh("touch -d @86400 " V "big-1080p.mp4"); /* another video, by its key */
    big = key_of(CARD "/Videos", "big-1080p.mp4");
    snprintf(block, sizeof block, "%s.new", thumb_file(big, path));
    CHECK(!mkdir(block, 0755) && worker_pass(level, -1, -1) == 0);
    CHECK((t = index_find(big)) && t->state == T_LATER && t->w == 1920 && !rmdir(block));
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);
    index_free(), index_load();
    CHECK(worker_pass(level, -1, -1) == 0 && index_find(big)->state == T_PICTURE);
    unlink(V "big-1080p.mp4");
    CHECK(worker_pass(level, -1, -1) == 0);
    CHECK(index_lines(CARD, &pictures) == 11 && pictures == 2 && cache_files() == 3);

    /* the cache tidied at a round's start: the lines and thumbnails of videos that are gone,
     * files that are no line's thumbnail, and a line whose thumbnail is gone, made again */
    unsigned long long rot = key_of(CARD "/Videos", "rotated.mp4"),
                       h264 = key_of(CARD "/Videos", "h264-aac.mp4");
    unlink(V "rotated.mp4"), unlink(V "Series/Season 2/c.webm"), unlink(thumb_file(h264, path));
    text(CACHE "/index.new", "torn"), text(CACHE "/0123456789abcdef.jpg", "no line's"),
        text(CACHE "/junk", "x");
    text(CACHE "/0123456789ABCDEF.jpg.new", "x");
    CHECK(worker_pass(level, -1, -1) == 0);
    CHECK(index_lines(CARD, &pictures) == 9 && pictures == 1 && cache_files() == 2);
    CHECK(!index_find(rot) && access(thumb_file(rot, path), 0) &&
          !access(thumb_file(h264, path), 0));
    /* what another process finds there, a server started later: the same lines; a file by
     * another hand has its garbage passed over */
    index_free();
    CHECK(!index_find(h264));
    index_load();
    CHECK((t = index_find(h264)) && t->state == T_PICTURE && t->w == 640 &&
          llabs(t->dur_ms - 30000) < 200);
    sh("printf '%%s\\n' 'garbage' '0123456789abcdef\t5\t1x1\tp' '0123456789abcdeg\t5\t1x1\tp' "
       "'0123456789abcde\t5\t1x1\tp' '1123456789abcdef\t-5\t1x1\tp' '2123456789abcdef\t5\t1x1\tq' "
       "'3123456789abcdef\t5\t1x1\tpp' '4123456789abcdef\t5\t1\tp' "
       "'5123456789abcdef\t5\t99999x1\tp' "
       "'6123456789abcdef\t 5\t1x1\tp' '7123456789ABCDEF\t5\t1x1\tp' 'fedcba9876543210\t7\t2x3\tx' "
       "> " CACHE "/index");
    sh("printf '8123456789abcdef\t5\t1x1\tp' >> " CACHE "/index"); /* torn: no line end */
    index_load();
    CHECK((t = index_find(0x0123456789abcdefull)) && t->dur_ms == 5 && t->w == 1 && t->h == 1 &&
          t->state == T_PICTURE);
    CHECK((t = index_find(0xfedcba9876543210ull)) && t->dur_ms == 7 && t->w == 2 && t->h == 3 &&
          t->state == T_NONE);
    for (unsigned long long i = 1; i <= 8; ++i)
        CHECK(!index_find(0x0123456789abcdefull + (i << 60)));
    CHECK(!index_find(0x7123456789abcdefull));
    /* a card without the Videos folder (or pulled) is not an empty library: its cache stays */
    sh("mv " CARD "/Videos " CARD "/Elsewhere");
    CHECK(worker_pass(level, -1, -1) == 0 && !access(thumb_file(h264, path), 0) &&
          index_find(0xfedcba9876543210ull));
    sh("mv " CARD "/Elsewhere " CARD "/Videos");
    CHECK(worker_pass(level, -1, -1) == 0 && !index_find(0xfedcba9876543210ull) &&
          index_find(h264));
    CHECK(index_lines(CARD, &pictures) == 9 && pictures == 1 && cache_files() == 2);

    /* the worker's order, and what it tells: the level shown first, each video said before it
     * is tried and after; a new level heard between two videos ends the round for the next */
    int to[2], from[2], n = 0;
    thumb_t told[40];
    sh("rm -rf " CACHE);
    index_free();
    CHECK(!pipe(to) && !pipe(from) && !fcntl(to[0], F_SETFL, O_NONBLOCK) &&
          !fcntl(from[0], F_SETFL, O_NONBLOCK));
    char msg[REL_MAX + 1] = "Series/Season 1";
    strcpy(level, "Movies");
    CHECK(write(to[1], msg, sizeof msg) == sizeof msg);
    CHECK(worker_pass(level, to[0], from[1]) == 1),
        SAME(level, "Series/Season 1"); /* after Movies' one video */
    while (read(from[0], &told[n], sizeof *told) == sizeof *told) ++n;
    CHECK(n == 2 && told[0].state == T_TRYING && told[1].state == T_NONE &&
          told[0].key == told[1].key);
    CHECK(told[0].key == key_of(CARD "/Videos", "Movies/Фильм.mkv"));
    CHECK(worker_pass(level, to[0], from[1]) == 0); /* then that level's, then the rest */
    for (n = 0; read(from[0], &told[n], sizeof *told) == sizeof *told;) ++n;
    CHECK(n == 16 && told[0].key == key_of(CARD "/Videos", "Series/Season 1/a.mkv") &&
          told[1].key == told[0].key);
    CHECK(told[2].key == key_of(CARD "/Videos", "Series/Season 1/b.avi") &&
          told[2].state == T_TRYING);
    CHECK(told[5].key == key_of(CARD "/Videos", "100% Real\xe2\x80\xa6 (2019).mp4") &&
          told[5].state == T_NONE);
    CHECK(worker_pass(level, to[0], from[1]) == 0 &&
          read(from[0], told, sizeof *told) < 0); /* nothing left to do */
    /* the server gone (its end of the pipe shut): the round ends after the video it is on */
    file(V "late.mp4", 1, 0), file(V "later.mp4", 1, 0);
    close(to[1]);
    CHECK(worker_pass(level, to[0], from[1]) == -1);
    CHECK(index_find(key_of(CARD "/Videos", "late.mp4")) &&
          !index_find(key_of(CARD "/Videos", "later.mp4")));
    unlink(V "late.mp4"), unlink(V "later.mp4");
    close(to[0]), close(from[0]), close(from[1]);
    index_free();
}

/* ---- The server itself --------------------------------------------------------------------- */

#define ECARD "build/e2e-card"
#define ETMP "build/e2e-tmp"

static pid_t server;
/* 1 when the server takes a connection (a socket file left by an earlier run takes none) */
static int up(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0),
        ok = !connect(fd, (struct sockaddr *)&addr, sizeof addr);
    close(fd);
    return ok;
}
/* The server started as run starts it (cmd then "serve"), on ECARD and ETMP; with env, the
 * NAME=value of the host build's test hooks, blanks between, when given. Back once its socket
 * answers. */
static void start(char **cmd, const char *env) {
    char *argv[16];
    int n = 0;
    while (cmd[n] && n < 14) argv[n] = cmd[n], ++n;
    argv[n] = "serve", argv[n + 1] = 0;
    setenv("VIDEOS_CARD", ECARD, 1), setenv("VIDEOS_TMP", ETMP, 1);
    paths_init();
    sock = ETMP "/q2-videos.browse.sock";
    if (!(server = fork())) {
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* a check that fails leaves no server behind */
        for (char *e = env ? strtok(strdup(env), " ") : 0; e; e = strtok(0, " ")) putenv(e);
        execvp(argv[0], argv);
        _exit(127);
    }
    CHECK(server > 0);
    for (long long end = now_ms() + 20000; !up(); nap(20))
        CHECK(now_ms() < end && !waitpid(server, 0, WNOHANG));
}
/* The server's end, awaited ms at most: its exit status, -1 when a signal ended it, -2 when it
 * is still there. */
static int ended(int ms) {
    int status = 0;
    for (long long end = now_ms() + ms; !waitpid(server, &status, WNOHANG); nap(10))
        if (now_ms() >= end) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
/* TERM, as Q2 Pod's launcher sends it: gone within a second, with status 0, its socket and its
 * art folder with it. */
static void stop(void) {
    CHECK(!kill(server, SIGTERM) && ended(1000) == 0);
    CHECK(access(sock, 0) && access(ETMP "/q2-videos.art", 0));
}

/* The top level asked every 200 ms, as a live level is asked again, until each of its videos
 * has a cover, within ms. Each cover is looked at: a thumbnail not black where the clip has a
 * picture (pictures of them in all), the "no picture" tile where it has none. */
static void await_covers(int ms, int rows, int pictures) {
    look_t k;
    int n = 0, had = -1, changes = 0;
    for (long long end = now_ms() + ms; n < rows; nap(200)) {
        CHECK(now_ms() < end && ask("menu") == rows + 1); /* and Settings */
        SAME(STATUS, "ok");
        n = covers() - 1;
        changes += n != had, had = n;
    }
    CHECK(changes > 1); /* they came while the list was shown */
    for (int i = 0; i < rows; ++i) {
        if (strcmp(ROW(i, KIND), "screen")) continue;
        cover_ok(art_path("play", ROW(i, CTX)), &k);
        if (same_file(art_path("play", ROW(i, CTX)), "assets/tile-nopicture.jpg")) continue;
        CHECK(k.luma > 60 && strstr(ROW(i, SUB), " s")); /* its length too */
        --pictures;
    }
    CHECK(!pictures);
}

/* The card of the run: these clips at the top, with killer.mp4, a copy of one, and huge.avi,
 * made here; and two in a folder. Four have no picture to make a thumbnail of: one has none,
 * one has a song's cover alone, one is in a codec the Q2 has no decoder for, and huge.avi's is
 * 8192 x 4320, a frame the worker's cap on one allocation of FFmpeg's refuses. offset.ts is a
 * transport stream with one keyframe. */
static const char *const TOP[] = { "60fps.mp4",    "anamorphic.mp4", "audio-only.mp4",
                                   "av1.mkv",      "big-1080p.mp4",  "cover.mp4",
                                   "h264-aac.mp4", "hevc.mp4",       "mpeg4.avi",
                                   "no-audio.mp4", "offset.ts",      "rotated.mp4",
                                   "sidecar.mp4",  "vp9-opus.webm" };
enum {
    CLIPS = sizeof TOP / sizeof *TOP,
    TOPS = CLIPS + 2, /* the videos at the top */
    BLANK = 4,        /* audio-only.mp4, cover.mp4, av1.mkv, huge.avi */
    ROWS = TOPS + 2,  /* the top's rows: the folder, the videos, Settings */
    ALL = TOPS + 2    /* the card's videos */
};

/* The server's worker, its child: its pid, 0 when it has none. */
static pid_t worker_of(pid_t pid) {
    char cmd[64], out[32] = "";
    snprintf(cmd, sizeof cmd, "pgrep -P %d", (int)pid);
    FILE *p = popen(cmd, "r");
    CHECK(p && (fgets(out, sizeof out, p), pclose(p) >= 0));
    return atoi(out);
}
/* 1 when the process pid is no more, or is only waiting to be reaped. */
static int dead(pid_t pid) {
    char path[64], *stat, *state;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (!(stat = file_get(path, 0))) return 1;
    int dead = !(state = strrchr(stat, ')')) || state[2] == 'Z' || state[2] == 'X';
    free(stat);
    return dead;
}

static void check_server(char **cmd, int host) {
    look_t k;
    int pictures, lock;
    sh("rm -rf " ECARD " " ETMP " && mkdir -p " ECARD "/Videos/Series " ECARD "/.videos " ETMP);
    for (int i = 0; i < CLIPS; ++i) sh("cp -p '%s/%s' " ECARD "/Videos/", clips, TOP[i]);
    sh("cp -p '%s/ass.mkv' '%s/two-audio-subs.mkv' " ECARD "/Videos/Series/", clips, clips);
    sh("cp assets/tile-*.jpg " ECARD "/.videos/ && cp " ECARD "/Videos/60fps.mp4 " ECARD
       "/Videos/killer.mp4");
    sh("'%s' -v error -y -f lavfi -i color=red:size=8192x4320:rate=1:duration=2 -c:v mjpeg -q:v "
       "10 " ECARD "/Videos/huge.avi",
       ffmpeg);
    text(ETMP "/q2-addon.caps", CAPS "\n");
    text(ETMP "/q2-videos.art.stale", "x"); /* not ours */
    sh("mkdir " ETMP "/q2-videos.art && touch " ETMP "/q2-videos.art/deadbeef.jpg " ETMP
       "/q2-videos.browse.sock");

    /* its start: an earlier run's socket and covers gone, the list answered before any thumbnail */
    start(cmd, host ? "VIDEOS_CRASH=killer VIDEOS_MEM_KB=4000000" : 0);
    CHECK(ask("menu") == ROWS && covers() == 2 && access(ETMP "/q2-videos.art/deadbeef.jpg", 0));
    row_is(0, "list", "dir", "Series", "2 videos", "Series", "");
    row_is(ROWS - 1, "list", "settings", "Settings", "", "", "");
    /* The thumbnails, as they come. The host build's worker dies on killer.mp4 (the checks'
     * hook, a decoder's crash): the server answers on, that video has the "no picture" tile, and
     * the others their thumbnails. Its memory is said to be 4 GB free, so that huge.avi is
     * refused by the cap and not for the memory of the computer the check runs on. */
    await_covers(host ? 60000 : 240000, ROWS - 1, TOPS - BLANK - host);
    CHECK(ask("dir\tSeries") == 2);
    for (long long end = now_ms() + 60000; covers() < 2; nap(200))
        CHECK(now_ms() < end && ask("dir\tSeries") == 2);
    CHECK(index_lines(ECARD, &pictures) == ALL && pictures == ALL - BLANK - host);
    index_load();
    CHECK(index_find(key_of(ECARD "/Videos", "killer.mp4"))->state == (host ? T_NONE : T_PICTURE));
    CHECK(index_find(key_of(ECARD "/Videos", "huge.avi"))->state == T_NONE);
    CHECK(index_find(key_of(ECARD "/Videos", "offset.ts"))->state == T_PICTURE);
    index_free();

    /* a thumbnail deleted from the cache is made again, once its row's cover is looked for anew
     * (the level opened again) */
    char gone[PATH_N];
    snprintf(gone, sizeof gone, ECARD "/.videos/cache/%016llx.jpg",
             key_of(ECARD "/Videos", "mpeg4.avi"));
    CHECK(!unlink(gone) && ask("menu") == ROWS);
    for (long long end = now_ms() + 60000; access(gone, 0) || covers() < ROWS; nap(200))
        CHECK(now_ms() < end && ask("menu") == ROWS);
    cover_ok(art_path("play", "mpeg4.avi"), &k);
    CHECK(same_file(art_path("play", "mpeg4.avi"), gone));

    /* requests that are none, over the socket: answered "bad", and the server still there */
    SAME(status_text("nonsense"), "bad"), SAME(status_text(""), "bad");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
    char reply[64] = "";
    long long began = now_ms();
    /* one that never ends its request is let go after 2 s, with "bad"; nothing waits behind it
     * for longer */
    CHECK(!connect(fd, (struct sockaddr *)&addr, sizeof addr) &&
          write(fd, "Q2SPOT/1\n1\tme", 13) == 13);
    CHECK(read(fd, reply, sizeof reply - 1) > 0 && strstr(reply, "\tbad\t") &&
          now_ms() - began < 3500);
    close(fd);
    CHECK(act("set\tnext") == 0), SAME(STATUS, "ok");
    CHECK(act("mark\tSeries/ass.mkv") == 0), SAME(MSG, "Marked as watched");
    CHECK(ask("dir\tSeries") == 2), SAME(ROW(0, SUB), "Watched");

    /* while a video plays (the player's lock) the worker makes nothing: a new video waits */
    CHECK((lock = lock_take(1)) >= 0);
    sh("cp " ECARD "/Videos/mpeg4.avi " ECARD "/Videos/new.avi");
    CHECK(ask("menu") == ROWS + 1 && covers() == ROWS);
    nap(2500);
    CHECK(ask("menu") == ROWS + 1 && covers() == ROWS && index_lines(ECARD, &pictures) == ALL);
    close(lock);
    for (long long end = now_ms() + 60000; covers() < ROWS + 1; nap(200))
        CHECK(now_ms() < end && ask("menu") == ROWS + 1);
    CHECK(index_lines(ECARD, &pictures) == ALL + 1 && pictures == ALL + 1 - BLANK - host);
    stop();

    /* started again: the cache is there, so every cover is in its first reply; and it is tidied
     * of a video that left the card meanwhile, its line and its thumbnail */
    char left[PATH_N], line[20], *lines = 0;
    snprintf(left, sizeof left, ECARD "/.videos/cache/%016llx.jpg",
             key_of(ECARD "/Videos", "new.avi"));
    snprintf(line, sizeof line, "%016llx\t", key_of(ECARD "/Videos", "new.avi"));
    CHECK(!access(left, 0) && !unlink(ECARD "/Videos/new.avi"));
    start(cmd, 0);
    CHECK(ask("menu") == ROWS && covers() == ROWS);
    for (long long end = now_ms() + 60000;; nap(100)) {
        CHECK(now_ms() < end && (lines = file_get(ECARD "/.videos/cache/index", 0)));
        int there = strstr(lines, line) != 0;
        free(lines);
        if (!there) break;
    }
    CHECK(access(left, 0) && index_lines(ECARD, &pictures) == ALL &&
          pictures == ALL - BLANK - host);
    stop();
    /* a TERM while the worker is in the middle of a library */
    sh("rm -rf " ECARD "/.videos/cache");
    for (int i = 0; i < 40; ++i)
        sh("cp " ECARD "/Videos/big-1080p.mp4 " ECARD "/Videos/copy%d.mp4", i);
    start(cmd, 0);
    CHECK(ask("menu") == 50), SAME(MSG, "pages");
    nap(host ? 100 : 1500);
    /* a player starting meanwhile has the lock once the video in the making is done (that one,
     * and one more if it began as the wait did), and keeps it: nothing is made under it */
    began = now_ms();
    int before = index_lines(ECARD, &pictures);
    CHECK((lock = lock_take(1)) >= 0 && now_ms() - began < THUMB_MS + 1000);
    int had = index_lines(ECARD, &pictures);
    nap(1200);
    CHECK(index_lines(ECARD, &pictures) == had && had < ALL + 40); /* it was not through */
    CHECK(had - before <= 2);
    close(lock);
    stop();
    sh("rm -rf " ECARD "/Videos/copy*.mp4");

    /* The launcher gone without its TERM (its shell killed, its trap not run): the server ends
     * as on a TERM, its socket and art folder with it, and so is not there to take a later
     * server's when it would have idled out. */
    pid_t shell = fork();
    if (!shell) {
        start(cmd, 0);
        pause();
        _exit(0);
    }
    CHECK(shell > 0);
    for (long long end = now_ms() + 20000; !up(); nap(20)) CHECK(now_ms() < end);
    CHECK(ask("menu") == ROWS && !kill(shell, SIGKILL) && waitpid(shell, 0, 0) == shell);
    for (long long end = now_ms() + 2000; !access(sock, 0) || !access(ETMP "/q2-videos.art", 0);
         nap(20))
        CHECK(now_ms() < end);
    if (!host) return;

    /* A decoder that never comes back (the host build's hook, on hang.mp4): the server ends that
     * worker after its time, here a second; the video has the "no picture" tile, the others
     * their thumbnails, and the lock is free again for a player. */
    sh("rm -rf " ECARD "/.videos/cache && cp " ECARD "/Videos/60fps.mp4 " ECARD "/Videos/hang.mp4");
    start(cmd, "VIDEOS_HANG=hang VIDEOS_STUCK_MS=1000");
    CHECK(ask("menu") == ROWS + 1);
    for (long long end = now_ms() + 60000; covers() < ROWS + 1; nap(200))
        CHECK(now_ms() < end && ask("menu") == ROWS + 1);
    CHECK(same_file(art_path("play", "hang.mp4"), "assets/tile-nopicture.jpg"));
    cover_ok(art_path("play", "killer.mp4"), &k);
    CHECK(k.luma > 60 && index_lines(ECARD, &pictures) == ALL + 1 && pictures == ALL - BLANK);
    began = now_ms();
    CHECK((lock = lock_take(1)) >= 0 && now_ms() - began < 1000);
    close(lock);
    stop();
    unlink(ECARD "/Videos/hang.mp4");

    /* A worker the kernel killed for want of memory (here by hand, a second into its hang on
     * oom.mp4): not the video's doing. It has the "no picture" tile while this server runs,
     * the worker started next passes it by, and no line is saved for it... */
    sh("cp " ECARD "/Videos/60fps.mp4 " ECARD "/Videos/oom.mp4");
    start(cmd, "VIDEOS_HANG=oom");
    for (long long end = now_ms() + 15000; access(art_path("play", "oom.mp4"), 0); nap(100)) {
        pid_t w = worker_of(server);
        CHECK(now_ms() < end && ask("menu") == ROWS + 1);
        if (w > 0) nap(1000), kill(w, SIGKILL);
    }
    CHECK(same_file(art_path("play", "oom.mp4"), "assets/tile-nopicture.jpg"));
    nap(3000); /* the next worker is started within two seconds, and is soon through */
    for (long long end = now_ms() + 5000; worker_of(server); nap(100)) CHECK(now_ms() < end);
    CHECK(ask("menu") == ROWS + 1 && covers() == ROWS + 1);
    CHECK(index_lines(ECARD, &pictures) == ALL && pictures == ALL - BLANK);
    stop();
    /* ...so the next server tries it once more: its thumbnail */
    start(cmd, 0);
    for (long long end = now_ms() + 60000; access(art_path("play", "oom.mp4"), 0); nap(200))
        CHECK(now_ms() < end && ask("menu") == ROWS + 1);
    cover_ok(art_path("play", "oom.mp4"), &k);
    CHECK(k.luma > 60 && index_lines(ECARD, &pictures) == ALL + 1 && pictures == ALL + 1 - BLANK);
    stop();
    unlink(ECARD "/Videos/oom.mp4");

    /* A server killed outright leaves no worker behind, even one that would never come back
     * from a decoder. */
    sh("cp " ECARD "/Videos/60fps.mp4 " ECARD "/Videos/orphan.mp4");
    start(cmd, "VIDEOS_HANG=orphan");
    pid_t orphan;
    for (long long end = now_ms() + 5000; (orphan = worker_of(server)) <= 0; nap(50))
        CHECK(now_ms() < end);
    nap(500); /* it hangs by now */
    CHECK(!dead(orphan) && !kill(server, SIGKILL) && ended(1000) == -1);
    for (long long end = now_ms() + 2000; !dead(orphan); nap(20))
        if (now_ms() >= end) kill(orphan, SIGKILL), CHECK(!"a worker left behind");
    unlink(ECARD "/Videos/orphan.mp4");

    /* Idleness, the ten minutes as 1.5 s through the host build's hook: the server ends itself
     * with status 0 (run's loop then ends), counted from the last request... */
    start(cmd, "VIDEOS_IDLE_MS=1500");
    began = now_ms();
    nap(1000);
    CHECK(ask("menu") == ROWS); /* a request at 1 s: 1.5 s from then */
    CHECK(ended(1200) == -2 && ended(2500) == 0 && now_ms() - began > 2400);
    CHECK(access(sock, 0) && access(ETMP "/q2-videos.art", 0));
    /* ...and never while a video plays (the player's lock), during which Q2 Pod asks nothing,
     * however long; the time is counted anew from the player's end. The server looks at the
     * lock once a second, so with 3 s to wait it ends 2 to 3 s after the lock is let go; one
     * that counted from its last request would end within the first of them. */
    CHECK((lock = lock_take(1)) >= 0);
    start(cmd, "VIDEOS_IDLE_MS=3000");
    CHECK(ended(4500) == -2);
    close(lock);
    CHECK(ended(1500) == -2 && ended(2500) == 0);
    CHECK(access(sock, 0) && access(ETMP "/q2-videos.art", 0));
}

int main(int argc, char **argv) {
    static char at[2][PATH_N], clip[PATH_N + 16];
    static char *host[] = { "build/host/q2-videos", 0 };
    snprintf(at[0], PATH_N, "%s/.cache/q2-videos/clips", getenv("HOME"));
    snprintf(at[1], PATH_N, "%s/.cache/q2-videos/ffmpeg-4.2/host/bin/ffmpeg", getenv("HOME"));
    clips = getenv("VIDEOS_CLIPS") ? getenv("VIDEOS_CLIPS") : at[0];
    ffmpeg = getenv("VIDEOS_FFMPEG") ? getenv("VIDEOS_FFMPEG") : at[1];
    snprintf(clip, sizeof clip, "%s/h264-aac.mp4", clips);
    if (access(clip, 0) || access(ffmpeg, X_OK))
        return fprintf(stderr,
                       "check_serve: no clips in %s (tools/clips.sh) or no %s (tools/ffmpeg.sh)\n",
                       clips, ffmpeg),
               1;
    av_log_set_level(AV_LOG_QUIET);
    if (argc > 1) { /* another build's server: the Q2's, under qemu */
        check_server(argv + 1, 0);
        puts("check_serve: ok, the server as given");
        return 0;
    }
    sh("rm -rf " CARD " " TMP " && mkdir -p " CARD " " TMP);
    setenv("VIDEOS_CARD", CARD, 1), setenv("VIDEOS_TMP", TMP, 1);
    paths_init();
    check_framing();
    check_levels();
    check_paging();
    check_contexts();
    check_actions();
    check_bent_requests();
    check_thumbs();
    check_covers();
    check_server(host, 1);
    puts("check_serve: ok");
    return 0;
}
