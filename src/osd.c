/* What is drawn over the picture: the controls, the volume, subtitles, the menu, and the whole
 * screens (loading, a video that can't be played). Flat: black bands at about 55% and white
 * text; the menu is Q2 Pod's own pop-up, in the colours it hands a card app in $Q2_POPUP. */
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHITE 0xffffff
#define BAND 140 /* a band's black over the picture, of 255 */
#define TOP_H 44
#define BOTTOM_Y 266
#define BAR_X 21 /* the bar, as Now Playing's: where the glass's corners let it start */
#define BAR_W 333
#define BAR_Y 283
#define BAR_H 6

/* ---- Q2 Pod's pop-up ----------------------------------------------------------------------- */

static const char *hex(const char *s, pix *c) { /* " RRGGBB" into c: past it, else 0 */
    unsigned v = 0;
    if (*s++ != ' ') return 0;
    for (int i = 0; i < 6; ++i, ++s) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (unsigned)d;
    }
    *c = v;
    return s;
}

void pop_read(pop_t *p, const char *s) {
    static const char *const names[] = { "", "minimal", "modern", "classic" };
    pop_t got = { 0 };
    /* without one: the modern box, its chosen row on grey */
    *p = (pop_t){ POP_MODERN, 0x1c1c1c, 0x3a3a3a, 0xffffff, 0xaaaaaa,
                  0x5a5a5a,   0xffffff, { 0 },    { 0 } };
    if (!s) return;
    for (int i = POP_MINIMAL; i <= POP_CLASSIC && !got.style; ++i) {
        size_t n = strlen(names[i]);
        if (!strncmp(s, names[i], n) && s[n] == ' ') got.style = i, s += n;
    }
    if (!got.style) return;
    pix *c[6] = { &got.box, &got.frame, &got.title, &got.text, &got.pick, &got.pick_text };
    for (int i = 0; i < 6; ++i)
        if (!(s = hex(s, c[i]))) return;
    for (int i = 0; got.style == POP_CLASSIC && i < 2 * POP_ROW; ++i)
        if (!(s = hex(s, i < POP_ROW ? &got.head[i] : &got.band[i - POP_ROW]))) return;
    if (!*s || *s == ' ') *p = got; /* a later Q2 Pod's further fields, after a space: not ours */
}

void menu_move(menu_t *m, int by) {
    int at = m->sel, step = by < 0 ? -1 : 1;
    for (by = abs(by); by; --by) { /* a row at a time, over the ones that can't be chosen */
        int to = at + step;
        while (to >= 0 && to < m->n && m->row[to].off) to += step;
        if (to < 0 || to >= m->n) break;
        at = to;
    }
    m->sel = at;
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + MENU_SHOWN) m->top = m->sel - MENU_SHOWN + 1;
}

/* The box, as Q2 Pod's popup() lays it out: the title's row, then the rows shown. */
static void menu_box(const pop_t *p, const menu_t *m, int *y, int *h, int *top) {
    int shown = m->n < MENU_SHOWN ? m->n : MENU_SHOWN, classic = p->style == POP_CLASSIC;
    *top = POP_ROW + (classic ? 1 : 6);
    *h = *top + shown * POP_ROW + (classic ? 0 : 8);
    *y = (VIEW_H - *h) / 2;
}

int menu_hit(const pop_t *p, const menu_t *m, int x, int y) {
    int by, h, top, bx = (VIEW_W - POP_W) / 2;
    menu_box(p, m, &by, &h, &top);
    if (x < bx || x >= bx + POP_W || y < by || y >= by + h) return -2;
    int row = y < by + top ? -1 : m->top + (y - by - top) / POP_ROW;
    return row < 0 || row >= m->n || row >= m->top + MENU_SHOWN || m->row[row].off ? -1 : row;
}

/* Minimal's text is capitals, 2 px apart (Q2 Pod's SUDO_TRACK): ASCII's letters raised, as
 * Q2 Pod's own rows are; any other script stays as it is. */
static const char *styled(const pop_t *p, const char *s, char *buf, size_t n) {
    if (p->style != POP_MINIMAL) return s;
    snprintf(buf, n, "%s", s);
    for (char *c = buf; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    return buf;
}

static void menu_draw(pix *page, const pop_t *p, const menu_t *m) {
    int x = (VIEW_W - POP_W) / 2, y, h, top, classic = p->style == POP_CLASSIC,
        minimal = p->style == POP_MINIMAL;
    int track = minimal ? 2 : 0, shown = m->n < MENU_SHOWN ? m->n : MENU_SHOWN;
    /* the row's text: Classic's larger, from the box's edge; Modern's inside its bar; Minimal's
     * beside its dot */
    int px = classic ? 23 : 18, small = classic ? 18 : 15, left = classic ? 16 : minimal ? 40 : 22;
    int right = classic ? 16 : 22;
    char buf[128], fit[128];
    menu_box(p, m, &y, &h, &top);
    fill(page, 0, 0, VIEW_W, VIEW_H, 0x000000, 128); /* what is under it, dimmed: black at half */
    if (classic) { /* its line round it, then the strip, its line and the paper, the band over that
                    */
        pix rows[POP_ROW + 1 + MENU_SHOWN * POP_ROW];
        for (int i = 0; i < h; ++i)
            rows[i] = i < POP_ROW ? p->head[i] : i == POP_ROW ? p->frame : p->box;
        if (m->sel >= m->top && m->sel < m->top + shown)
            memcpy(rows + top + (m->sel - m->top) * POP_ROW, p->band, sizeof p->band);
        rbox(page, x - 1, y - 1, POP_W + 2, h + 2, 13, p->frame, 255, 0);
        rbox(page, x, y, POP_W, h, 12, 0, 255, rows);
    } else { /* the box in its frame: Modern's the rounder */
        float r = minimal ? 6 : 12;
        rbox(page, x, y, POP_W, h, r, p->frame, 255, 0);
        rbox(page, x + 1, y + 1, POP_W - 2, h - 2, r - 1, p->box, 255, 0);
    }
    const char *title = styled(p, m->title, buf, sizeof buf);
    int tpx = classic ? 21 : 20;
    text_at(page, (VIEW_W - text_w(title, tpx, track)) / 2,
            y + (classic ? 0 : 4) + (POP_ROW - tpx) / 2, title, tpx, track, p->title, 255);
    for (int i = 0; i < shown; ++i) {
        const row_t *r = &m->row[m->top + i];
        int ry = y + top + i * POP_ROW, on = m->top + i == m->sel;
        pix ink = on ? p->pick_text : p->text;
        int alpha = r->off ? 110 : 255, vw = 0;
        if (on && p->style == POP_MODERN)
            rbox(page, x + 10, ry + 1, POP_W - 20, POP_ROW - 2, 6, p->pick, 255, 0);
        if (on && minimal) rbox(page, x + 24, ry + (POP_ROW - 6) / 2, 6, 6, 3, p->pick, 255, 0);
        if (*r->value) { /* at the right, smaller, and never more than half the row */
            const char *v = text_fit(styled(p, r->value, buf, sizeof buf), small, track,
                                     (POP_W - left - right) / 2, fit, sizeof fit);
            vw = text_w(v, small, track);
            text_at(page, x + POP_W - right - vw, ry + (POP_ROW - small) / 2, v, small, track, ink,
                    on ? alpha : alpha * 3 / 4);
            vw += 10;
        }
        const char *label = text_fit(styled(p, r->label, buf, sizeof buf), px, track,
                                     POP_W - left - right - vw, fit, sizeof fit);
        text_at(page, x + left, ry + (POP_ROW - px) / 2, label, px, track, ink, alpha);
    }
    if (m->n > shown) { /* more than fit: where in the list the rows shown are */
        int room = shown * POP_ROW - 8, len = room * shown / m->n,
            at = (room - len) * m->top / (m->n - shown);
        rbox(page, x + POP_W - 6, y + top + 4 + at, 3, len < 8 ? 8 : len, 1.5f,
             classic ? p->frame : p->text, 160, 0);
    }
}

/* ---- Subtitles ----------------------------------------------------------------------------- */

#define SUB_BOTTOM 262 /* the last line's bottom: above the bottom band, on the picture, always */
#define SUB_SIDE 12
#define SUB_RING 2 /* the outline, px */
#define SUB_H                                                                                      \
    (SUB_LINES * 18 + 2 * SUB_RING)           /* the rows they can take, the outline's with them   \
                                               */
#define SUB_Y (SUB_BOTTOM + SUB_RING - SUB_H) /* the first of those rows, in the view */

int subs_layout(const char *text, int subsize, char lines[][LINE_N], int *px) {
    static const int sizes[] = { 24, 20, 18, 16, 14 }, from[3] = { 3, 1, 0 };
    int n = 0;
    for (int i = from[subsize < 0 || subsize > 2 ? 1 : subsize]; i < 5; ++i) {
        *px = sizes[i]; /* a fourth line asked for says whether three were enough */
        n = text_wrap(text, *px, VIEW_W - 2 * SUB_SIDE, lines, i < 4 ? 4 : SUB_LINES);
        if (n <= 3) break;
    }
    return n;
}

/* The lines as they are drawn, made once for each text: white letters in a black outline two
 * pixels wide, so they read over any picture, a white one too. cover is how much of the page
 * each pixel hides, white its own grey. */
static struct {
    char text[3 * LINE_N];
    int size, top, x0, x1; /* the rows from top and the columns from x0 to x1 have something */
    unsigned char cover[SUB_H * VIEW_W], white[SUB_H * VIEW_W];
} sub = { .size = -1 };

static void subs_render(const char *text, int subsize) {
    char lines[SUB_LINES][LINE_N];
    int px, n = subs_layout(text, subsize, lines, &px), step = px + 4;
    snprintf(sub.text, sizeof sub.text, "%s", text), sub.size = subsize;
    memset(sub.white, 0, sizeof sub.white), memset(sub.cover, 0, sizeof sub.cover);
    sub.top = SUB_H - SUB_RING - px - (n - 1) * step - SUB_RING, sub.x0 = VIEW_W, sub.x1 = 0;
    if (sub.top < 0) sub.top = 0;
    for (int i = 0; i < n; ++i) {
        int w = text_w(lines[i], px, 0), x = (VIEW_W - w) / 2;
        text_mask(sub.white, VIEW_W, SUB_H, x, SUB_H - SUB_RING - px - (n - 1 - i) * step, lines[i],
                  px);
        if (x - 4 < sub.x0) sub.x0 = x - 4 < 0 ? 0 : x - 4; /* a letter may lean past its place */
        if (x + w + 4 > sub.x1) sub.x1 = x + w + 4 > VIEW_W ? VIEW_W : x + w + 4;
    }
    for (int y = sub.top; y < SUB_H; ++y)
        for (int x = sub.x0; x < sub.x1; ++x) {
            unsigned ring = 0, in = sub.white[y * VIEW_W + x];
            for (int dy = -SUB_RING; dy <= SUB_RING; ++dy) /* the most of any letter within 2 px */
                for (int dx = -SUB_RING; dx <= SUB_RING; ++dx) {
                    int qx = x + dx, qy = y + dy;
                    if (abs(dx) + abs(dy) < 2 * SUB_RING && qx >= 0 && qx < VIEW_W && qy >= 0 &&
                        qy < SUB_H && sub.white[qy * VIEW_W + qx] > ring)
                        ring = sub.white[qy * VIEW_W + qx];
                }
            sub.cover[y * VIEW_W + x] = (unsigned char)(ring + in - ring * in / 255);
        }
}

/* ---- The bands ----------------------------------------------------------------------------- */

const char *time_text(long long ms, char *out, size_t n) {
    long long s = (ms < 0 ? -ms : ms) / 1000;
    if (s >= 3600)
        snprintf(out, n, "%s%lld:%02lld:%02lld", ms < 0 ? "-" : "", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%s%lld:%02lld", ms < 0 ? "-" : "", s / 60, s % 60);
    return out;
}

const char *skip_text(int secs, char *out, size_t n) {
    int s = abs(secs);
    if (s < 60)
        snprintf(out, n, "%+d s", secs);
    else
        snprintf(out, n, "%c%d:%02d", secs < 0 ? '-' : '+', s / 60, s % 60);
    return out;
}

int hit(int x, int y) {
    if (y < TOP_H + 8 && x >= VIEW_W - 78) return HIT_MENU; /* the dots, with a finger's room */
    if (y >= BOTTOM_Y - 4) return HIT_BAR;
    if (x >= VIEW_W / 3 && x < VIEW_W - VIEW_W / 3 && y >= TOP_H + 8) return HIT_MIDDLE;
    return HIT_NONE;
}

long long bar_ms(int x, long long dur_ms) {
    x = x < BAR_X ? 0 : x > BAR_X + BAR_W ? BAR_W : x - BAR_X;
    return dur_ms * x / BAR_W;
}

/* A bar from x, w wide, filled to part of whole. */
static void bar(pix *page, int x, int w, long long part, long long whole) {
    int to = whole > 0 ? (int)(w * (part < 0 ? 0 : part > whole ? whole : part) / whole) : 0;
    rbox(page, x, BAR_Y, w, BAR_H, 3, 0x5a5a5a, 255, 0);
    if (to >= 2) rbox(page, x, BAR_Y, to < BAR_H ? BAR_H : to, BAR_H, 3, WHITE, 255, 0);
}

static void centred(pix *page, int y, const char *s, int px, int w, pix rgb) {
    char fit[LINE_N];
    s = text_fit(s, px, 0, w, fit, sizeof fit);
    text_at(page, (VIEW_W - text_w(s, px, 0)) / 2, y, s, px, 0, rgb, 255);
}

/* A short text on a flat dark pill, its middle at (cx, cy). */
static void pill(pix *page, int cx, int cy, const char *s, int px) {
    char fit[LINE_N];
    s = text_fit(s, px, 0, VIEW_W - 2 * clear_x(cy - 14, cy + 14) - 24, fit, sizeof fit);
    int w = text_w(s, px, 0) + 24, h = px + 12;
    rbox(page, cx - w / 2, cy - h / 2, w, h, (float)h / 2, 0x000000, 160, 0);
    text_at(page, cx - w / 2 + 12, cy - px / 2, s, px, 0, WHITE, 255);
}

int osd_any(const osd_t *o) {
    return o->controls || o->disc || o->target || o->volume >= 0 || o->badge || o->notice ||
           (o->subs && *o->subs) || o->menu || o->message || o->plain;
}

void osd_draw(pix *page, const osd_t *o) {
    char a[LINE_N];
    if (o->plain) fill(page, 0, 0, VIEW_W, VIEW_H, 0x000000, 255);
    if (o->message) { /* a whole screen: the title, what is going on, and why */
        char lines[4][LINE_N];
        int w = VIEW_W - 48, n = o->reason ? text_wrap(o->reason, 16, w, lines, 4) : 0;
        int y = (VIEW_H - (o->reason ? 62 + n * 22 : 50)) / 2;
        centred(page, y, o->title, o->reason ? 15 : 18, w, o->reason ? 0x8a8a8a : WHITE);
        centred(page, y + 28, o->message, o->reason ? 20 : 16, w, o->reason ? WHITE : 0x9a9a9a);
        for (int i = 0; i < n; ++i) centred(page, y + 62 + i * 22, lines[i], 16, w, 0xaaaaaa);
        return;
    }
    if (o->plain && !o->controls)
        centred(page, 151, o->title, 18, VIEW_W - 48, WHITE); /* sound alone */
    int band = o->controls || o->volume >= 0;                 /* the bottom band is up */
    if (o->subs && *o->subs) { /* in one place, whatever else is up */
        if (strcmp(o->subs, sub.text) || o->subsize != sub.size) subs_render(o->subs, o->subsize);
        if (sub.x1 > sub.x0)
            mask_over(page, sub.x0, SUB_Y + sub.top, sub.x1 - sub.x0, SUB_H - sub.top, VIEW_W,
                      sub.cover + sub.top * VIEW_W + sub.x0, sub.white + sub.top * VIEW_W + sub.x0);
    }
    if (o->controls) {
        fill(page, 0, 0, VIEW_W, TOP_H, 0x000000, BAND);
        /* the title between the corners, and clear of the dots at its right */
        int room = VIEW_W - 2 * 72;
        centred(page, 15, o->title, 18, room, WHITE);
        for (int i = 0; i < 3; ++i) disc(page, 324.5f + (float)i * 7, 24.5f, 2.5f, WHITE, 255);
    }
    if (band) fill(page, 0, BOTTOM_Y, VIEW_W, VIEW_H - BOTTOM_Y, 0x000000, BAND);
    if (o->volume >= 0) { /* a speaker, and the bar filled to the level */
        /* in two parts, each convex: the whole isn't */
        static const float body[] = { 46, 283, 51, 283, 51, 289, 46, 289 },
                           cone[] = { 51, 283, 58, 277, 58, 295, 51, 289 };
        poly(page, body, 4, WHITE, 255);
        poly(page, cone, 4, WHITE, 255);
        bar(page, 72, 257, o->volume, 100);
    } else if (o->controls) {
        bar(page, BAR_X, BAR_W, o->pos_ms, o->dur_ms);
        for (int i = 1; i < o->nchapters && o->dur_ms > 0;
             ++i) { /* a gap where each chapter begins */
            long long at = o->chapter[i];
            if (at > 0 && at < o->dur_ms)
                fill(page, BAR_X + (int)(BAR_W * at / o->dur_ms), BAR_Y, 2, BAR_H, 0x000000, 255);
        }
        if (o->knob && o->dur_ms > 0)
            disc(page, (float)BAR_X + (float)BAR_W * (float)o->pos_ms / (float)o->dur_ms,
                 (float)BAR_Y + 3, 7, WHITE, 255);
        text_at(page, 46, 296, time_text(o->pos_ms, a, sizeof a), 14, 0, WHITE, 255);
        if (o->paused) /* the pause's two bars, between the times */
            fill(page, 183, 298, 3, 11, WHITE, 255), fill(page, 190, 298, 3, 11, WHITE, 255);
        if (o->dur_ms > 0) {
            time_text(o->pos_ms > o->dur_ms ? 0 : o->pos_ms - o->dur_ms, a, sizeof a);
            text_at(page, 329 - text_w(a, 14, 0), 296, a, 14, 0, WHITE, 255);
        }
    }
    if (o->disc) { /* the pause's two bars on a flat disc */
        disc(page, VIEW_W / 2.0f, VIEW_H / 2.0f, 32, 0x000000, 150);
        fill(page, VIEW_W / 2 - 11, VIEW_H / 2 - 14, 8, 28, WHITE, 255);
        fill(page, VIEW_W / 2 + 4, VIEW_H / 2 - 14, 8, 28, WHITE, 255);
    }
    if (o->target) pill(page, VIEW_W / 2, VIEW_H / 2, time_text(o->pos_ms, a, sizeof a), 18);
    if (o->badge) /* a side button's seek so far, at its side */
        pill(page, o->badge < 0 ? 78 : VIEW_W - 78, VIEW_H / 2, skip_text(o->badge, a, sizeof a),
             18);
    if (o->notice) pill(page, VIEW_W / 2, TOP_H + 26, o->notice, 15);
    if (o->menu) menu_draw(page, o->pop, o->menu);
}
