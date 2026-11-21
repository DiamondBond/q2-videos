/* The app's state on the card, in state_dir: "settings" and "marks". Both are small text files
 * written whole (file_put): the Q2 corrupts a card that is pulled mid-write. */
#include "videos.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* path = state_dir/name; the folder is made for a write (a card that never ran the app) */
static const char *state_file(char *path, const char *name, int writing) {
    if (writing) mkdir(state_dir, 0755);
    snprintf(path, PATH_N, "%s/%s", state_dir, name);
    return path;
}

/* ---- Settings ------------------------------------------------------------------------------ */

static const struct {
    const char *key;
    size_t at;
    int values, initial;
} SETTINGS[] = {
    { "next", offsetof(settings_t, next), 2, 0 },
    { "subs", offsetof(settings_t, subs), 2, 1 },
    { "subsize", offsetof(settings_t, subsize), 3, 1 },
    { "fill", offsetof(settings_t, fill), 2, 0 },
    { "sort", offsetof(settings_t, sort), 2, SORT_NAME },
};
#define NSETTINGS (sizeof SETTINGS / sizeof *SETTINGS)
#define FIELD(s, i) ((int *)((char *)(s) + SETTINGS[i].at))

void settings_load(settings_t *s) {
    char path[PATH_N], *text = file_get(state_file(path, "settings", 0), 0);
    for (size_t i = 0; i < NSETTINGS; ++i) *FIELD(s, i) = SETTINGS[i].initial;
    for (char *save = 0, *line = text ? strtok_r(text, "\r\n", &save) : 0; line;
         line = strtok_r(0, "\r\n", &save)) {
        char *eq = strchr(line, '='), *end;
        if (!eq) continue;
        *eq = 0;
        long v = strtol(eq + 1, &end, 10);
        for (size_t i = 0; i < NSETTINGS; ++i)
            if (!strcmp(line, SETTINGS[i].key) && end != eq + 1 && !*end && v >= 0 &&
                v < SETTINGS[i].values)
                *FIELD(s, i) = (int)v;
    }
    free(text);
}

int settings_save(const settings_t *s) {
    char path[PATH_N], text[200];
    int n = 0;
    for (size_t i = 0; i < NSETTINGS; ++i)
        n += snprintf(text + n, sizeof text - (size_t)n, "%s=%d\n", SETTINGS[i].key, *FIELD(s, i));
    return file_put(state_file(path, "settings", 1), text, (size_t)n);
}

int settings_step(settings_t *s, const char *key) {
    for (size_t i = 0; i < NSETTINGS; ++i)
        if (!strcmp(key, SETTINGS[i].key))
            return *FIELD(s, i) = (*FIELD(s, i) + 1) % SETTINGS[i].values, 1;
    return 0;
}

/* ---- Marks --------------------------------------------------------------------------------- */

#define MS_MAX (10000ll * 3600000) /* a place or a length past 10,000 hours is neither */
#define SEQ_MAX (1 << 30)          /* a billion plays on: no file of ours says more */

void marks_free(marks_t *m) {
    for (int i = 0; i < m->n; ++i) free(m->v[i].rel);
    m->n = 0;
    m->seq = 0;
}

void marks_drop(marks_t *m, mark_t *k) {
    free(k->rel);
    *k = m->v[--m->n]; /* the last takes its place: seq orders them, not the slots */
}

static mark_t *oldest(marks_t *m) {
    mark_t *k = m->v;
    for (int i = 1; i < m->n; ++i)
        if (m->v[i].seq < k->seq) k = &m->v[i];
    return k;
}

static mark_t *by_path(marks_t *m, const char *rel) {
    for (int i = 0; i < m->n; ++i)
        if (!strcmp(m->v[i].rel, rel)) return &m->v[i];
    return 0;
}

/* A line of the file into k, its path left pointing into the line. 0 for a line that isn't one
 * of ours whole: a field missing, not a number or out of its range, a path rel_ok refuses. */
static int mark_parse(char *line, mark_t *k) {
    long long f[7];
    for (int i = 0; i < 7; ++i) {
        char *end;
        if (*line != '-' && (*line < '0' || *line > '9')) return 0; /* strtoll would skip blanks */
        errno = 0;
        f[i] = strtoll(line, &end, 10);
        if (errno || *end != '\t') return 0;
        line = end + 1;
    }
    /* nor a place past its length (when that is known) or either past any video's, which a
     * seek would multiply out of 64 bits; nor a seq so high that the next would wrap round and
     * make the video just played the oldest */
    if (f[0] < 0 || f[1] < 0 || f[0] > MS_MAX || f[1] > MS_MAX || (f[1] && f[0] > f[1]) ||
        f[2] < 0 || f[2] > UINT_MAX || f[3] < 0 || f[3] > SEQ_MAX || f[4] < -1 || f[4] > INT_MAX ||
        f[5] < -1 || f[5] > INT_MAX || f[6] < 0 || !rel_ok(line))
        return 0;
    *k = (mark_t){ f[0], f[1], f[6], (unsigned)f[2], (unsigned)f[3], (int)f[4], (int)f[5], line };
    return 1;
}

void marks_load(marks_t *m) {
    char path[PATH_N], *text = file_get(state_file(path, "marks", 0), 0), *end;
    marks_free(m);
    /* only a line that ends is taken: the rest of a torn one could pass for a shorter path */
    for (char *line = text; line && (end = strchr(line, '\n')); line = end + 1) {
        mark_t k, *old;
        *end = 0;
        if (!mark_parse(line, &k)) continue;
        if (k.seq > m->seq) m->seq = k.seq;
        if ((old = by_path(m, k.rel))) {
            if (k.seq < old->seq) continue;
            k.rel = old->rel;
            *old = k;
            continue;
        }
        if (m->n == MARKS_MAX) { /* a file past the cap: the most recent MARKS_MAX stay */
            if (k.seq < (old = oldest(m))->seq) continue;
            marks_drop(m, old);
        }
        if ((k.rel = strdup(k.rel))) m->v[m->n++] = k;
    }
    free(text);
}

int marks_save(const marks_t *m) {
    char path[PATH_N], *text = 0;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    if (!f) return 0;
    for (int i = 0; i < m->n; ++i) {
        const mark_t *k = &m->v[i];
        fprintf(f, "%lld\t%lld\t%u\t%u\t%d\t%d\t%lld\t%s\n", k->pos_ms, k->dur_ms, k->flags, k->seq,
                k->audio, k->sub, k->size, k->rel);
    }
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    ok = ok && file_put(state_file(path, "marks", 1), text, len);
    free(text);
    return ok;
}

mark_t *marks_find(marks_t *m, const char *rel, long long size) {
    mark_t *k = rel ? by_path(m, rel) : 0;
    if (k && k->size != size) marks_drop(m, k), k = 0;
    return k;
}

mark_t *marks_put(marks_t *m, const char *rel, long long size) {
    mark_t *k = marks_find(m, rel, size);
    if (!k) {
        char *copy = rel_ok(rel) ? strdup(rel) : 0;
        if (!copy) return 0;
        if (m->n == MARKS_MAX) marks_drop(m, oldest(m));
        k = &m->v[m->n++];
        *k = (mark_t){ .size = size, .audio = -1, .sub = -1, .rel = copy };
    }
    k->seq = ++m->seq;
    return k;
}
