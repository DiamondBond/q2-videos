/* What the player draws with: shapes and text on a page. A page is the panel's own layout (320 x
 * 375, as the framebuffer has it) and everything here takes the view's coordinates (375 x 320,
 * the panel turned), so drawing is the same whichever way the panel lies and a page goes to the
 * screen row by row. Flat colours only; edges are anti-aliased by coverage. */
#include "player.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define GLASS 80 /* the radius of the glass's corners (q2-pod docs/ipod.md, Rounded corners) */

int inset(int y) {
    int d = y < VIEW_H - 1 - y ? y : VIEW_H - 1 - y;
    if (d < 0) d = 0;
    if (d >= GLASS) return 0;
    return (int)ceilf(GLASS - sqrtf((float)(GLASS * GLASS - (GLASS - d) * (GLASS - d))));
}

int clear_x(int y0, int y1) {
    int a = inset(y0), b = inset(y1); /* the row nearest an edge hides the most */
    return (a > b ? a : b) + 4;
}

/* ---- Shapes -------------------------------------------------------------------------------- */

/* c over *d at a of 255. Red and blue are mixed in one multiply, as they don't meet. */
static inline void blend(pix *d, pix c, unsigned a) {
    a += a >> 7; /* 0 to 256 */
    unsigned rb = *d & 0xff00ff, g = *d & 0x00ff00;
    rb = (rb + (((c & 0xff00ff) - rb) * a >> 8)) & 0xff00ff;
    g = (g + (((c & 0x00ff00) - g) * a >> 8)) & 0x00ff00;
    *d = 0xff000000u | rb | g;
}

static inline void dot(pix *page, int x, int y, pix rgb, int alpha) {
    if (x >= 0 && x < VIEW_W && y >= 0 && y < VIEW_H && alpha > 0)
        blend(view_at(page, x, y), rgb, (unsigned)(alpha > 255 ? 255 : alpha));
}

void fill(pix *page, int x, int y, int w, int h, pix rgb, int alpha) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > VIEW_W) w = VIEW_W - x;
    if (y + h > VIEW_H) h = VIEW_H - y;
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    /* a column of the view is a run of the page's row: the memory is walked in order */
    for (int i = 0; i < w; ++i) {
        pix *p = view_at(page, x + i, y + h - 1);
        if (alpha >= 255)
            for (int k = 0; k < h; ++k) p[k] = 0xff000000u | rgb;
        else
            for (int k = 0; k < h; ++k) blend(p + k, rgb, (unsigned)alpha);
    }
}

static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void rbox(pix *page, int x, int y, int w, int h, float r, pix rgb, int alpha, const pix *rows) {
    if (r > (float)w / 2) r = (float)w / 2;
    if (r > (float)h / 2) r = (float)h / 2;
    int edge = (int)ceilf(r),
        side = edge < (w + 1) / 2 ? edge : (w + 1) / 2; /* a corner's columns */
    for (int i = 0; i < h; ++i) {
        pix c = rows ? rows[i] : rgb;
        float py = (float)i + 0.5f, low = (float)h - r;
        float qy = r > py ? r - py : py > low ? py - low : 0; /* into a corner */
        if (qy <= 0) {                                        /* a row between the corners */
            if (rows || i < edge || i >= h - edge) fill(page, x, y + i, w, 1, c, alpha);
            continue;
        }
        for (int k = 0; k < side; ++k) { /* the corner's pixels: the left one and its mirror */
            float qx = r - ((float)k + 0.5f);
            float in = qx <= 0 ? 1 : sat(0.5f - (sqrtf(qx * qx + qy * qy) - r));
            int a = (int)((float)alpha * in + 0.5f);
            dot(page, x + k, y + i, c, a);
            if (w - 1 - k != k) dot(page, x + w - 1 - k, y + i, c, a);
        }
        fill(page, x + edge, y + i, w - 2 * edge, 1, c, alpha);
    }
    /* the rows between, at once when they are one colour: the page's memory in order */
    if (!rows) fill(page, x, y + edge, w, h - 2 * edge, rgb, alpha);
}

void disc(pix *page, float cx, float cy, float r, pix rgb, int alpha) {
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); ++y)
        for (int x = (int)floorf(cx - r); x <= (int)ceilf(cx + r); ++x) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            dot(page, x, y, rgb,
                (int)((float)alpha * sat(0.5f - (sqrtf(dx * dx + dy * dy) - r)) + 0.5f));
        }
}

void poly(pix *page, const float *xy, int n, pix rgb, int alpha) {
    float x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1], area = 0;
    for (int i = 0; i < n; ++i) {
        const float *a = xy + 2 * i, *b = xy + 2 * ((i + 1) % n);
        if (a[0] < x0) x0 = a[0];
        if (a[0] > x1) x1 = a[0];
        if (a[1] < y0) y0 = a[1];
        if (a[1] > y1) y1 = a[1];
        area += a[0] * b[1] - b[0] * a[1]; /* its sign: which way round the corners go */
    }
    for (int y = (int)floorf(y0); y <= (int)ceilf(y1); ++y)
        for (int x = (int)floorf(x0); x <= (int)ceilf(x1); ++x) {
            int in = 0; /* of 16 points in the pixel, those inside every edge */
            for (int s = 0; s < 16; ++s) {
                float px = (float)x + ((float)(s % 4) + 0.5f) / 4,
                      py = (float)y + ((float)(s / 4) + 0.5f) / 4;
                int inside = 1;
                for (int i = 0; i < n && inside; ++i) {
                    const float *a = xy + 2 * i, *b = xy + 2 * ((i + 1) % n);
                    float side = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                    inside = area >= 0 ? side >= 0 : side <= 0;
                }
                in += inside;
            }
            dot(page, x, y, rgb, alpha * in / 16);
        }
}

/* ---- Text ---------------------------------------------------------------------------------- */

#define FONT "/release/assets/default/raw/fonts/default.ttf"
#define GLYPHS 2048             /* the cache's slots */
#define GLYPH_BYTES (384 << 10) /* and what its bitmaps may take */

typedef struct {
    unsigned key; /* the code point and the size; 0 an empty slot */
    short w, h, x0, y0;
    float advance;
    unsigned char *bits; /* w x h coverage */
} glyph_t;

static stbtt_fontinfo font;
static int font_ok, font_up, font_down; /* its ascent and descent, in its own units */
static glyph_t cache[GLYPHS];
static int cached;
static size_t cache_bytes;

int font_open(void) {
    const char *path = getenv("VIDEOS_FONT");
    struct stat st;
    int fd = open(path && *path ? path : FONT, O_RDONLY | O_CLOEXEC), gap;
    if (fd < 0) return 0;
    /* mapped, not read: 10 MB of which a video's few letters touch a little */
    void *data =
        fstat(fd, &st) ? MAP_FAILED : mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return 0;
    if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return munmap(data, (size_t)st.st_size), 0;
    stbtt_GetFontVMetrics(&font, &font_up, &font_down, &gap);
    return font_ok = 1;
}

unsigned utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p, n = c >= 0xf0 && c < 0xf8   ? 3
                         : c >= 0xe0 && c < 0xf0 ? 2
                         : c >= 0xc2 && c < 0xe0 ? 1
                                                 : 0;
    if (!c) return 0;
    if (n) {
        static const unsigned least[4] = { 0, 0x80, 0x800, 0x10000 };
        unsigned v = c & (0x3fu >> n), k = 1;
        for (; k <= n && (p[k] & 0xc0) == 0x80; ++k) v = v << 6 | (p[k] & 0x3f);
        /* a whole sequence, and no longer than its code point needs */
        if (k > n && v >= least[n] && v <= 0x10ffff && (v < 0xd800 || v > 0xdfff))
            return *s += n + 1, v;
    }
    return *s += 1, c; /* ASCII, or a byte that is no UTF-8: Latin-1 */
}

/* ponytail: the cache is emptied whole when it is full; a subtitle in a script of thousands of
 * letters draws them again now and then. Evict the oldest if that ever shows. */
static void cache_clear(void) {
    for (int i = 0; i < GLYPHS; ++i) free(cache[i].bits);
    memset(cache, 0, sizeof cache);
    cached = 0, cache_bytes = 0;
}

static const glyph_t *glyph(unsigned cp, int px) {
    static const glyph_t none;
    if (!font_ok || px < 4 || px > 255) return &none;
    unsigned key = cp | (unsigned)px << 21 | 1u << 31, at = key * 2654435761u % GLYPHS;
    for (; cache[at].key; at = (at + 1) % GLYPHS)
        if (cache[at].key == key) return &cache[at];
    if (cached >= GLYPHS * 3 / 4 || cache_bytes > GLYPH_BYTES) {
        cache_clear();
        at = key * 2654435761u % GLYPHS;
    }
    glyph_t *g = &cache[at];
    float sc = stbtt_ScaleForPixelHeight(&font, (float)px);
    int index = stbtt_FindGlyphIndex(&font, (int)cp), adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&font, index, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&font, index, sc, sc, &x0, &y0, &x1, &y1);
    g->key = key, g->advance = (float)adv * sc;
    g->x0 = (short)x0, g->y0 = (short)y0, g->w = (short)(x1 - x0), g->h = (short)(y1 - y0);
    if (g->w > 0 && g->h > 0 && (g->bits = malloc((size_t)(g->w * g->h)))) {
        stbtt_MakeGlyphBitmap(&font, g->bits, g->w, g->h, g->w, sc, sc, index);
        cache_bytes += (size_t)(g->w * g->h);
    } else
        g->w = g->h = 0;
    ++cached;
    return g;
}

static float width(const char *s, int px, int track) {
    float w = 0;
    for (unsigned cp; (cp = utf8_next(&s));) w += glyph(cp, px)->advance + (float)track;
    return w > 0 ? w - (float)track : 0;
}

int text_w(const char *s, int px, int track) { return (int)(width(s, px, track) + 0.5f); }

void text_at(pix *page, int x, int y, const char *s, int px, int track, pix rgb, int alpha) {
    if (!font_ok) return;
    float pen = (float)x, sc = stbtt_ScaleForPixelHeight(&font, (float)px);
    int base = y + (int)((float)font_up * sc + 0.5f);
    for (unsigned cp; (cp = utf8_next(&s));) {
        const glyph_t *g = glyph(cp, px);
        int gx = (int)floorf(pen + 0.5f) + g->x0, gy = base + g->y0;
        for (int i = 0; i < g->w; ++i)
            for (int k = 0; k < g->h; ++k) {
                int a = g->bits[k * g->w + i];
                if (a) dot(page, gx + i, gy + k, rgb, a * alpha / 255);
            }
        pen += g->advance + (float)track;
    }
}

void text_outlined(pix *page, int x, int y, const char *s, int px) {
    for (int i = 0; i < 9; ++i)
        if (i != 4) text_at(page, x + i % 3 - 1, y + i / 3 - 1, s, px, 0, 0x000000, 255);
    text_at(page, x, y, s, px, 0, 0xffffff, 255);
}

#define DOTS "\xe2\x80\xa6" /* … */

const char *text_fit(const char *s, int px, int track, int w, char *out, size_t n) {
    if (width(s, px, track) <= (float)w && strlen(s) < n) return strcpy(out, s);
    float used = width(DOTS, px, track) + (float)track;
    const char *p = s, *end = s; /* end: where the last letter that fits ends */
    for (unsigned cp; (cp = utf8_next(&p));) {
        used += glyph(cp, px)->advance + (float)track;
        if (used - (float)track > (float)w || (size_t)(p - s) + sizeof DOTS > n) break;
        if (cp != ' ') end = p; /* no space before the dots */
    }
    snprintf(out, n, "%.*s%s", (int)(end - s), s, n > sizeof DOTS ? DOTS : "");
    return out;
}

static int cjk(unsigned cp) { return cp >= 0x2e80 && cp <= 0xffef; }

int text_wrap(const char *s, int px, int w, char lines[][LINE_N], int max) {
    int n = 0;
    while (*s) {
        if (*s == '\n' || *s == ' ') { /* an empty line takes none of the screen's */
            ++s;
            continue;
        }
        if (n == max) { /* more than fits: the last line says so */
            char *line = lines[n - 1], *end = line + strlen(line), with[LINE_N + sizeof DOTS];
            while (snprintf(with, sizeof with, "%.*s" DOTS, (int)(end - line), line),
                   end > line && (width(with, px, 0) > (float)w || strlen(with) >= LINE_N))
                do --end; /* a letter less, and no space before the dots */
                while (end > line && ((*end & 0xc0) == 0x80 || end[-1] == ' '));
            strcpy(line, with);
            break;
        }
        const char *p = s, *at, *brk_end = 0, *brk_next = 0, *end = 0;
        float used = 0;
        int after_cjk = 0;
        for (unsigned cp; at = p, (cp = utf8_next(&p)) && cp != '\n';) {
            if (cp == ' ') {
                if (at > s) brk_end = at, brk_next = p;
            } else if (at > s && (cjk(cp) || after_cjk))
                brk_end = brk_next = at;
            used += glyph(cp, px)->advance;
            if (cp != ' ' && used > (float)w && at > s) { /* this letter is one too many */
                end = brk_end ? brk_end : at;
                p = brk_end ? brk_next : at;
                break;
            }
            if (p - s >= LINE_N - 4) { /* nor more than a line's buffer holds */
                end = p;
                break;
            }
            after_cjk = cjk(cp);
        }
        if (!end) end = at, p = *at == '\n' ? at + 1 : at; /* it all fits: to the line's end */
        while (end > s && end[-1] == ' ') --end;
        snprintf(lines[n++], LINE_N, "%.*s", (int)(end - s), s);
        s = p;
    }
    return n;
}
