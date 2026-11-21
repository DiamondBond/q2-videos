/* The Videos folder: finding it on the card, listing a level of it, the order of a list, and the
 * texts and keys a row is made of. Nothing here opens a video; the server probes them. */
#include "videos.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- Names --------------------------------------------------------------------------------- */

int is_video(const char *name) {
    static const char *const EXT[] = { "mp4",  "m4v", "mkv", "webm", "avi",  "mov", "mpg",
                                       "mpeg", "wmv", "flv", "ts",   "m2ts", "3gp", "ogv" };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    for (size_t i = 0; i < sizeof EXT / sizeof *EXT; ++i)
        if (!strcasecmp(dot + 1, EXT[i])) return 1;
    return 0;
}

static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ponytail: case is folded for ASCII alone, so "Écoute" and "écoute" sort apart (both after
 * every ASCII name); fold more if a library is ever listed wrong for it. */
int natcmp(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (*x && *y) {
        if (digit(*x) && digit(*y)) { /* two numbers: the longer, less its zeros, is the larger */
            size_t i = 0, j = 0;
            while (*x == '0') ++x;
            while (*y == '0') ++y;
            while (digit(x[i])) ++i;
            while (digit(y[j])) ++j;
            int c = i != j ? (i < j ? -1 : 1) : memcmp(x, y, i);
            if (c) return c;
            x += i, y += j;
        } else {
            int c = lower(*x) - lower(*y);
            if (c) return c;
            ++x, ++y;
        }
    }
    if (*x || *y) return *x ? 1 : -1;
    return strcmp(a, b); /* "a" and "A", "1" and "01": still one order */
}

int rel_ok(const char *rel) {
    if (!rel || strlen(rel) > REL_MAX) return 0;
    for (const char *p = rel;; ++p) { /* a part at a time */
        if (*p == '.' || *p == '/' || !*p) return 0;
        for (; *p && *p != '/'; ++p)
            if ((unsigned char)*p < 0x20) return 0;
        if (!*p) return 1;
    }
}

const char *lib_title(const char *path, char *out, size_t n) {
    const char *name = strrchr(path, '/'), *dot;
    name = name ? name + 1 : path;
    dot = strrchr(name, '.');
    snprintf(out, n, "%.*s", dot && dot > name ? (int)(dot - name) : (int)strlen(name), name);
    return out;
}

/* ---- The folder ---------------------------------------------------------------------------- */

int lib_root(char *root, size_t n) {
    char best[256] = "", path[PATH_N];
    DIR *d = opendir(card_dir);
    struct dirent *e;
    struct stat st;
    /* exFAT has one spelling at most; where several can be (a computer), the first in order */
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, "Videos") && (!*best || strcmp(e->d_name, best) < 0) &&
            snprintf(path, sizeof path, "%s/%s", card_dir, e->d_name) < (int)sizeof path &&
            !stat(path, &st) && S_ISDIR(st.st_mode))
            strcpy(best, e->d_name);
    if (d) closedir(d);
    return *best && snprintf(root, n, "%s/%s", card_dir, best) < (int)n;
}

int lib_path(const char *root, const char *rel, char *out, size_t n) {
    if (rel && !*rel) return snprintf(out, n, "%s", root) < (int)n;
    return rel_ok(rel) && snprintf(out, n, "%s/%s", root, rel) < (int)n;
}

static int add(listing_t *l, const char *name, int dir, int count, const struct stat *st) {
    if (l->n == l->room) {
        int room = l->room ? l->room * 2 : 32;
        entry_t *v = realloc(l->v, (size_t)room * sizeof *v);
        if (!v) return 0;
        l->v = v, l->room = room;
    }
    char *copy = strdup(name);
    if (!copy) return 0;
    l->v[l->n++] = (entry_t){ copy, dir, count, st ? st->st_size : 0, st ? st->st_mtime : 0 };
    l->dirs += dir;
    return 1;
}

void lib_free(listing_t *l) {
    for (int i = 0; i < l->n; ++i) free(l->v[i].name);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* What e, an entry of the folder p[0..len), is: 'd' a folder, 'v' a video, 0 anything else, a
 * dot entry or a path too long for a row (p + from is where the row's path starts). Its path is
 * left in p, and with want a video's stat in st. A link to a folder is not followed: no card
 * has one, and on a computer a loop of them would never end. */
static int kind(char *p, size_t len, size_t from, const struct dirent *e, struct stat *st,
                int want) {
    size_t k = strlen(e->d_name);
    int type = e->d_type, have = 0;
    if (!rel_ok(e->d_name) || len + 1 + k >= PATH_N || len + 1 + k - from > REL_MAX) return 0;
    p[len] = '/';
    memcpy(p + len + 1, e->d_name, k + 1);
    if (type == DT_UNKNOWN) { /* a file system that doesn't say */
        if (lstat(p, st)) return 0;
        type = S_ISDIR(st->st_mode) ? DT_DIR : S_ISREG(st->st_mode) ? DT_REG : DT_LNK;
        have = type == DT_REG;
    }
    if (type == DT_DIR) return 'd';
    if ((type != DT_REG && type != DT_LNK) || !is_video(e->d_name)) return 0;
    if ((type == DT_LNK || (want && !have)) && (stat(p, st) || !S_ISREG(st->st_mode))) return 0;
    return 'v';
}

/* The videos under the folder p[0..len), through its subfolders; each is added to all, by its
 * path from p + from, when all is given. */
static int walk(char *p, size_t len, size_t from, listing_t *all) {
    struct dirent *e;
    struct stat st;
    int count = 0;
    p[len] = 0;
    DIR *d = opendir(p);
    while (d && (e = readdir(d))) {
        int k = kind(p, len, from, e, &st, all != 0);
        if (k == 'd')
            count += walk(p, len + 1 + strlen(e->d_name), from, all);
        else if (k == 'v' && (!all || add(all, p + from, 0, 0, &st)))
            ++count;
    }
    if (d) closedir(d);
    return count;
}

static int by_name(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    return x->dir != y->dir ? y->dir - x->dir : natcmp(x->name, y->name);
}
static int by_newest(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->dir || y->dir || x->mtime == y->mtime) return by_name(a, b);
    return x->mtime > y->mtime ? -1 : 1;
}

/* ponytail: a subfolder's count walks everything under it, each time its level is listed (a
 * level on show is asked for every 2 s). Fine for the hundreds of videos a card holds; keep the
 * counts between requests if a library of thousands makes the list slow. */
int lib_list(const char *root, const char *rel, int sort, listing_t *l) {
    char p[PATH_N];
    struct dirent *e;
    struct stat st;
    memset(l, 0, sizeof *l);
    if (!lib_path(root, rel, p, sizeof p)) return 0;
    size_t len = strlen(p), from = strlen(root) + 1;
    DIR *d = opendir(p);
    if (!d) return 0;
    while ((e = readdir(d))) {
        int k = kind(p, len, from, e, &st, 1), count;
        if (k == 'd' && (count = walk(p, len + 1 + strlen(e->d_name), from, 0)))
            add(l, e->d_name, 1, count, 0);
        else if (k == 'v')
            add(l, e->d_name, 0, 0, &st);
    }
    closedir(d);
    if (l->n) qsort(l->v, (size_t)l->n, sizeof *l->v, sort == SORT_NEWEST ? by_newest : by_name);
    return 1;
}

int lib_all(const char *root, listing_t *l) {
    char p[PATH_N];
    memset(l, 0, sizeof *l);
    DIR *d = opendir(root);
    if (!d) return 0;
    closedir(d);
    if (!lib_path(root, "", p, sizeof p)) return 0;
    walk(p, strlen(p), strlen(p) + 1, l);
    if (l->n) qsort(l->v, (size_t)l->n, sizeof *l->v, by_name);
    return 1;
}

int lib_next(const char *root, const char *rel, int sort, char *out, size_t n) {
    char dir[REL_MAX + 1];
    listing_t l;
    int found = 0;
    if (!rel_ok(rel)) return 0;
    const char *name = strrchr(rel, '/');
    snprintf(dir, sizeof dir, "%.*s", name ? (int)(name - rel) : 0, rel);
    name = name ? name + 1 : rel;
    if (lib_list(root, dir, sort, &l))
        for (int i = l.dirs; i + 1 < l.n && !found; ++i)
            if (!strcmp(l.v[i].name, name))
                found = snprintf(out, n, "%s%s%s", dir, *dir ? "/" : "", l.v[i + 1].name) < (int)n;
    lib_free(&l);
    return found;
}

/* ponytail: a mark whose video left the card is asked for again each time, most recent first,
 * until one is there; marks_find drops such a mark once its row is listed with another size, and
 * the cap ends the rest. Drop them here if a card of stale marks ever makes the menu slow. */
const mark_t *lib_continue(const char *root, const marks_t *m) {
    char p[PATH_N];
    struct stat st;
    for (long long below = 1ll << 32;;) {
        const mark_t *k = 0;
        for (int i = 0; i < m->n; ++i)
            if (mark_unfinished(&m->v[i]) && m->v[i].seq < below && (!k || m->v[i].seq > k->seq))
                k = &m->v[i];
        if (!k) return 0;
        if (is_video(k->rel) && lib_path(root, k->rel, p, sizeof p) && !stat(p, &st) &&
            S_ISREG(st.st_mode) && st.st_size == k->size)
            return k;
        below = k->seq;
    }
}

/* ---- Texts and keys ------------------------------------------------------------------------ */

const char *text_length(long long ms, char *out, size_t n) {
    long long s = (ms + 500) / 1000, min = (s + 30) / 60;
    if (ms <= 0)
        snprintf(out, n, "%s", "");
    else if (s < 60)
        snprintf(out, n, "%lld sec", s ? s : 1);
    else if (min < 60)
        snprintf(out, n, "%lld min", min);
    else if (min % 60)
        snprintf(out, n, "%lld h %lld min", min / 60, min % 60);
    else
        snprintf(out, n, "%lld h", min / 60);
    return out;
}

const char *text_left(long long pos_ms, long long dur_ms, char *out, size_t n) {
    char length[32];
    if (dur_ms - pos_ms < 60000)
        snprintf(out, n, "Less than a minute left");
    else
        snprintf(out, n, "%s left", text_length(dur_ms - pos_ms, length, sizeof length));
    return out;
}

const char *text_size(long long bytes, char *out, size_t n) {
    const long long KB = 1024, MB = KB * KB, GB = MB * KB;
    if (bytes < 0) bytes = 0;
    if (bytes >= 1000 * MB) { /* tenths, rounded */
        long long t = (bytes * 10 + GB / 2) / GB;
        snprintf(out, n, "%lld.%lld GB", t / 10, t % 10);
    } else if (bytes >= 1000 * KB)
        snprintf(out, n, "%lld MB", (bytes + MB / 2) / MB);
    else
        snprintf(out, n, "%lld KB", (bytes + KB / 2) / KB);
    return out;
}

unsigned fnv32(unsigned h, const void *data, size_t n) {
    for (const unsigned char *p = data; n--; ++p) h = (h ^ *p) * 16777619u;
    return h;
}

unsigned long long fnv64(unsigned long long h, const void *data, size_t n) {
    for (const unsigned char *p = data; n--; ++p) h = (h ^ *p) * 1099511628211ull;
    return h;
}

unsigned art_key(const char *uri, const char *context) {
    unsigned h = fnv32(FNV32, uri, strlen(uri));
    return fnv32(fnv32(h, "\t", 1), context, strlen(context));
}

/* ponytail: FAT keeps local time, so a change of time zone moves every mtime and with it every
 * key: the thumbnails are made again, once. */
unsigned long long cache_key(const char *rel, long long size, long long mtime) {
    char tail[48];
    int n = snprintf(tail, sizeof tail, "\t%lld\t%lld", size, mtime);
    return fnv64(fnv64(FNV64, rel, strlen(rel)), tail, (size_t)n);
}
