/* Subtitle text: a SubRip file beside the video read whole, what libavcodec's subtitle decoders
 * hand over made plain, the cues on show at a time, and the names tracks go by. Nothing here
 * draws; osd.c wraps and draws the lines. */
#include "player.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Cues ---------------------------------------------------------------------------------- */

static int cues_add(cues_t *c, long long start, long long end, const char *text) {
    if (c->n == c->room) {
        int room = c->room ? c->room * 2 : 64;
        cue_t *v = realloc(c->v, (size_t)room * sizeof *v);
        if (!v) return 0;
        c->v = v, c->room = room;
    }
    char *copy = strdup(text);
    if (!copy) return 0;
    c->v[c->n++] = (cue_t){ start, end, copy };
    return 1;
}

void cues_free(cues_t *c) {
    for (int i = 0; i < c->n; ++i) free(c->v[i].text);
    free(c->v);
    memset(c, 0, sizeof *c);
}

/* ponytail: every cue is looked at each time, a film's two thousand in a few microseconds; keep
 * the place between calls if a file of far more ever shows in the frame time. */
int cues_at(const cues_t *c, long long ms, const cue_t **out, int max, long long *next) {
    int n = 0;
    long long change = -1;
    for (int i = 0; i < c->n; ++i) {
        const cue_t *k = &c->v[i];
        long long edge = ms < k->start ? k->start : ms < k->end ? k->end : -1;
        if (edge >= 0 && (change < 0 || edge < change)) change = edge;
        if (ms >= k->start && ms < k->end && n < max) out[n++] = k;
    }
    if (next) *next = change;
    return n;
}

/* ---- Plain text ---------------------------------------------------------------------------- */

/* text's lines trimmed in place, the empty ones left out */
static void trim_lines(char *text) {
    char *o = text;
    for (char *line = text, *end; *line; line = end) {
        end = line + strcspn(line, "\n");
        char *a = line, *b = end;
        if (*end) ++end;
        while (a < b && (*a == ' ' || *a == '\t' || *a == '\r')) ++a;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) --b;
        if (a == b) continue;
        if (o > text) *o++ = '\n';
        memmove(o, a, (size_t)(b - a));
        o += b - a;
    }
    *o = 0;
}

void tags_strip(char *text) {
    char *o = text;
    for (const char *p = text; *p;) {
        const char *close = 0;
        if (p[0] == '{' && p[1] == '\\')
            close = strchr(p, '}');
        else if (p[0] == '<' && (p[1] == '/' || ((p[1] | 32) >= 'a' && (p[1] | 32) <= 'z'))) {
            close = strchr(p, '>'); /* <i>, </font>: a letter after it, and no line end inside */
            if (close && memchr(p, '\n', (size_t)(close - p))) close = 0;
        }
        if (close)
            p = close + 1;
        else
            *o++ = *p++;
    }
    *o = 0;
    trim_lines(text);
}

void ass_plain(const char *event, char *out, size_t n) {
    /* past the fields before the text: eight commas, nine when the line has its times too */
    int commas = strncmp(event, "Dialogue:", 9) ? 8 : 9, drawing = 0;
    const char *p = event;
    for (; commas && *p; ++p) commas -= *p == ',';
    if (commas) p = event; /* not an event: text as it is */
    char *o = out, *end = out + (n ? n - 1 : 0);
    while (*p && o < end) {
        if (*p == '{') { /* an override block; \p1 starts a drawing's numbers, \p0 ends them */
            const char *close = strchr(p, '}'), *d = p;
            if (!close) { /* never closed: text */
                *o++ = *p++;
                continue;
            }
            while ((d = strstr(d, "\\p")) && d < close)
                if ((d += 2, *d >= '0' && *d <= '9'))
                    drawing = *d != '0' || (d[1] >= '0' && d[1] <= '9');
            p = close + 1;
        } else if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n' || p[1] == 'h')) {
            if (!drawing) *o++ = p[1] == 'h' ? ' ' : '\n';
            p += 2;
        } else if (drawing)
            ++p;
        else
            *o++ = *p++;
    }
    if (*p && o == end) { /* cut short: never a letter in half */
        while (o > out && (o[-1] & 0xc0) == 0x80) --o;
        if (o > out && (unsigned char)o[-1] >= 0xc0) --o;
    }
    if (n) *o = 0;
    trim_lines(out);
}

/* ---- SubRip -------------------------------------------------------------------------------- */

/* "01:02:03,456" (the hours may be missing, the comma a point) at *s: its ms, *s moved past. */
static int stamp(const char **s, long long *ms) {
    const char *p = *s;
    long long part[3] = { 0, 0, 0 }, frac = 0;
    int n = 0, digits = 0;
    while (*p == ' ' || *p == '\t') ++p;
    for (;; ++p) {
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') part[n] = part[n] * 10 + (*p++ - '0');
        if (*p != ':' || ++n == 3) break;
    }
    if (n == 3 || n == 0) return 0;
    if (*p == ',' || *p == '.')
        for (++p; *p >= '0' && *p <= '9'; ++p)
            if (digits < 3) frac = frac * 10 + (*p - '0'), ++digits;
    for (; digits && digits < 3; ++digits) frac *= 10;
    *ms = n == 2 ? ((part[0] * 60 + part[1]) * 60 + part[2]) * 1000 + frac
                 : (part[0] * 60 + part[1]) * 1000 + frac;
    *s = p;
    return 1;
}

static int by_start(const void *a, const void *b) {
    const cue_t *x = a, *y = b;
    return x->start != y->start ? (x->start < y->start ? -1 : 1)
           : x->end < y->end    ? -1
                                : x->end > y->end;
}

int srt_parse(const char *data, size_t n, cues_t *c) {
    if (n >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) data += 3, n -= 3;
    char *text = strndup(data, n); /* to its first NUL: a text file has none */
    if (!text) return 0;

    int before = c->n, open = 0;
    long long start = 0, end = 0;
    char *body = 0; /* the open cue's text: from here to the line that ends it */
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        const char *t = line, *arrow = 0;
        long long a = 0, b = 0;
        for (size_t i = 0; i + 2 < len && !arrow; ++i)
            if (!memcmp(line + i, "-->", 3)) arrow = line + i;
        int timing = arrow && stamp(&t, &a) && (t += strspn(t, " \t"), t == arrow) &&
                     (t += 3, stamp(&t, &b));
        int blank = strspn(line, " \t\r") == len;
        if (open && (timing || blank || !next)) { /* the cue's last line: the one before this */
            char *stop_at = blank || timing ? line : line + len;
            char keep = *stop_at;
            *stop_at = 0;
            tags_strip(body);
            if (timing) { /* cues with no empty line between: the number above the times goes */
                char *last = strrchr(body, '\n');
                if (last && strspn(last + 1, "0123456789") == strlen(last + 1)) *last = 0;
            }
            if (*body && end > start) cues_add(c, start, end, body);
            *stop_at = keep;
            open = 0;
        }
        if (timing) open = 1, start = a, end = b, body = next ? next + 1 : line + len;
        if (next) ++next;
    }
    free(text);
    if (c->n > before) qsort(c->v + before, (size_t)(c->n - before), sizeof *c->v, by_start);
    return c->n - before;
}

/* ---- Names --------------------------------------------------------------------------------- */

const char *lang_name(const char *code) {
    static const char *const LANG[][4] = {
        { "en", "eng", "", "English" },      { "ja", "jpn", "", "Japanese" },
        { "es", "spa", "", "Spanish" },      { "fr", "fre", "fra", "French" },
        { "de", "ger", "deu", "German" },    { "it", "ita", "", "Italian" },
        { "pt", "por", "", "Portuguese" },   { "ru", "rus", "", "Russian" },
        { "zh", "chi", "zho", "Chinese" },   { "ko", "kor", "", "Korean" },
        { "ar", "ara", "", "Arabic" },       { "hi", "hin", "", "Hindi" },
        { "nl", "dut", "nld", "Dutch" },     { "sv", "swe", "", "Swedish" },
        { "no", "nor", "nob", "Norwegian" }, { "da", "dan", "", "Danish" },
        { "fi", "fin", "", "Finnish" },      { "pl", "pol", "", "Polish" },
        { "tr", "tur", "", "Turkish" },      { "el", "gre", "ell", "Greek" },
        { "he", "heb", "", "Hebrew" },       { "th", "tha", "", "Thai" },
        { "vi", "vie", "", "Vietnamese" },   { "id", "ind", "", "Indonesian" },
        { "uk", "ukr", "", "Ukrainian" },    { "cs", "cze", "ces", "Czech" },
        { "hu", "hun", "", "Hungarian" },    { "ro", "rum", "ron", "Romanian" },
    };
    for (size_t i = 0; i < sizeof LANG / sizeof *LANG; ++i)
        for (int k = 0; k < 3; ++k)
            if (!strcasecmp(code, LANG[i][k]) && *code) return LANG[i][3];
    return code;
}

/* the file with the video's own name first: it is the one shown by default */
static int by_path(const void *a, const void *b) {
    const sidecar_t *x = a, *y = b;
    int own = !strcmp(y->name, "External") - !strcmp(x->name, "External");
    return own ? own : natcmp(x->path, y->path);
}

int sidecars(const char *path, sidecar_t *out, int max) {
    char dir[PATH_N];
    const char *name = strrchr(path, '/'), *dot;
    snprintf(dir, sizeof dir, "%.*s", name ? (int)(name - path) : 1, name ? path : ".");
    name = name ? name + 1 : path;
    dot = strrchr(name, '.');
    size_t stem = dot && dot > name ? (size_t)(dot - name) : strlen(name);
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && n < max && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        /* "<stem>.srt" or "<stem>.<word>.srt" */
        if (len < stem + 4 || strncasecmp(e->d_name, name, stem) ||
            strcasecmp(e->d_name + len - 4, ".srt"))
            continue;
        const char *word = e->d_name + stem;
        size_t wlen = len - 4 - stem; /* "" or ".en" */
        if (wlen && (word[0] != '.' || wlen < 2 || wlen > 32 || memchr(word + 1, '.', wlen - 1)))
            continue;
        if (snprintf(out[n].path, PATH_N, "%s/%s", dir, e->d_name) >= PATH_N) continue;
        char lang[40];
        snprintf(lang, sizeof lang, "%.*s", wlen ? (int)wlen - 1 : 0, word + 1);
        snprintf(out[n].name, sizeof out[n].name, "%s", wlen ? lang_name(lang) : "External");
        ++n;
    }
    if (d) closedir(d);
    if (n) qsort(out, (size_t)n, sizeof *out, by_path);
    return n;
}
