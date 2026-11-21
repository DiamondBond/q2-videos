/* The page's server: Videos' lists over Q2SPOT/1 (Q2 Pod's docs/librespot.md) on
 * tmp_dir/q2-videos.browse.sock, one request a connection, one at a time. Q2 Pod draws the rows;
 * a video's row is of kind screen, which has Q2 Pod run this same binary as the player. The
 * rows' covers are files in tmp_dir/q2-videos.art, copied there from the app's tiles and from the
 * thumbnails the worker (thumb.c) leaves in the card's cache; no reply waits for the worker. */
#include "serve.h"
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IDLE_MS 600000 /* no request and no video playing this long: the server ends itself */
#define REQ_MS 2000    /* a request is read, and its reply taken, within this */
#define HEAD_MAX 128   /* a reply's first two lines at most */
/* A worker this long on one video is ended. Its own clock lets a video go after THUMB_MS, but is
 * looked at between two calls of FFmpeg's, not inside one that never comes back; and the worker
 * holds the lock the player waits for. */
#define STUCK_MS (4 * THUMB_MS)
/* The covers the art folder holds at most: those of the rows Q2 Pod holds of one level, ten
 * replies of ROWS_MAX (addon.c ADDON_PAGES), after which its More… row opens the next as a level
 * of their own. A tile is 2 kB and a thumbnail about 6, so 3 MB of /tmp at the very most. */
#define COVERS (10 * ROWS_MAX)

static volatile sig_atomic_t quit;
static void on_term(int sig) {
    (void)sig;
    quit = 1;
}

static long long now_ms(void) { return mono_ms(); }

/* ---- Replies ------------------------------------------------------------------------------- */

typedef struct {
    char rows[REPLY_MAX - HEAD_MAX];
    size_t len;
    int count;
    const char *status;
    char msg[64], cursor[12]; /* msg: as it goes out, escaped */
} rep_t;

int q2_escape(const char *s, char *out, size_t room, size_t max) {
    size_t n = strlen(s), k = 0;
    if (n > max) /* cut before a character's first byte, never inside one */
        for (n = max; n && ((unsigned char)s[n] & 0xc0) == 0x80;) --n;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        int esc = c == '%' || c == '\t' || c == '\n' || c == '\r';
        if (k + (esc ? 3 : 1) > room) return -1;
        if (esc)
            out[k++] = '%', out[k++] = "0123456789ABCDEF"[c >> 4],
            out[k++] = "0123456789ABCDEF"[c & 15];
        else
            out[k++] = (char)c;
    }
    return (int)k;
}

static int hex(char c) {
    return c >= '0' && c <= '9'                 ? c - '0'
           : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                                                : -1;
}

int q2_unescape(char *s) {
    char *o = s;
    for (; *s; ++o) {
        int v = *s != '%'                          ? (unsigned char)*s
                : hex(s[1]) >= 0 && hex(s[2]) >= 0 ? hex(s[1]) << 4 | hex(s[2])
                                                   : 0;
        if (!v) return 0;
        s += *s == '%' ? 3 : 1;
        *o = (char)v;
    }
    *o = 0;
    return 1;
}

/* A row added to the reply. 0 when the reply has no room for it: ROWS_MAX rows, or its bytes. */
static int row(rep_t *r, const char *kind, const char *uri, const char *title, const char *sub,
               const char *ctx, const char *position) {
    const char *f[] = { kind, uri, title, sub, ctx, position };
    size_t at = r->len;
    if (r->count == ROWS_MAX) return 0;
    for (int i = 0; i < 6; ++i) {
        /* what is shown is cut; a uri and a context never are. One byte is the tab's or the
         * line end's */
        int k = at < sizeof r->rows ? q2_escape(f[i], r->rows + at, sizeof r->rows - at - 1,
                                                i == 2 || i == 3 ? TEXT_MAX : REQ_MAX)
                                    : -1;
        if (k < 0) return 0;
        at += (size_t)k;
        r->rows[at++] = i < 5 ? '\t' : '\n';
    }
    r->len = at;
    return ++r->count;
}

static void fail(rep_t *r, const char *status, const char *msg) {
    r->status = status;
    snprintf(r->msg, sizeof r->msg, "%s", msg);
}

/* 1 when Q2 Pod has the card-app feature word: tmp_dir/q2-addon.caps is one line of them, which
 * every scan of its writes anew. Firmware that writes no file has none of them. */
static int has_cap(const char *word) {
    char path[PATH_N];
    size_t n = strlen(word);
    int has = 0;
    snprintf(path, sizeof path, "%s/q2-addon.caps", tmp_dir);
    char *caps = file_get(path, 0);
    for (const char *s = caps; s && *s && !has; s += strcspn(s, " \t\r\n")) {
        s += strspn(s, " \t\r\n");
        has = !strncmp(s, word, n) && (!s[n] || strchr(" \t\r\n", s[n]));
    }
    free(caps);
    return has;
}

/* ---- Covers -------------------------------------------------------------------------------- */

/* A row's cover is tmp_dir/q2-videos.art/<key>.jpg (art_key), which Q2 Pod's UI thread reads: so
 * the files are in /tmp, copies, each written whole and renamed. The server alone writes there,
 * and art says what it has put: for each row's key, what the cover is of. The folder holds the
 * covers of the level shown and no other's. */
enum { C_NONE, C_FOLDER, C_SETTINGS, C_NOPICTURE, C_THUMB };
static const char *const TILE[] = { 0, "tile-folder.jpg", "tile-settings.jpg",
                                    "tile-nopicture.jpg" };
static struct {
    unsigned key;
    int of;                   /* C_: a tile, or a video's thumbnail */
    unsigned long long thumb; /* C_THUMB: the thumbnail's key in the cache */
} art[COVERS];
static int arts;
static char shown[REL_MAX + 1]; /* the level they are of, and the row its COVERS rows start at */
static int shown_off;

static const char *art_file(const char *name, char *path) {
    snprintf(path, PATH_N, "%s/q2-videos.art/%s", tmp_dir, name);
    return path;
}

/* The folder emptied (made, at the start; an earlier run's files go too). */
static void art_clear(void) {
    char path[PATH_N];
    struct dirent *e;
    mkdir(art_file("", path), 0755);
    DIR *d = opendir(path);
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') unlink(art_file(e->d_name, path));
    if (d) closedir(d);
    arts = 0;
}

/* The row whose key is key has the cover it should have: of's (C_THUMB: the cache's thumb), or
 * none for C_NONE. 1 when it has one now. A cover that is no longer the row's goes: a video that
 * changed is another video, with a thumbnail yet to make. */
static int cover(unsigned key, int of, unsigned long long thumb) {
    char name[16], path[PATH_N], from[PATH_N];
    unsigned char *jpg = 0;
    struct stat st;
    size_t n = 0;
    int i = 0;
    while (i < arts && art[i].key != key) ++i;
    if (i < arts && art[i].of == of && art[i].thumb == thumb) return 1;
    snprintf(name, sizeof name, "%08x.jpg", key);
    art_file(name, path);
    if (of == C_THUMB)
        thumb_file(thumb, from);
    else if (of)
        snprintf(from, sizeof from, "%s/%s", state_dir, TILE[of]);
    /* a JPEG of a cover's size and nothing else: the card's files are not ours alone */
    if (of && i < COVERS && !stat(from, &st) && S_ISREG(st.st_mode) && st.st_size <= THUMB_BYTES)
        jpg = (unsigned char *)file_get(from, &n);
    int ok = jpg && n > 3 && n <= THUMB_BYTES && jpg[0] == 0xff && jpg[1] == 0xd8 &&
             file_put(path, jpg, n);
    free(jpg);
    if (ok) {
        art[i].key = key, art[i].of = of, art[i].thumb = thumb;
        arts += i == arts;
    } else if (i < arts) {
        unlink(path);
        art[i] = art[--arts];
    }
    return ok;
}

/* "refresh" into msg, spelt for the number of covers the reply's rows have. Q2 Pod looks at a
 * live level's reply only when its bytes differ from the one it shows (addon.c got(), a hash of
 * them), and only then counts the rows' covers again to draw the list anew. A thumbnail that
 * lands while no row's text changes (a video in progress, one already watched, one that can't be
 * decoded) would never show. So the count is in the bytes: each letter of the word whose bit is
 * set in it is written as its escape, which reads back as the same word. */
static void live(char *msg, int covers) {
    for (int i = 0; i < 7; ++i)
        msg += sprintf(msg, covers >> i & 1 ? "%%%02X" : "%c", "refresh"[i]);
}

/* ---- The worker ---------------------------------------------------------------------------- */

/* The worker is a child process (thumb.c says why), started when a level lists a video the index
 * has no line for, and gone once every video has one; and once at the server's start whatever
 * the levels list, since its round begins by tidying the cache of the videos that left the card.
 * It is told the level shown, on to, and tells what it is on and what it found, on from. */
static struct {
    pid_t pid;
    int to, from;
    int want;                  /* one is to start: a level has work for it, or the start's */
    int off;                   /* none is started any more: it can't work here */
    int told, fails;           /* lines it told since it started; deaths in a row on no video */
    long long next;            /* when one may start, a moment after one died */
    unsigned long long trying; /* the video it is on, 0 none, and since when */
    long long since;
    char level[REL_MAX + 1]; /* the level last asked for, and whether it was told of it */
    int sent;
} wk = { .to = -1, .from = -1, .want = 1 };
static int listener = -1; /* the socket, which a worker has no business holding */

/* The level at rel lists a video without a line in the index: the worker does that level first. */
static void worker_ask(const char *rel) {
    wk.sent = wk.sent && !strcmp(rel, wk.level);
    snprintf(wk.level, sizeof wk.level, "%s", rel);
    wk.want = 1;
    /* whole or not at all (the pipe is not waited for): a full pipe's worker hears the next */
    if (wk.pid > 0 && !wk.sent)
        wk.sent = write(wk.to, wk.level, sizeof wk.level) == (ssize_t)sizeof wk.level;
}

static void worker_start(void) {
    int to[2], from[2];
    pid_t self = getpid();
    if (!wk.want || wk.pid > 0 || wk.off || now_ms() < wk.next) return;
    if (pipe2(to, O_CLOEXEC)) return;
    if (pipe2(from, O_CLOEXEC)) return (void)(close(to[0]), close(to[1]));
    fcntl(to[1], F_SETFL, O_NONBLOCK), fcntl(from[0], F_SETFL, O_NONBLOCK);
    if (!(wk.pid = fork())) {
        signal(SIGTERM, SIG_DFL), signal(SIGINT, SIG_DFL), signal(SIGPIPE, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* never left behind by a server that was killed */
        if (getppid() != self) _exit(0);
        close(to[1]), close(from[0]), close(listener);
        worker_main(wk.level, to[0], from[1]);
    }
    close(to[0]), close(from[1]);
    if (wk.pid < 0) {
        close(to[1]), close(from[0]);
        wk.next = now_ms() + 5000;
        return;
    }
    wk.to = to[1], wk.from = from[0];
    wk.want = wk.told = 0, wk.sent = 1, wk.trying = 0;
}

/* What the worker told, taken; and its end, when it has shut its pipe. One that died takes the
 * video it was on with it: that video gets the "no picture" tile and is not tried again, and
 * another worker goes on past it. */
static void worker_read(void) {
    thumb_t t;
    ssize_t k;
    int status = 0;
    while ((k = read(wk.from, &t, sizeof t)) == (ssize_t)sizeof t)
        if (t.state == T_TRYING)
            wk.trying = t.key, wk.since = now_ms();
        else
            index_put(&t), wk.trying = 0, wk.fails = 0, ++wk.told;
    if (k) return; /* nothing more for now */
    waitpid(wk.pid, &status, 0);
    close(wk.to), close(wk.from);
    wk.pid = 0, wk.to = wk.from = -1;
    if (WIFEXITED(status) && !WEXITSTATUS(status)) {
        /* through. A level that still lists a video without a line starts another; one that
         * then has nothing to tell either is not started again for a while */
        wk.want = 0, wk.next = now_ms() + (wk.told ? 0 : 30000);
        return;
    }
    if (wk.trying) index_put(&(thumb_t){ .key = wk.trying, .state = T_NONE });
    wk.off = !wk.trying && ++wk.fails >= 3;
    wk.want = 1, wk.trying = 0, wk.next = now_ms() + 1000;
}

static void worker_stop(void) {
    if (wk.pid <= 0) return;
    kill(wk.pid, SIGKILL); /* it holds nothing to put right: a write cut short is a .new, tidied */
    waitpid(wk.pid, 0, 0);
    close(wk.to), close(wk.from);
    wk.pid = 0, wk.to = wk.from = -1;
}

/* ---- Levels -------------------------------------------------------------------------------- */

static marks_t marks; /* 24 kB: not the stack's */

/* 1 when rel can be a row's context: Q2 Pod must be able to send it back (CTX_MAX). */
static int ctx_ok(const char *rel) {
    char esc[CTX_MAX];
    return q2_escape(rel, esc, sizeof esc, REL_MAX) >= 0;
}

/* A video's subtitle, by its state: what is left of one in progress, Watched, else its length,
 * and its size until the worker has read that. */
static const char *state_text(const mark_t *m, const thumb_t *t, long long size, char *out,
                              size_t n) {
    long long dur = m && m->dur_ms ? m->dur_ms : t ? t->dur_ms : 0;
    if (mark_unfinished(m)) return text_left(m->pos_ms, dur, out, n);
    if (m && m->flags & MARK_WATCHED) return snprintf(out, n, "Watched"), out;
    return dur ? text_length(dur, out, n) : text_size(size, out, n);
}

enum { I_CONTINUE, I_FOLDER, I_VIDEO, I_NONE, I_SETTINGS };
typedef struct {
    int type, i;      /* i: a folder's or a video's entry in the listing */
    const thumb_t *t; /* a video's line of the index, once it has one */
} item_t;

/* The level at rel ("" the top), from its row off on. Its rows: at the top Continue Watching,
 * when a video is unfinished; the folders that hold videos; the videos; at the top, Settings. A
 * level of LIVE_ROWS rows at most is live ("refresh"); a longer one is paged ("pages"), its
 * cursor the row to go on from.
 * ponytail: a cursor counts rows, so a video that comes or goes between two pages shifts the
 * rest by one (a row twice, or one missed, until the list is opened again); a name to go on
 * after, if a library changes under a list often enough to show. */
static void level(rep_t *r, const char *rel, int off) {
    char root[PATH_N], path[PATH_N], ctx[REL_MAX + 1], name[256], text[64], sub[400];
    char cw_rel[REL_MAX + 1] = "";
    struct stat st;
    settings_t set;
    listing_t l;
    mark_t cw = { 0 };
    int top = !*rel, n = 0, sent = 0, covers = 0, missing = 0;

    if (!has_cap("screen")) { /* Q2 Pod 1.8.9 and older: no row of ours would do anything */
        row(r, "info", "", "Update Q2 Pod to play videos", "This app needs Q2 Pod 1.9.0 or later",
            "", "");
        return;
    }
    int arty = has_cap("art");
    settings_load(&set);
    marks_load(&marks);
    memset(&l, 0, sizeof l);
    int found = lib_root(root, sizeof root) && lib_list(root, rel, set.sort, &l);
    item_t *item = calloc((size_t)l.n + 3, sizeof *item);
    if (!item || (!found && !top)) {
        fail(r, item ? "unavailable" : "error", item ? "Folder not found" : "Out of memory");
        free(item), lib_free(&l);
        return;
    }

    /* its mark is copied: listing a video whose size has changed drops that video's mark, and
     * moves another into its place */
    const mark_t *k = top && found ? lib_continue(root, &marks) : 0;
    if (k && ctx_ok(k->rel) && lib_path(root, k->rel, path, sizeof path) && !stat(path, &st)) {
        cw = *k, snprintf(cw_rel, sizeof cw_rel, "%s", k->rel);
        item[n++] =
            (item_t){ I_CONTINUE, 0, index_find(cache_key(cw_rel, st.st_size, st.st_mtime)) };
    }
    int first = n;
    for (int i = 0; i < l.n; ++i) {
        /* ponytail: a folder's count takes in a video whose path is past CTX_MAX (390 to 400
         * bytes, or shorter with % in it, each three), though its row is left out */
        snprintf(ctx, sizeof ctx, "%s%s%s", rel, top ? "" : "/", l.v[i].name);
        if (!ctx_ok(ctx)) continue;
        item[n] = (item_t){ l.v[i].dir ? I_FOLDER : I_VIDEO, i, 0 };
        if (!l.v[i].dir) item[n].t = index_find(cache_key(ctx, l.v[i].size, l.v[i].mtime));
        ++n;
    }
    if (n == first) item[n++].type = I_NONE;
    if (top) item[n++].type = I_SETTINGS;
    for (int i = 0; i < n; ++i)
        missing |= (item[i].type == I_VIDEO || item[i].type == I_CONTINUE) && !item[i].t;

    /* Another level than the one shown last, or its rows from another COVERS on (More…, or Back
     * from there): the art folder is this one's now. Q2 Pod has let the other's rows go before
     * it asks; Settings, which has no covers, leaves the folder be. */
    if (arty && off % COVERS == 0 && (strcmp(rel, shown) || off != shown_off)) {
        art_clear();
        snprintf(shown, sizeof shown, "%s", rel), shown_off = off;
    }

    for (int i = off; i < n; ++i, ++sent) {
        const item_t *it = &item[i];
        const entry_t *e = l.n ? &l.v[it->i] : 0; /* a folder's or a video's alone */
        const thumb_t *t = it->t;
        unsigned key = 0;
        int ok, of = C_NONE;
        if (it->type == I_NONE)
            ok = row(r, "info", "", "No videos",
                     top ? "Copy videos into the Videos folder on the card" : "", "", "");
        else if (it->type == I_SETTINGS)
            ok = row(r, "list", "settings", "Settings", "", "", ""), of = C_SETTINGS,
            key = art_key("settings", "");
        else if (it->type == I_FOLDER) {
            snprintf(ctx, sizeof ctx, "%s%s%s", rel, top ? "" : "/", e->name);
            snprintf(text, sizeof text, "%d video%s", e->count, e->count == 1 ? "" : "s");
            ok = row(r, "list", "dir", e->name, text, ctx, ""), of = C_FOLDER,
            key = art_key("dir", ctx);
        } else { /* a video: held Play/Pause marks it watched, or not */
            int again = it->type == I_CONTINUE;
            if (again)
                snprintf(ctx, sizeof ctx, "%s", cw_rel);
            else
                snprintf(ctx, sizeof ctx, "%s%s%s", rel, top ? "" : "/", e->name);
            state_text(again ? &cw : marks_find(&marks, ctx, e->size), t, again ? cw.size : e->size,
                       text, sizeof text);
            lib_title(ctx, name, sizeof name);
            snprintf(sub, sizeof sub, "%s \xc2\xb7 %s", name, text);
            ok = row(r, "screen", "play", again ? "Continue Watching" : name, again ? sub : text,
                     ctx, "mark");
            of = !t                      ? C_NONE
                 : t->state == T_PICTURE ? C_THUMB
                                         : C_NOPICTURE,
            key = art_key("play", ctx);
        }
        if (!ok) break;
        if (arty && it->type != I_NONE) {
            int has = cover(key, of, t ? t->key : 0);
            covers += has;
            /* a thumbnail gone from the cache: the worker's tidying has it made again */
            missing |= of == C_THUMB && !has && access(thumb_file(t->key, path), 0);
        }
    }
    if (off + sent < n) /* the rows after these, by this cursor */
        snprintf(r->cursor, sizeof r->cursor, "%d", off + sent),
            snprintf(r->msg, sizeof r->msg, "pages");
    else if (off || n > LIVE_ROWS)
        snprintf(r->msg, sizeof r->msg, "pages");
    else
        live(r->msg, covers);
    if (missing) worker_ask(rel);
    free(item), lib_free(&l);
}

/* The Settings level: each row steps its setting when pressed (set), and the level, being live,
 * is asked again at once, so the row shows the new value in place. */
static void settings_page(rep_t *r) {
    static const char *const OFF_ON[] = { "Off", "On" },
                             *const SIZE[] = { "Small", "Medium", "Large" },
                             *const SORT[] = { "By Name", "Newest First" };
    settings_t s;
    settings_load(&s);
    row(r, "action", "set", "Play Next", OFF_ON[s.next], "next", "");
    row(r, "action", "set", "Subtitles", OFF_ON[s.subs], "subs", "");
    row(r, "action", "set", "Subtitle Size", SIZE[s.subsize], "subsize", "");
    row(r, "action", "set", "Fit to Screen", OFF_ON[s.fill], "fill", "");
    row(r, "action", "set", "Sort", SORT[s.sort], "sort", "");
    row(r, "info", "", "Version", VERSION, "", "");
    live(r->msg, 0);
}

/* ---- Actions: their replies have no rows, and their message is the toast ------------------- */

static void set(rep_t *r, const char *key) {
    settings_t s;
    settings_load(&s);
    if (!settings_step(&s, key))
        fail(r, "bad", "No such setting");
    else if (!settings_save(&s))
        fail(r, "error", "Can't write to the card");
}

/* Held Play/Pause on a video's row: watched, or not watched again, which forgets its place too.
 * A mark there already is changed where it is, so Continue Watching keeps its order. */
static void mark(rep_t *r, const char *rel) {
    char root[PATH_N], path[PATH_N];
    struct stat st;
    if (!is_video(rel) || !lib_root(root, sizeof root) || !lib_path(root, rel, path, sizeof path) ||
        stat(path, &st) || !S_ISREG(st.st_mode)) {
        fail(r, "unavailable", "Video not found");
        return;
    }
    marks_load(&marks);
    mark_t *k = marks_find(&marks, rel, st.st_size);
    int watched = !(k && k->flags & MARK_WATCHED);
    if (!watched)
        k->flags &= ~MARK_WATCHED, k->pos_ms = 0;
    else if (k || (k = marks_put(&marks, rel, st.st_size)))
        k->flags |= MARK_WATCHED;
    if (!k || !marks_save(&marks))
        fail(r, "error", "Can't write to the card");
    else
        snprintf(r->msg, sizeof r->msg, watched ? "Marked as watched" : "Marked as unwatched");
}

/* ---- Requests ------------------------------------------------------------------------------ */

/* A cursor's row number: "" is 0; -1 when it is none of ours. */
static int offset(const char *cursor) {
    size_t n = strlen(cursor);
    return n > 9 || strspn(cursor, "0123456789") != n ? -1 : atoi(cursor);
}

static void handle(rep_t *r, const char *op, int args, const char *a, const char *b) {
    int off, known = 1;
    if (!strcmp(op, "menu") && args < 2 && (off = offset(a)) >= 0)
        level(r, "", off);
    else if (!strcmp(op, "dir") && args && rel_ok(a) && (off = offset(b)) >= 0)
        level(r, a, off);
    else if (!strcmp(op, "settings") && !args)
        settings_page(r);
    else if (!strcmp(op, "set") && args == 1)
        set(r, a);
    else if (!strcmp(op, "mark") && args == 1 && rel_ok(a))
        mark(r, a);
    else {
        static const char *const OPS[] = { "menu", "dir", "settings", "set", "mark" };
        known = 0;
        for (size_t i = 0; i < sizeof OPS / sizeof *OPS; ++i) known |= !strcmp(op, OPS[i]);
        fail(r, known ? "bad" : "unsupported", known ? "Bad request" : "Not supported");
    }
}

size_t serve_reply(const char *req, size_t n, char *out) {
    static char text[REQ_MAX + 1];
    static rep_t r;
    char *f[5] = { 0 }, *id = "0";
    int k = 0;
    r.len = 0, r.count = 0, r.status = "ok", r.msg[0] = r.cursor[0] = 0;
    if (n > 9 && n <= REQ_MAX && !memcmp(req, "Q2SPOT/1\n", 9) && !memchr(req, 0, n)) {
        memcpy(text, req + 9, n - 9);
        text[n - 9] = 0;
        text[strcspn(text, "\n")] = 0; /* the request is its one line */
        for (char *s = text; s && k < 5;) f[k++] = strsep(&s, "\t");
        for (int i = 0; i < k; ++i)
            if (!q2_unescape(f[i])) k = 0;
    }
    /* the id: Q2 Pod's request number, sent back so that it knows the reply for its own. An op
     * has two arguments at most. */
    if (k < 2 || k > 4 || !*f[0] || strlen(f[0]) > 10 || f[0][strspn(f[0], "0123456789")])
        fail(&r, "bad", "Bad request");
    else
        id = f[0], handle(&r, f[1], k - 2, k > 2 ? f[2] : "", k > 3 ? f[3] : "");
    size_t head = (size_t)snprintf(out, HEAD_MAX, "Q2SPOT/1\n%s\t%s\t%s\t%s\n", id, r.status, r.msg,
                                   r.cursor);
    memcpy(out + head, r.rows, r.len);
    return head + r.len;
}

/* ---- The socket ---------------------------------------------------------------------------- */

/* One connection: Q2 Pod writes its request and shuts its side; the reply, and the close. */
static void answer(int fd) {
    static char req[REQ_MAX + 1], out[REPLY_MAX];
    struct pollfd p = { fd, POLLIN, 0 };
    struct timeval tv = { REQ_MS / 1000, 0 };
    long long end = now_ms() + REQ_MS, left;
    size_t n = 0, len, sent = 0;
    ssize_t k = 1;
    /* one byte more than a request may have, to know one that is too long */
    while (!quit && n < sizeof req && (left = end - now_ms()) > 0 && poll(&p, 1, (int)left) > 0 &&
           (k = read(fd, req + n, sizeof req - n)) > 0)
        n += (size_t)k;
    /* a request that never ended is no request */
    len = serve_reply(req, k ? 0 : n, out);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    while (!quit && sent < len && (k = send(fd, out + sent, len - sent, MSG_NOSIGNAL)) > 0)
        sent += (size_t)k;
    close(fd);
}

int serve_main(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct sigaction sa = { .sa_handler = on_term }; /* no SA_RESTART: a TERM ends a wait */
    char path[PATH_N];
    long long idle = IDLE_MS, stuck = STUCK_MS;
#ifdef HOST /* the checks wait a second, not ten minutes */
    if (getenv("VIDEOS_IDLE_MS")) idle = atoll(getenv("VIDEOS_IDLE_MS"));
    if (getenv("VIDEOS_STUCK_MS")) stuck = atoll(getenv("VIDEOS_STUCK_MS"));
#endif
    sigaction(SIGTERM, &sa, 0), sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s/q2-videos.browse.sock", tmp_dir) >=
        (int)sizeof addr.sun_path)
        return 1;
    unlink(addr.sun_path);
    /* the socket first: Q2 Pod waits for it, and its first request for the rest */
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof addr) ||
        listen(listener, 4))
        return 1;
    art_clear();
    index_load();

    for (long long last = now_ms(), now; !quit;) {
        struct pollfd p[2] = { { listener, POLLIN, 0 },
                               { wk.from, POLLIN, 0 } }; /* -1: not looked at */
        int fd;
        if (poll(p, 2, 1000) > 0) {
            if (p[1].revents) worker_read();
            if (p[0].revents && (fd = accept(listener, 0, 0)) >= 0) answer(fd), last = now_ms();
        }
        now = now_ms();
        /* a worker that is stuck: ended here, and seen to by worker_read as any that died */
        if (wk.pid > 0 && wk.trying && now - wk.since > stuck) kill(wk.pid, SIGKILL);
        /* A video playing: Q2 Pod asks nothing meanwhile, which is not idleness, and the ten
         * minutes start again when the player ends. No worker is started under it either. */
        if ((fd = lock_take(0)) < 0)
            last = now;
        else
            close(fd), worker_start();
        if (now - last >= idle) break;
    }
    worker_stop();
    close(listener);
    unlink(addr.sun_path);
    art_clear(); /* the folder is the app's to clean */
    rmdir(art_file("", path));
    return 0;
}
