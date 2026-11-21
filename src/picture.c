/* Where a picture lies on the screen, how it gets onto a page, and how much of it is decoded
 * when the Q2 can't keep up. */
#include "player.h"
#include <string.h>

void geometry(int w, int h, int sar_num, int sar_den, int rot, int fill, geom_t *g) {
    if (sar_num <= 0 || sar_den <= 0) sar_num = sar_den = 1;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    rot = ((rot % 360 + 360) % 360 + 45) / 90 % 4 * 90;
    int turned = rot == 90 || rot == 270;
    /* its shape as shown: the source's pixels sar wide, then the turn */
    double aw = (double)w * sar_num / sar_den, ah = h, pw = turned ? ah : aw, ph = turned ? aw : ah;
    *g = (geom_t){ .sw = w, .sh = h, .rot = rot };
    if (fill) {
        /* the screen's shape cut from its middle: off its sides when it is the wider, else off
         * its top and bottom; even, as the chroma's rows and columns are halved */
        int wider = pw * VIEW_H > ph * VIEW_W;
        double keep = wider ? ph * VIEW_W / VIEW_H / pw : pw * VIEW_H / VIEW_W / ph;
        if (wider != turned) { /* the cut runs along the source's width */
            g->sw = (int)(w * keep + 0.5) & ~1;
            if (g->sw < 2) g->sw = w < 2 ? w : 2;
            g->sx = (w - g->sw) / 2 & ~1;
        } else {
            g->sh = (int)(h * keep + 0.5) & ~1;
            if (g->sh < 2) g->sh = h < 2 ? h : 2;
            g->sy = (h - g->sh) / 2 & ~1;
        }
        g->vw = VIEW_W, g->vh = VIEW_H;
    } else {
        double by = VIEW_W / pw < VIEW_H / ph ? VIEW_W / pw : VIEW_H / ph;
        g->vw = (int)(pw * by + 0.5), g->vh = (int)(ph * by + 0.5);
        if (g->vw < 1) g->vw = 1;
        if (g->vh < 1) g->vh = 1;
        if (g->vw > VIEW_W) g->vw = VIEW_W;
        if (g->vh > VIEW_H) g->vh = VIEW_H;
    }
    g->vx = (VIEW_W - g->vw) / 2, g->vy = (VIEW_H - g->vh) / 2;
    g->dw = turned ? g->vh : g->vw, g->dh = turned ? g->vw : g->vh;
}

void picture_place(pix *page, const pix *img, int line, const geom_t *g) {
    if (g->vw < VIEW_W || g->vh < VIEW_H)
        for (int i = 0; i < PAGE_PX; ++i) page[i] = 0xff000000u; /* the bars */
    /* The picture's (u, v) is the page's pixel base + u * du + v * dv: the view's (x, y) is the
     * page's x * PAGE_W + PAGE_W - 1 - y, and the turn says which of u and v x and y are. */
    int x = g->vx, y = PAGE_W - 1 - g->vy, du, dv, base;
    switch (g->rot) {
    case 90:
        du = -1, dv = -PAGE_W, base = (x + g->dh - 1) * PAGE_W + y;
        break;
    case 180:
        du = -PAGE_W, dv = 1, base = (x + g->dw - 1) * PAGE_W + y - (g->dh - 1);
        break;
    case 270:
        du = 1, dv = PAGE_W, base = x * PAGE_W + y - (g->dw - 1);
        break;
    default:
        du = PAGE_W, dv = -1, base = x * PAGE_W + y;
    }
    /* Where u steps a whole row of the page, 16 columns of the picture at a time: sixteen runs
     * of the page's memory written in step, not a new row for every pixel. */
    int strip = du == 1 || du == -1 ? g->dw : 16;
    for (int u0 = 0; u0 < g->dw; u0 += strip) {
        int n = g->dw - u0 < strip ? g->dw - u0 : strip;
        for (int v = 0; v < g->dh; ++v) {
            const pix *s = img + v * line + u0;
            pix *d = page + base + u0 * du + v * dv;
            for (int k = 0; k < n; ++k) d[k * du] = s[k];
        }
    }
}

void ladder_init(ladder_t *l, int w, int h, long long now) {
    *l = (ladder_t){ .floor = (long long)w * h > 1280 * 720, .ahead_since = now, .hold = 2000 };
    l->level = l->floor;
    l->eased_at = l->late_at = -1000000;
}

int ladder_frame(ladder_t *l, long long now, int late, int keyframe) {
    if (l->level == 3 && keyframe) l->level = 2, l->ahead_since = now; /* caught up at a keyframe */
    if (late) {
        if (now - l->late_at > 1000) l->late = 0; /* another bout */
        l->late_at = l->ahead_since = now;
        if (++l->late >= 3 && l->level < 3) {
            ++l->level, l->late = 0;
            if (now - l->eased_at < 5000 && l->hold < 60000) l->hold *= 2; /* eased off too soon */
        }
    } else if (l->level > l->floor && l->level < 3 && now - l->ahead_since >= l->hold)
        --l->level, l->ahead_since = l->eased_at = now;
    return l->level;
}
