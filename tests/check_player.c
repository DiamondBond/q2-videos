/* The player's checks (make check, under ASan and UBSan): its pure parts first, then whole runs
 * of player_main over the test clips. A run is a child process with a script for its input
 * (src/platform_script.c) on the script's own clock, so thirty seconds of video take as long as
 * their decoding and the same script always shows the same frames; what happened is read from
 * the log the player writes at each frame the script asks for, from those frames, and from the
 * marks, the settings and the volume file it leaves.
 *
 * With --qemu "<command>" (make qemu-check) the same kind of run is made twice, here and by the
 * Q2's own binary under the emulator against the Q2's FFmpeg, and the two must show the same.
 *
 * The clips are tools/clips.sh's ($VIDEOS_CLIPS, else ~/.cache/q2-videos/clips); the font is the
 * Q2's ($VIDEOS_FONT, else the one the Makefile takes from a Q2 Pod rootfs). */
#include "player.h"
#include "check.h"
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CARD "build/player-card"
#define TMP "build/player-tmp"
#define OUT "build/player-out"
#define FONT_ROOTFS "build/rootfs/release/assets/default/raw/fonts/default.ttf"
/* a is within by of b */
#define NEAR(a, b, by)                                                                             \
    ((a) >= (b) - (by) && (a) <= (b) + (by)                                                        \
         ? (void)0                                                                                 \
         : (fprintf(stderr, "%s:%d: %s is %lld, not within %lld of %lld\n", __FILE__, __LINE__,    \
                    #a, (long long)(a), (long long)(by), (long long)(b)),                          \
            abort()))

static const char *clips, *qemu;

/* ---- Subtitles ----------------------------------------------------------------------------- */

static void check_subs(void) {
    cues_t c = { 0 };
    const cue_t *on[4];
    long long next;
    /* a BOM, CRLF line ends, tags, a cue of two lines */
    const char *plain =
        "\xef\xbb\xbf"
        "1\r\n00:00:01,000 --> 00:00:03,000\r\n<i>Caf\xc3\xa9</i> d\xc3\xa9j\xc3\xa0 vu\r\n\r\n"
        "2\r\n00:00:04,000 --> 00:00:06,500\r\n{\\an8}One\r\n<b>Two</b>\r\n\r\n";
    CHECK(srt_parse(plain, strlen(plain), &c) == 2 && c.n == 2);
    CHECK(c.v[0].start == 1000 && c.v[0].end == 3000 && c.v[1].start == 4000 && c.v[1].end == 6500);
    SAME(c.v[0].text, "Caf\xc3\xa9 d\xc3\xa9j\xc3\xa0 vu"), SAME(c.v[1].text, "One\nTwo");
    cues_free(&c);
    /* bytes that are no UTF-8 stay as they are (drawn as Latin-1's); hours; a dot for the comma; no
     * blank line between two cues, and none at the end; out of order */
    const char *mixed = "9\n01:00:00.500 --> 01:00:02.000\nlater\n"
                        "1\n00:00:01,000 --> 00:00:03,000\nCaf\xe9 "
                        "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n"
                        "2\n00:00:02,000 --> 00:00:05,000\noverlaps";
    CHECK(srt_parse(mixed, strlen(mixed), &c) == 3);
    SAME(c.v[0].text, "Caf\xe9 \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
    SAME(c.v[1].text, "overlaps"), SAME(c.v[2].text, "later");
    CHECK(c.v[2].start == 3600500 && c.v[2].end == 3602000);
    /* which are on show, and when that next changes */
    CHECK(cues_at(&c, 0, on, 4, &next) == 0 && next == 1000);
    CHECK(cues_at(&c, 1000, on, 4, &next) == 1 && on[0] == &c.v[0] && next == 2000);
    CHECK(cues_at(&c, 2500, on, 4, &next) == 2 && on[0] == &c.v[0] && on[1] == &c.v[1] &&
          next == 3000);
    CHECK(cues_at(&c, 2500, on, 1, &next) == 1 && next == 3000); /* no more than asked for */
    CHECK(cues_at(&c, 3000, on, 4, &next) == 1 && on[0] == &c.v[1] && next == 5000);
    CHECK(cues_at(&c, 5000, on, 4, &next) == 0 && next == 3600500);
    CHECK(cues_at(&c, 3602000, on, 4, &next) == 0 && next == -1);
    cues_free(&c);
    /* nothing of a file that is no SubRip; a cue without text; times the wrong way round */
    const char *junk = "WEBVTT\n\nhello\n\n1\n00:00:05,000 --> "
                       "00:00:04,000\nbackwards\n\n2\n00:00:01,000 --> 00:00:02,000\n\n";
    CHECK(srt_parse(junk, strlen(junk), &c) == 0 && srt_parse("", 0, &c) == 0 && c.n == 0);
    cues_free(&c);

    char out[64], tiny[8];
    ass_plain("0,0,Default,,0,0,0,,{\\i1}Italic{\\i0} and plain", out, sizeof out);
    SAME(out, "Italic and plain");
    ass_plain("1,0,Default,,0,0,0,,First line\\NSecond line", out, sizeof out);
    SAME(out, "First line\nSecond line");
    ass_plain(
        "2,0,Default,,0,0,0,,{\\an8\\pos(320,40)\\b1}Top{\\b0}, with a comma\\hand a hard space",
        out, sizeof out);
    SAME(out, "Top, with a comma and a hard space");
    ass_plain("3,0,Default,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}after the drawing", out,
              sizeof out);
    SAME(out, "after the drawing");
    ass_plain("Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,a line as the file has it", out,
              sizeof out);
    SAME(out, "a line as the file has it");
    ass_plain("4,0,Default,,0,0,0,,an {unclosed brace", out, sizeof out);
    SAME(out, "an {unclosed brace");
    ass_plain("no commas at all", out, sizeof out);
    SAME(out, "no commas at all"); /* not an event: as it is */
    ass_plain("5,0,Default,,0,0,0,,\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", tiny,
              sizeof tiny);
    SAME(tiny, "\xd0\x9f\xd1\x80\xd0\xb8"); /* cut between letters, never inside one */
    char tags[] = "<i>slanted</i> and <font color=\"red\">red</font>, 2 < 3 {\\an8}top";
    tags_strip(tags), SAME(tags, "slanted and red, 2 < 3 top");

    SAME(lang_name("en"), "English"), SAME(lang_name("eng"), "English");
    SAME(lang_name("JPN"), "Japanese");
    SAME(lang_name("ru"), "Russian"), SAME(lang_name("tlh"), "tlh");

    /* the subtitle files beside a video: its own name first, then the ones with a word, by name;
     * case aside; no others */
    sidecar_t side[4];
    sh("rm -rf " TMP "/side && mkdir -p " TMP "/side && cd " TMP
       "/side && touch 'My Film.mkv' 'My Film.srt' "
       "'my film.EN.SRT' 'My Film.ja.srt' 'My Film 2.srt' 'My Film.txt' 'Other.srt' 'My "
       "Film.commentary.srt'");
    CHECK(sidecars(TMP "/side/My Film.mkv", side, 4) == 4);
    SAME(side[0].name, "External"), SAME(side[1].name, "commentary"), SAME(side[2].name, "English");
    SAME(side[3].name, "Japanese");
    SAME(side[0].path, TMP "/side/My Film.srt"), SAME(side[2].path, TMP "/side/my film.EN.SRT");
    CHECK(sidecars(TMP "/side/My Film.mkv", side, 2) == 2 &&
          sidecars(TMP "/side/Other.mp4", side, 4) == 1);
    CHECK(sidecars(TMP "/side/None.mp4", side, 4) == 0 &&
          sidecars(TMP "/nowhere/None.mp4", side, 4) == 0);
}

/* ---- Text, shapes, the corners ------------------------------------------------------------- */

static void check_draw(void) {
    /* the glass's corners: 80 px hidden in the first and last rows, none from the 80th */
    CHECK(inset(0) == 80 && inset(319) == 80 && inset(80) == 0 && inset(160) == 0 &&
          inset(239) == 0);
    CHECK(inset(24) == 23 && inset(295) == 23 && inset(283) == 14 &&
          inset(36) == 14); /* 80 - sqrt(80^2 - 56^2) = 22.9 */
    for (int y = 1; y < 160; ++y) CHECK(inset(y) <= inset(y - 1) && inset(y) == inset(319 - y));
    CHECK(clear_x(283, 288) == 21); /* the bar's rows: where it starts */
    CHECK(clear_x(100, 200) == 4 && clear_x(15, 33) == inset(15) + 4 &&
          clear_x(290, 310) == inset(310) + 4);

    const char *s = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\xff\xc3(",
               *p = s; /* A é € an emoji, then bytes that are no UTF-8 */
    CHECK(utf8_next(&p) == 'A' && utf8_next(&p) == 0xe9 && utf8_next(&p) == 0x20ac &&
          utf8_next(&p) == 0x1f600);
    CHECK(utf8_next(&p) == 0xff && utf8_next(&p) == 0xc3 && utf8_next(&p) == '(' &&
          utf8_next(&p) == 0 && !*p);
    p = "\xc0\x80\xed\xa0\x80"; /* an overlong zero and a surrogate: their bytes, one by one */
    CHECK(utf8_next(&p) == 0xc0 && utf8_next(&p) == 0x80 && utf8_next(&p) == 0xed);

    CHECK(font_open());
    /* widths add up, grow with the size, and tracking adds its pixels between the letters */
    int w = text_w("Hello", 18, 0);
    CHECK(w > 30 && w < 60 && text_w("", 18, 0) == 0 && text_w("Hello", 26, 0) > w);
    NEAR(text_w("HelloHello", 18, 0), 2 * w, 1);
    NEAR(text_w("Hello", 18, 2), w + 8, 1);
    CHECK(text_w("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 22, 0) >=
          3 * 15); /* CJK: about a square each */
    CHECK(text_w("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", 22, 0) > 40);

    /* what fits is left alone; what doesn't ends in dots within the width, cut at a letter */
    char out[LINE_N], small[12];
    SAME(text_fit("Short", 18, 0, 200, out, sizeof out), "Short");
    text_fit("A Rather Long Title for a Video That Will Not Fit", 18, 0, 200, out, sizeof out);
    CHECK(text_w(out, 18, 0) <= 200 && text_w(out, 18, 0) > 160 && strlen(out) > 12);
    CHECK(!strcmp(out + strlen(out) - 3, "\xe2\x80\xa6") && out[strlen(out) - 4] != ' ');
    text_fit("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe5\xad\x97\xe5\xb9\x95\xe3\x81\xa7"
             "\xe3\x81\x99",
             22, 0, 100, out, sizeof out);
    CHECK(text_w(out, 22, 0) <= 100 && strlen(out) % 3 == 0 &&
          strlen(out) >= 9); /* whole letters, then the dots */
    text_fit("A Rather Long Title", 18, 0, 1000, small,
             sizeof small); /* a buffer shorter than the text */
    CHECK(strlen(small) < sizeof small && !strcmp(small + strlen(small) - 3, "\xe2\x80\xa6"));
    SAME(text_fit("Wide", 18, 0, 0, out, sizeof out), "\xe2\x80\xa6");

    /* wrapping: at spaces, a line's own ends kept, each line within the width */
    char lines[3][LINE_N];
    const char *said = "I thought you said the meeting was tomorrow, not today.";
    char joined[3 * LINE_N] = "";
    int n = text_wrap(said, 22, 300, lines, 3);
    CHECK(n >= 2 && text_w(said, 22, 0) > 300);
    for (int i = 0; i < n; ++i) {
        CHECK(text_w(lines[i], 22, 0) <= 300 && *lines[i] && lines[i][0] != ' ' &&
              lines[i][strlen(lines[i]) - 1] != ' ');
        sprintf(joined + strlen(joined), "%s%s", i ? " " : "", lines[i]);
    }
    SAME(joined, said); /* broken at spaces, nothing lost */
    CHECK(text_wrap("First line\nSecond line", 22, 300, lines, 3) == 2);
    SAME(lines[0], "First line"), SAME(lines[1], "Second line");
    CHECK(text_wrap("\n\nOne\n\n", 22, 300, lines, 3) == 1 &&
          text_wrap("", 22, 300, lines, 3) == 0);
    /* CJK has no spaces: between any two letters */
    n = text_wrap("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe5\xad\x97\xe5\xb9\x95\xe3\x81"
                  "\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc"
                  "\xe3\x82\xb9\xe3\x81\xaa\xe3\x81\x97\xe3\x81\xa7\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf"
                  "\x94\xe3\x81\x99",
                  22, 200, lines, 3);
    CHECK(n == 2 && strlen(lines[0]) % 3 == 0 && text_w(lines[0], 22, 0) <= 200 &&
          text_w(lines[0], 22, 0) > 170);
    /* a word longer than a line breaks inside; more than the lines allowed ends in dots */
    n = text_wrap("Donaudampfschifffahrtsgesellschaftskapit\xc3\xa4nsm\xc3\xbctze", 22, 150, lines,
                  3);
    CHECK(n == 3 && text_w(lines[0], 22, 0) <= 150 && text_w(lines[0], 22, 0) > 120);
    n = text_wrap("one two three four five six seven eight nine ten eleven twelve thirteen "
                  "fourteen fifteen sixteen",
                  22, 150, lines, 3);
    CHECK(n == 3 && !strcmp(lines[2] + strlen(lines[2]) - 3, "\xe2\x80\xa6") &&
          text_w(lines[2], 22, 0) <= 150);
    n = text_wrap("a\nb\nc\nd", 22, 150, lines, 3);
    CHECK(n == 3), SAME(lines[2], "c\xe2\x80\xa6");

    /* drawing stays on the page whatever it is asked: ASan watches the rows around it */
    pix *page = malloc(PAGE_PX * sizeof *page);
    memset(page, 0, PAGE_PX * sizeof *page);
    fill(page, -50, -50, 1000, 1000, 0x102030, 255);
    CHECK(*view_at(page, 0, 0) == 0xff102030u && *view_at(page, 374, 319) == 0xff102030u);
    fill(page, 10, 10, 20, 20, 0xffffff, 128);
    CHECK((*view_at(page, 15, 15) & 0xff) > 0x90 && (*view_at(page, 15, 15) & 0xff) < 0xa0 &&
          *view_at(page, 30, 15) == 0xff102030u);
    rbox(page, -20, 300, 500, 60, 12, 0xff0000, 255, 0),
        rbox(page, 100, 100, 3, 3, 9, 0x00ff00, 255, 0);
    disc(page, 0, 0, 30, 0x0000ff, 200), disc(page, 374, 319, 500, 0x0000ff, 10);
    const float tri[] = { -10, 150, 400, 100, 400, 200 };
    poly(page, tri, 3, 0xffff00, 255);
    CHECK(*view_at(page, 300, 150) == 0xffffff00u);
    text_at(page, -30, -5, "Clipped at the corner", 26, 0, 0xffffff, 255),
        text_at(page, 350, 310, "and at the other", 26, 3, 0xffffff, 255);
    text_outlined(page, 370, 0, "\xe6\x97\xa5\xe6\x9c\xac", 26);
    /* a box's corners are round: its own corner pixel is left, its middle is filled */
    memset(page, 0, PAGE_PX * sizeof *page);
    rbox(page, 100, 100, 80, 40, 12, 0xffffff, 255, 0);
    CHECK(*view_at(page, 100, 100) == 0 && *view_at(page, 179, 139) == 0 &&
          *view_at(page, 140, 120) == 0xffffffffu);
    CHECK(*view_at(page, 112, 100) == 0xffffffffu && *view_at(page, 100, 112) == 0xffffffffu &&
          *view_at(page, 99, 120) == 0);
    pix rows[40];
    for (int i = 0; i < 40; ++i) rows[i] = (pix)i;
    rbox(page, 200, 100, 80, 40, 12, 0, 255, rows); /* a colour a row */
    CHECK(*view_at(page, 240, 100) == 0xff000000u && *view_at(page, 240, 117) == 0xff000011u &&
          *view_at(page, 201, 139) == 0);
    free(page);
}

/* ---- The picture's place, and the ladder --------------------------------------------------- */

/* A picture of w x h whose corners are red, green, blue and white (clockwise from the top left),
 * shown turned: the colours at the view's corners of where it lies. */
static void corners(int w, int h, int rot, pix *tl, pix *tr, pix *br, pix *bl) {
    geom_t g;
    geometry(w, h, 1, 1, rot, 0, &g);
    pix *img = calloc((size_t)(g.dw * g.dh), sizeof *img), *page = malloc(PAGE_PX * sizeof *page);
    for (int i = 0; i < g.dw * g.dh; ++i) img[i] = 0xff808080u;
    img[0] = 0xffff0000u, img[g.dw - 1] = 0xff00ff00u, img[g.dw * g.dh - 1] = 0xff0000ffu,
    img[g.dw * (g.dh - 1)] = 0xffffffffu;
    memset(page, 0x55, PAGE_PX * sizeof *page);
    picture_place(page, img, g.dw, &g);
    *tl = *view_at(page, g.vx, g.vy), *tr = *view_at(page, g.vx + g.vw - 1, g.vy);
    *br = *view_at(page, g.vx + g.vw - 1, g.vy + g.vh - 1),
    *bl = *view_at(page, g.vx, g.vy + g.vh - 1);
    /* what is not picture is black, all of it */
    if (g.vx > 0)
        CHECK(*view_at(page, 0, 0) == 0xff000000u && *view_at(page, 374, 319) == 0xff000000u &&
              *view_at(page, g.vx - 1, 160) == 0xff000000u);
    if (g.vy > 0)
        CHECK(*view_at(page, 0, 0) == 0xff000000u && *view_at(page, 187, g.vy - 1) == 0xff000000u &&
              *view_at(page, 187, g.vy + g.vh) == 0xff000000u);
    free(img), free(page);
}

static void check_picture(void) {
    geom_t g;
    pix a, b, c, d;
    /* Fit: all of it, bars above and below a wide picture, beside a tall one */
    geometry(640, 360, 1, 1, 0, 0, &g);
    CHECK(g.vx == 0 && g.vy == 54 && g.vw == 375 && g.vh == 211 && g.dw == 375 && g.dh == 211);
    CHECK(g.sx == 0 && g.sy == 0 && g.sw == 640 && g.sh == 360 && g.rot == 0);
    geometry(480, 640, 1, 1, 0, 0, &g);
    CHECK(g.vy == 0 && g.vh == 320 && g.vw == 240 && g.vx == 67);
    geometry(375, 320, 0, 0, 0, 0, &g); /* the screen's own shape; no aspect said */
    CHECK(g.vx == 0 && g.vy == 0 && g.vw == 375 && g.vh == 320);
    /* pixels that aren't square: 720 x 576 at 16:15 is 4:3 */
    geometry(720, 576, 16, 15, 0, 0, &g);
    CHECK(g.vh == 281 && g.vw == 375 && g.vy == 19);
    geometry(720, 576, 64, 45, 0, 0, &g); /* and at 64:45 is 16:9 */
    CHECK(g.vw == 375 && g.vh == 211);
    /* turned: a phone's 640 x 360 held upright is 360 x 640 on the screen, scaled before its turn
     */
    geometry(640, 360, 1, 1, 90, 0, &g);
    CHECK(g.vw == 180 && g.vh == 320 && g.vx == 97 && g.vy == 0 && g.dw == 320 && g.dh == 180 &&
          g.rot == 90);
    geometry(640, 360, 1, 1, -90, 0, &g), CHECK(g.rot == 270 && g.vw == 180);
    geometry(640, 360, 1, 1, 180, 0, &g), CHECK(g.rot == 180 && g.vw == 375 && g.vh == 211);
    geometry(640, 360, 1, 1, 450, 0, &g), CHECK(g.rot == 90);
    geometry(640, 360, 1, 1, 91, 0, &g), CHECK(g.rot == 90); /* to the nearest quarter */
    /* Fill: the screen's shape cut from its middle, on even pixels */
    geometry(640, 360, 1, 1, 0, 1, &g);
    CHECK(g.vx == 0 && g.vy == 0 && g.vw == 375 && g.vh == 320 && g.sy == 0 && g.sh == 360);
    CHECK(g.sw == 422 && g.sx == 108 && g.sx + g.sw <= 640); /* 360 * 375 / 320 = 421.9 */
    geometry(480, 640, 1, 1, 0, 1, &g);
    CHECK(g.sx == 0 && g.sw == 480 && g.sh == 410 && g.sy == 114 && g.vw == 375 && g.vh == 320);
    geometry(640, 360, 1, 1, 90, 1, &g); /* turned: the cut is along its length */
    CHECK(g.sy == 0 && g.sh == 360 && g.sw == 306 && g.sx == 166 && g.dw == 320 && g.dh == 375);
    geometry(720, 576, 16, 15, 0, 1, &g);
    CHECK(g.sw == 632 && g.sh == 576 && g.sx == 44); /* 4:3 into 375:320: a little off its sides */
    geometry(0, 0, 1, 1, 0, 0, &g), CHECK(g.vw >= 1 && g.vh >= 1); /* nothing divides by zero */
    geometry(100000, 2, 1, 1, 0, 0, &g), CHECK(g.vw == 375 && g.vh == 1);
    geometry(100000, 2, 1, 1, 0, 1, &g), CHECK(g.sw >= 2 && g.sx + g.sw <= 100000);

    /* each turn puts the picture's corners where they belong */
    corners(640, 360, 0, &a, &b, &c, &d);
    CHECK(a == 0xffff0000u && b == 0xff00ff00u && c == 0xff0000ffu && d == 0xffffffffu);
    corners(640, 360, 90, &a, &b, &c, &d); /* clockwise: its top left goes to the top right */
    CHECK(b == 0xffff0000u && c == 0xff00ff00u && d == 0xff0000ffu && a == 0xffffffffu);
    corners(640, 360, 180, &a, &b, &c, &d);
    CHECK(c == 0xffff0000u && d == 0xff00ff00u && a == 0xff0000ffu && b == 0xffffffffu);
    corners(640, 360, 270, &a, &b, &c, &d);
    CHECK(d == 0xffff0000u && a == 0xff00ff00u && b == 0xff0000ffu && c == 0xffffffffu);
    corners(375, 320, 0, &a, &b, &c, &d), CHECK(a == 0xffff0000u && c == 0xff0000ffu);
    corners(320, 375, 270, &a, &b, &c, &d), CHECK(d == 0xffff0000u && b == 0xff0000ffu);

    CHECK(too_large(1920, 1080, 20000) && !too_large(1280, 720, 20000) &&
          !too_large(1920, 1080, 60000));

    /* falling behind: three late frames inside a second step down the quality, to the keyframe
     * skip at most; a keyframe ends that; two seconds on time step it up again */
    ladder_t l;
    ladder_init(&l, 1280, 720, 0);
    CHECK(l.level == 0 && ladder_frame(&l, 10, 1, 0) == 0 && ladder_frame(&l, 20, 1, 0) == 0);
    CHECK(ladder_frame(&l, 30, 1, 0) == 1 && ladder_frame(&l, 40, 0, 0) == 1);
    CHECK(ladder_frame(&l, 50, 1, 0) == 1 && ladder_frame(&l, 60, 1, 0) == 1 &&
          ladder_frame(&l, 70, 1, 0) == 2);
    CHECK(ladder_frame(&l, 80, 1, 0) == 2 && ladder_frame(&l, 90, 1, 0) == 2 &&
          ladder_frame(&l, 100, 1, 0) == 3);
    for (int i = 0; i < 9; ++i) CHECK(ladder_frame(&l, 110 + i, 1, 0) == 3); /* no further */
    CHECK(ladder_frame(&l, 200, 0, 1) == 2); /* a keyframe: decoding again */
    CHECK(ladder_frame(&l, 2100, 0, 0) == 2 &&
          ladder_frame(&l, 2200, 0, 0) == 1); /* two seconds on time */
    CHECK(ladder_frame(&l, 4100, 0, 0) == 1 && ladder_frame(&l, 4200, 0, 0) == 0 &&
          ladder_frame(&l, 9000, 0, 0) == 0);
    /* late frames far apart are no trend */
    CHECK(ladder_frame(&l, 10000, 1, 0) == 0 && ladder_frame(&l, 12000, 1, 0) == 0 &&
          ladder_frame(&l, 14000, 1, 0) == 0);
    /* eased off and late again within five seconds: it holds twice as long before the next try */
    ladder_init(&l, 1280, 720, 0);
    for (int i = 0; i < 3; ++i) ladder_frame(&l, i, 1, 0);
    CHECK(l.level == 1 && ladder_frame(&l, 2010, 0, 0) == 0 && l.hold == 2000);
    for (int i = 0; i < 3; ++i) ladder_frame(&l, 3000 + i, 1, 0);
    CHECK(l.level == 1 && l.hold == 4000 && ladder_frame(&l, 6000, 0, 0) == 1 &&
          ladder_frame(&l, 7010, 0, 0) == 0);
    /* above 720p the loop filter is skipped from the start, and stays skipped */
    ladder_init(&l, 1920, 1080, 0);
    CHECK(l.level == 1 && ladder_frame(&l, 5000, 0, 0) == 1 && ladder_frame(&l, 50000, 0, 0) == 1);
    ladder_init(&l, 1280, 720, 0), CHECK(l.level == 0);
}

/* ---- The sound's parts --------------------------------------------------------------------- */

static void check_sound(void) {
    /* hciplayer's curve: 0.002 a step to 50, then 0.018 a step to 1 at 100 */
    CHECK(bt_gain(0) == 0 && bt_gain(1) == 131 && bt_gain(49) == 6422 && bt_gain(50) == 6553 &&
          bt_gain(51) == 7733);
    CHECK(bt_gain(100) == 65536 && bt_gain(75) == 36044 && bt_gain(-5) == 0 &&
          bt_gain(400) == 65536);
    for (int v = 1; v <= 100; ++v) CHECK(bt_gain(v) > bt_gain(v - 1));
    CHECK(arg_gain("37") == bt_gain(37) && arg_gain("100") == 65536 && arg_gain("0") == 0);
    CHECK(arg_gain("h41") == 65536 && arg_gain(0) == 65536 &&
          arg_gain("") == 65536); /* the DAC's own; the jack */
    CHECK(arg_level("37") == 37 && arg_level("h41") == 41 && arg_level("250") == 100);
    CHECK(arg_level(0) == -1 && arg_level("") == -1 && arg_level("h") == -1);

    short s[6] = { 32767, -32768, 1000, -1000, 0, 7 };
    scale(s, 6, 32768);
    CHECK(s[0] == 16383 && s[1] == -16384 && s[2] == 500 && s[3] == -500 && s[4] == 0 && s[5] == 3);
    short full[2] = { 32767, -32768 };
    scale(full, 2, 65536), CHECK(full[0] == 32767 && full[1] == -32768);
    scale(full, 2, 0), CHECK(!full[0] && !full[1]);
    short st[4] = { 1000, 3000, -32768, -32768 };
    mono(st, 2), CHECK(st[0] == 2000 && st[1] == 2000 && st[2] == -32768 && st[3] == -32768);
    short lr[4] = { 1, 2, 3, 4 };
    swap_lr(lr, 2), CHECK(lr[0] == 2 && lr[1] == 1 && lr[2] == 4 && lr[3] == 3);

    char dev[16];
    SAME(usb_device(
             " 0 [Shanling       ]: x2000 - Shanling\n 2 [DAC            ]: USB-Audio - USB DAC\n",
             dev),
         "plughw:2,0");
    SAME(usb_device(" 0 [Shanling       ]: x2000 - Shanling\n", dev), "plughw:1,0");
    SAME(usb_device("", dev), "plughw:1,0"), SAME(usb_device(0, dev), "plughw:1,0");
    SAME(usb_device("12 [Audio          ]: USB-Audio - Something\n", dev), "plughw:12,0");
}

/* ---- The controls' arithmetic -------------------------------------------------------------- */

static void check_controls(void) {
    /* the wheel: 28 units a tick, clockwise (its travel falling) up; what is short of one is kept
     */
    int acc = 0;
    CHECK(wheel_ticks(&acc, -27) == 0 && wheel_ticks(&acc, -1) == 1 && acc == 0);
    CHECK(wheel_ticks(&acc, -70) == 2 && acc == 14 && wheel_ticks(&acc, -14) == 1 && acc == 0);
    CHECK(wheel_ticks(&acc, 28) == -1 && wheel_ticks(&acc, 10) == 0 &&
          wheel_ticks(&acc, 18) == -1 && acc == 0);
    CHECK(wheel_ticks(&acc, -10) == 0 && wheel_ticks(&acc, 20) == 0 &&
          acc == -10); /* there and back: no tick */
    acc = 0;
    CHECK(wheel_ticks(&acc, -200) == 7 && acc == 4); /* a turn is seven */

    /* seek mode: 5 s a tick, more for a fast turn once it is under way, 60 s at most */
    CHECK(seek_ramp(1000, 1) == 5 && seek_ramp(600, 5) == 5 && seek_ramp(300, 5) == 10 &&
          seek_ramp(100, 9) == 30);
    CHECK(seek_ramp(50, 20) == 60 && seek_ramp(10, 50) == 60 && seek_ramp(0, 50) == 60);
    CHECK(seek_ramp(50, 1) == 5 && seek_ramp(50, 2) == 10 &&
          seek_ramp(50, 3) == 15); /* a turn works up to it */
    CHECK(seek_ramp(100000, 99) == 5 && seek_ramp(-5, 3) == 15);

    /* a side button held: again every 250 ms after 500, in 30 s steps once held 2 s */
    CHECK(hold_repeats(0) == 0 && hold_repeats(499) == 0 && hold_repeats(500) == 1 &&
          hold_repeats(749) == 1);
    CHECK(hold_repeats(750) == 2 && hold_repeats(2000) == 7 && hold_repeats(3000) == 11);
    CHECK(hold_step(0) == 10 && hold_step(1999) == 10 && hold_step(2000) == 30 &&
          hold_step(60000) == 30);

    /* where a stop leaves the mark: the start within 30 s of it; watched in the last 30 s or 5% */
    mark_t k = { .pos_ms = 1 };
    mark_place(&k, 29999, 600000);
    CHECK(k.pos_ms == 0 && !k.flags && k.dur_ms == 600000 && !mark_unfinished(&k));
    mark_place(&k, 30000, 600000), CHECK(k.pos_ms == 30000 && !k.flags && mark_unfinished(&k));
    mark_place(&k, 569999, 600000);
    CHECK(k.pos_ms == 569999 && !k.flags); /* 30 s from the end is 570 s; 95% the same */
    mark_place(&k, 570000, 600000);
    CHECK(k.pos_ms == 0 && k.flags == MARK_WATCHED && !mark_unfinished(&k));
    mark_place(&k, 100000, 600000);
    CHECK(k.pos_ms == 100000 && !k.flags); /* begun again: no longer watched */
    k.flags = MARK_WATCHED;
    mark_place(&k, 5000, 600000);
    CHECK(k.pos_ms == 0 && k.flags == MARK_WATCHED); /* hardly begun: still watched */
    /* a long video: 95% comes before the last 30 s */
    k = (mark_t){ 0 };
    mark_place(&k, 6839999, 7200000), CHECK(k.pos_ms == 6839999 && !k.flags);
    mark_place(&k, 6840000, 7200000), CHECK(k.pos_ms == 0 && k.flags == MARK_WATCHED);
    /* a short one: the last 30 s would be most of it, so its second half */
    k = (mark_t){ 0 };
    mark_place(&k, 19999, 40000), CHECK(k.pos_ms == 0 && !k.flags);
    mark_place(&k, 20000, 40000), CHECK(k.flags == MARK_WATCHED);
    k = (mark_t){ 0 };
    mark_place(&k, 59999, 120000), CHECK(k.pos_ms == 59999 && !k.flags);
    mark_place(&k, 90000, 120000), CHECK(k.pos_ms == 0 && k.flags == MARK_WATCHED);
    /* a length unknown: the place alone */
    k = (mark_t){ .dur_ms = 77 };
    mark_place(&k, 45000, 0), CHECK(k.pos_ms == 45000 && !k.flags && k.dur_ms == 77);

    char t[16];
    SAME(time_text(0, t, sizeof t), "0:00"), SAME(time_text(59999, t, sizeof t), "0:59");
    SAME(time_text(754000, t, sizeof t), "12:34");
    SAME(time_text(3723000, t, sizeof t), "1:02:03");
    SAME(time_text(-1270000, t, sizeof t), "-21:10");
    SAME(time_text(-3600000, t, sizeof t), "-1:00:00"), SAME(time_text(-999, t, sizeof t), "-0:00");
    SAME(time_text(360000000, t, sizeof t), "100:00:00");

    /* a tap: the dots at the top right, the bar's band, the middle; the bar's ends are its ends */
    CHECK(hit(330, 24) == HIT_MENU && hit(300, 50) == HIT_MENU && hit(290, 24) == HIT_NONE &&
          hit(330, 60) == HIT_NONE);
    CHECK(hit(187, 285) == HIT_BAR && hit(5, 310) == HIT_BAR && hit(187, 255) == HIT_MIDDLE);
    CHECK(hit(187, 160) == HIT_MIDDLE && hit(60, 160) == HIT_NONE && hit(320, 160) == HIT_NONE &&
          hit(187, 20) == HIT_NONE);
    CHECK(bar_ms(21, 600000) == 0 && bar_ms(0, 600000) == 0 && bar_ms(354, 600000) == 600000 &&
          bar_ms(374, 600000) == 600000);
    NEAR(bar_ms(187, 600000), 299099, 1), CHECK(bar_ms(187, 0) == 0);

    /* Q2 Pod's datagrams */
    event_t e;
    CHECK(event_parse("k d 171", &e) && e.kind == 'k' && e.what == 'd' && e.a == 171);
    CHECK(event_parse("k u 218", &e) && e.what == 'u' && e.a == 218);
    CHECK(event_parse("t m 374 0", &e) && e.kind == 't' && e.what == 'm' && e.a == 374 && e.b == 0);
    CHECK(event_parse("w -56", &e) && e.kind == 'w' && e.a == -56 && event_parse("w 28", &e) &&
          e.a == 28);
    CHECK(event_parse("v 41", &e) && e.kind == 'v' && e.a == 41 && !*e.arg);
    CHECK(event_parse("v 41 37", &e) && e.a == 41 && !strcmp(e.arg, "37") &&
          event_parse("v 41 h41", &e) && !strcmp(e.arg, "h41"));
    CHECK(!event_parse("", &e) && !event_parse("k x 171", &e) && !event_parse("t d 5", &e) &&
          !event_parse("q", &e) && !event_parse("w", &e));

    /* Q2_POPUP: a whole value, or the modern look on grey */
    pop_t p;
    pop_read(&p, "modern 1c1c1c 3a3a3a ffffff aaaaaa 2f7bf6 fefefe");
    CHECK(p.style == POP_MODERN && p.box == 0x1c1c1c && p.frame == 0x3a3a3a &&
          p.title == 0xffffff && p.text == 0xaaaaaa);
    CHECK(p.pick == 0x2f7bf6 && p.pick_text == 0xfefefe);
    pop_read(&p, "minimal 000000 4a4a4a ffffff 8c8c8c ffffff ffffff and more from a later Q2 Pod");
    CHECK(p.style == POP_MINIMAL && p.frame == 0x4a4a4a && p.text == 0x8c8c8c);
    char classic[1024] = "classic ffffff 5a595a 000000 000000 0081de ffffff";
    for (int i = 0; i < 64; ++i) sprintf(classic + strlen(classic), " %06x", 0x100000 + i);
    pop_read(&p, classic);
    CHECK(p.style == POP_CLASSIC && p.box == 0xffffff && p.pick == 0x0081de &&
          p.head[0] == 0x100000 && p.head[31] == 0x10001f);
    CHECK(p.band[0] == 0x100020 && p.band[31] == 0x10003f);
    const char *junk[] = { 0,
                           "",
                           "modern",
                           "modernist 000000 000000 000000 000000 000000 000000",
                           "modern 000000 000000 000000 000000 000000",
                           "modern 000000 000000 000000 000000 000000 00000g",
                           "modern 000000 000000 000000 000000 000000 0000000",
                           "classic ffffff 5a595a 000000 000000 0081de ffffff",
                           "sepia 000000 000000 000000 000000 000000 000000" };
    for (size_t i = 0; i < sizeof junk / sizeof *junk; ++i) {
        pop_read(&p, junk[i]);
        CHECK(p.style == POP_MODERN && p.box == 0x1c1c1c && p.pick == 0x5a5a5a &&
              p.pick_text == 0xffffff);
    }

    /* a menu: rows that can't be chosen are stepped over, the ends hold, and a long list scrolls */
    static menu_t m;
    m.title = "Options", m.n = 5, m.row[1].off = 1, m.row[4].off = 1;
    menu_move(&m, 1), CHECK(m.sel == 2);
    menu_move(&m, 1), CHECK(m.sel == 3);
    menu_move(&m, 1), CHECK(m.sel == 3); /* the last is off */
    menu_move(&m, -2), CHECK(m.sel == 0);
    menu_move(&m, -1), CHECK(m.sel == 0 && m.top == 0);
    menu_move(&m, 9), CHECK(m.sel == 3);
    pop_read(&p, 0);
    /* the modern box: its title's 38 rows, five rows, 8 below */
    int top = (320 - (38 + 5 * 32 + 8)) / 2;
    CHECK(menu_hit(&p, &m, 187, top + 38 + 5) == 0 &&
          menu_hit(&p, &m, 187, top + 38 + 2 * 32 + 31) == 2);
    CHECK(menu_hit(&p, &m, 187, top + 38 + 32 + 5) == -1 &&
          menu_hit(&p, &m, 187, top + 10) == -1); /* off; the title */
    CHECK(menu_hit(&p, &m, 40, 160) == -2 && menu_hit(&p, &m, 187, top - 1) == -2 &&
          menu_hit(&p, &m, 330, 160) == -2);
    memset(&m, 0, sizeof m);
    m.title = "Chapters", m.n = 12;
    menu_move(&m, 6), CHECK(m.sel == 6 && m.top == 0);
    menu_move(&m, 1), CHECK(m.sel == 7 && m.top == 1);
    menu_move(&m, 99), CHECK(m.sel == 11 && m.top == 5);
    menu_move(&m, -8), CHECK(m.sel == 3 && m.top == 3);
    top = (320 - (38 + 7 * 32 + 8)) / 2;
    CHECK(menu_hit(&p, &m, 187, top + 38 + 5) == 3 &&
          menu_hit(&p, &m, 187, top + 38 + 6 * 32 + 5) == 9);
}

/* ---- Whole runs ---------------------------------------------------------------------------- */

static char *said;    /* the last run's log */
static char dir[128]; /* and where its frames are */

/* The card as new, then each "clip" or "clip=name there" of the list put in its Videos folder. */
static void videos(const char *list) {
    char one[256], *as;
    sh("rm -rf " CARD "/Videos " CARD "/.videos && mkdir -p " CARD "/Videos");
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        snprintf(one, sizeof one, "%.*s", (int)n, p);
        p += n + (p[n] == ',');
        if ((as = strchr(one, '='))) *as++ = 0;
        sh("cp '%s/%s' '" CARD "/Videos/%s'", clips, one, as ? as : one);
    }
}

/* A file of the Videos folder, or of the app's state (".videos/settings"), with this text. */
static void put(const char *file, const char *text) {
    char path[PATH_N];
    sh("mkdir -p " CARD "/.videos");
    snprintf(path, sizeof path, CARD "/%s", file);
    CHECK(file_put(path, text, strlen(text)));
}

static long long size_of(const char *rel) {
    char path[PATH_N];
    struct stat st;
    snprintf(path, sizeof path, CARD "/Videos/%s", rel);
    CHECK(!stat(path, &st));
    return st.st_size;
}

/* A mark for rel, as the player would have left it. */
static void marked(const char *rel, long long pos, long long dur, unsigned flags, int audio,
                   int sub) {
    char text[512];
    snprintf(text, sizeof text, "%lld\t%lld\t%u\t7\t%d\t%d\t%lld\t%s\n", pos, dur, flags, audio,
             sub, size_of(rel), rel);
    put(".videos/marks", text);
}

/* The mark on the card for rel; one with audio -9 when there is none. */
static mark_t mark_of(const char *rel) {
    static marks_t m;
    mark_t none = { .audio = -9 }, *k;
    marks_load(&m);
    if ((k = marks_find(&m, rel, size_of(rel)))) none = *k, none.rel = 0;
    marks_free(&m);
    return none;
}

/* Starts playing rel by script; more is "NAME=value" lines for its environment. device: the
 * Q2's binary under the emulator, into a folder of its own. */
static pid_t begin(int device, const char *name, const char *rel, const char *script,
                   const char *more) {
    char path[PATH_N], env[2048];
    snprintf(dir, sizeof dir, OUT "/%s%s", name, device ? ".q2" : "");
    sh("rm -rf %s && mkdir -p %s", dir, dir);
    snprintf(path, sizeof path, "%s/script", dir);
    FILE *f = fopen(path, "w");
    CHECK(f);
    for (const char *p = script; *p; p += strcspn(p, "\n"), p += *p == '\n') {
        int ms, a, b; /* "key CODE": a short press, down and up; "tap X Y": a finger's */
        if (sscanf(p, "%d key %d", &ms, &a) == 2)
            fprintf(f, "%d k d %d\n%d k u %d\n", ms, a, ms, a);
        else if (sscanf(p, "%d tap %d %d", &ms, &a, &b) == 3)
            fprintf(f, "%d t d %d %d\n%d t u %d %d\n", ms, a, b, ms, a, b);
        else
            fprintf(f, "%.*s\n", (int)strcspn(p, "\n"), p);
    }
    CHECK(!fclose(f));
    fflush(0); /* what the checks have said so far is theirs to write, not the child's as well */
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid) return pid;
    setenv("VIDEOS_SCRIPT", path, 1), setenv("VIDEOS_OUT", dir, 1);
    setenv("Q2_URI", "play", 1), setenv("Q2_CONTEXT", rel, 1);
    unsetenv("Q2_POPUP"), unsetenv("VIDEOS_CLOCK"), unsetenv("VIDEOS_MEM_KB"),
        unsetenv("VIDEOS_SINK");
    snprintf(env, sizeof env, "%s", more ? more : "");
    for (char *v = strtok(env, "\n"); v; v = strtok(0, "\n")) {
        char *eq = strchr(v, '=');
        CHECK(eq);
        *eq = 0;
        setenv(v, eq + 1, 1);
    }
    alarm(120); /* a run that hangs fails */
    for (int fd = 3; fd < 64; ++fd)
        close(fd); /* a process of its own, as Q2 Pod starts it: none of the checks' files */
    if (device) {
        snprintf(env, sizeof env, "exec %s plughw:0,0", qemu);
        execl("/bin/sh", "sh", "-c", env, (char *)0);
        _exit(127);
    }
    /* ARG2 in more: the volume argument Q2 Pod gives on Bluetooth and USB */
    char *argv[] = { "q2-videos", "plughw:0,0", getenv("ARG2"), 0 };
    exit(player_main(argv[2] ? 3 : 2, argv)); /* exit, not _exit: what it leaked is told */
}

/* Waits for the run to end, well, and reads its log into said. */
static void finish(pid_t pid, const char *name) {
    char path[PATH_N];
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr, "%s: the run ended with status %#x\n", name, status), abort();
    snprintf(path, sizeof path, "%s/log", dir);
    free(said);
    if (!(said = file_get(path, 0))) fprintf(stderr, "%s: the run left no log\n", name), abort();
}
static void play(const char *name, const char *rel, const char *script, const char *more) {
    finish(begin(0, name, rel, script, more), name);
}

static int says(const char *what) { return strstr(said, what) != 0; }

/* The log's line for the frame name. */
static const char *at(const char *name) {
    static char line[2048];
    char key[80];
    snprintf(key, sizeof key, "shot=%s ", name);
    const char *p = strstr(said, key);
    if (!p) fprintf(stderr, "no frame %s in %s/log:\n%s", name, dir, said), abort();
    snprintf(line, sizeof line, "%.*s", (int)strcspn(p, "\n"), p);
    return line;
}
static int shot(const char *name) { /* 1 when the run wrote that frame */
    char key[80];
    snprintf(key, sizeof key, "shot=%s ", name);
    return says(key);
}
/* A number of that line: "pos", "pic", ... */
static long long num(const char *name, const char *key) {
    char k[40];
    snprintf(k, sizeof k, " %s=", key);
    const char *p = strstr(at(name), k);
    if (!p) fprintf(stderr, "no %s in: %s\n", key, at(name)), abort();
    return atoll(p + strlen(k));
}
/* Its subtitle lines, a | between two; its notice; where its picture lies. */
static const char *lines(const char *name) { return strstr(at(name), " text=") + 6; }
static const char *notice(const char *name) {
    static char n[128];
    const char *p = strstr(at(name), " notice=") + 8;
    snprintf(n, sizeof n, "%.*s", (int)strcspn(p, "|"), p);
    return n;
}
static const char *geom(const char *name) {
    static char g[40];
    const char *p = strstr(at(name), " geom=") + 6;
    snprintf(g, sizeof g, "%.*s", (int)strcspn(p, " "), p);
    return g;
}

/* A frame the run wrote: 375 x 320 pixels of three bytes after its header. */
static unsigned char *frame_in(const char *where, const char *name) {
    char path[PATH_N];
    size_t n;
    snprintf(path, sizeof path, "%s/%s.ppm", where, name);
    unsigned char *f = (unsigned char *)file_get(path, &n);
    if (!f || n != 15 + VIEW_W * VIEW_H * 3 || memcmp(f, "P6\n375 320\n255\n", 15))
        fprintf(stderr, "%s is no frame\n", path), abort();
    return f;
}
static unsigned rgb(const char *name, int x, int y) { /* a pixel of it, 0xRRGGBB */
    unsigned char *f = frame_in(dir, name), *p = f + 15 + (y * VIEW_W + x) * 3;
    unsigned c = (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
    free(f);
    return c;
}
/* How many pixels of the box from (x0, y0) to (x1, y1), both in it, are exactly c. */
static int count(const char *name, int x0, int y0, int x1, int y1, unsigned c) {
    unsigned char *f = frame_in(dir, name);
    int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const unsigned char *p = f + 15 + (y * VIEW_W + x) * 3;
            n += ((unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]) == c;
        }
    free(f);
    return n;
}
/* How many pixels of the box are not black: what is drawn on a black screen or band. */
static int ink(const char *name, int x0, int y0, int x1, int y1) {
    return (x1 - x0 + 1) * (y1 - y0 + 1) - count(name, x0, y0, x1, y1, 0);
}
/* The test picture's colour about (x, y): 'r', 'g', 'y', 'b', 'm', 'c' for its bars, 'k' black,
 * 'w' white, '?' anything between. The most frequent of 25 points, as things move over the bars. */
static char colour(const char *name, int x, int y) {
    unsigned char *f = frame_in(dir, name);
    int n[128] = { 0 };
    char best = '?';
    for (int i = 0; i < 25; ++i) {
        int px = x + (i % 5 - 2) * 3, py = y + (i / 5 - 2) * 3;
        if (px < 0 || px >= VIEW_W || py < 0 || py >= VIEW_H) continue;
        const unsigned char *p = f + 15 + (py * VIEW_W + px) * 3;
        int mid = 0;
        for (int c = 0; c < 3; ++c) mid |= p[c] >= 70 && p[c] <= 185;
        ++n[(int)(mid ? '?' : "kbgcrmyw"[(p[0] > 185) << 2 | (p[1] > 185) << 1 | (p[2] > 185)])];
    }
    free(f);
    for (const char *c = "kbgcrmyw"; *c; ++c)
        if (n[(int)*c] > n[(int)best]) best = *c;
    return best;
}
/* How many pixels of two frames differ by more than a little; *worst, the most one does. */
static int differ(const char *a_dir, const char *a, const char *b_dir, const char *b, int *worst) {
    unsigned char *fa = frame_in(a_dir, a), *fb = frame_in(b_dir, b);
    int n = 0, most = 0;
    for (int i = 0; i < VIEW_W * VIEW_H; ++i) {
        int d = 0;
        for (int c = 0; c < 3; ++c) {
            int v = abs(fa[15 + 3 * i + c] - fb[15 + 3 * i + c]);
            if (v > d) d = v;
        }
        if (d > most) most = d;
        n += d > 6;
    }
    free(fa), free(fb);
    if (worst) *worst = most;
    return n;
}
static int moved(const char *a, const char *b) { return differ(dir, a, dir, b, 0); }

#define WHITE 0xffffffu
#define GREY 0x5a5a5au /* the bar's part not played */

/* Plays to the end, in step, and leaves the video watched; left early, the start is kept. */
static void run_plain(void) {
    videos("h264-aac.mp4");
    play("plain", "h264-aac.mp4", "0 shot start\n2000 shot two\n5000 shot five\n29000 shot late\n",
         0);
    CHECK(says("open h264-aac.mp4\n") &&
          says("ready dur=30024 video=1 audio=1 naudio=1 nsubs=0 nside=0 chapters=3 sub=-1\n"));
    CHECK(says("end h264-aac.mp4 over pos=300"));
    /* the place is the script's time, the picture the one whose time it is (25 a second), and
     * the sound has gone out all along: 44.1 kHz in, 48 out, 440 Hz crossing 880 times a second */
    CHECK(num("start", "pos") == 0 && num("start", "pic") == 0 &&
          num("start", "state") == ENG_READY);
    CHECK(num("two", "pos") == 2000 && num("two", "pic") == 2000 && num("five", "pos") == 5000 &&
          num("five", "pic") == 5000);
    CHECK(num("late", "pos") == 29000 && num("late", "pic") == 29000);
    NEAR(num("two", "frames"), 2 * RATE + RATE / 5, 2048);
    NEAR(num("five", "frames"), 5 * RATE + RATE / 5, 2048);
    NEAR(num("two", "tone"), 880, 15), NEAR(num("late", "tone"), 880, 15);
    CHECK(!num("two", "controls") && !num("two", "paused") && !num("two", "level") &&
          !num("two", "silent"));
    /* Fit: all of the 16:9 picture, black above and below; its bars as the test picture has them */
    SAME(geom("two"), "0,54,375,211");
    CHECK(count("two", 0, 0, 374, 53, 0) == 375 * 54 &&
          count("two", 0, 265, 374, 319, 0) == 375 * 55);
    CHECK(colour("two", 20, 200) == 'r' && colour("two", 80, 110) == 'g' &&
          colour("two", 140, 110) == 'y');
    CHECK(colour("two", 205, 110) == 'b' && colour("two", 265, 110) == 'm' &&
          colour("two", 350, 200) == 'c');
    CHECK(moved("two", "five") > 500);
    mark_t k = mark_of("h264-aac.mp4");
    CHECK(k.flags == MARK_WATCHED && k.pos_ms == 0 && k.dur_ms == 30024 && k.seq == 1 &&
          k.audio == -1 && k.sub == -1);
    /* the volume was asked for; nothing dims; the lock is let go with the process */
    char *asked = file_get(TMP "/q2-videos.volume", 0);
    SAME(asked, "?");
    free(asked);
    CHECK(access(TMP "/q2-videos.dim", 0));
    int lock = lock_take(0);
    CHECK(lock >= 0);
    close(lock);

    /* Return leaves it: under 30 s in the start is kept, and watched stays as it was */
    play("left", "h264-aac.mp4", "6000 key 170\n9000 shot never\n", 0);
    CHECK(says("end h264-aac.mp4 quit pos=6000") && !shot("never"));
    k = mark_of("h264-aac.mp4");
    CHECK(k.flags == MARK_WATCHED && k.pos_ms == 0 && k.seq == 2);
}

/* Starts where the mark says, and Return, a pause and TERM each leave the place on the card. */
static void run_resume(void) {
    videos("long.mp4");
    marked("long.mp4", 61000, 120022, 0, -1, -1);
    play("resume", "long.mp4", "0 shot first\n1000 shot one\n4000 key 170\n", 0);
    /* from the keyframe before the place, one every two seconds: its picture at once */
    NEAR(num("first", "pos"), 60000, 40), CHECK(num("first", "pic") == 60000);
    NEAR(num("one", "pos"), 61000, 40), NEAR(num("one", "pic"), 60980, 40);
    mark_t k = mark_of("long.mp4");
    NEAR(k.pos_ms, 64000, 40);
    CHECK(!k.flags && k.dur_ms == 120022 && k.seq == 8 && mark_unfinished(&k));

    /* a watched video starts over, whatever place it has */
    marked("long.mp4", 61000, 120022, MARK_WATCHED, -1, -1);
    play("rewatch", "long.mp4", "1000 shot one\n2000 key 170\n", 0);
    CHECK(num("one", "pos") == 1000 && mark_of("long.mp4").flags == MARK_WATCHED &&
          mark_of("long.mp4").pos_ms == 0);

    /* TERM, as Q2 Pod's held Return sends it: the place is on the card when the process ends */
    marked("long.mp4", 61000, 120022, 0, -1, -1);
    play("term", "long.mp4", "5000 k d 170\n5700 term\n9000 shot never\n", 0);
    CHECK(says("end long.mp4 term pos=65") && !shot("never"));
    NEAR(mark_of("long.mp4").pos_ms, 65700, 60);

    /* a pause writes the place at once: it is there though the process is killed after it */
    marked("long.mp4", 61000, 120022, 0, -1, -1);
    pid_t pid =
        begin(0, "pause-mark", "long.mp4", "900 key 171\n1300 shot paused\n", "VIDEOS_CLOCK=real");
    for (int i = 0; i < 500 && (access(OUT "/pause-mark/paused.ppm", 0) ||
                                mark_of("long.mp4").pos_ms == 61000);
         ++i)
        usleep(10000);
    CHECK(!kill(pid, SIGKILL) && waitpid(pid, 0, 0) == pid);
    /* from 60 s, the keyframe before the place, and under a second on by the real clock */
    CHECK(mark_of("long.mp4").pos_ms > 60000 && mark_of("long.mp4").pos_ms < 61000);

    /* played into its last 30 s and left: watched, and from the start the next time */
    marked("long.mp4", 95000, 120022, 0, -1, -1);
    play("to-watched", "long.mp4", "1000 shot one\n2000 key 170\n", 0);
    NEAR(num("one", "pos"), 95000, 60);
    k = mark_of("long.mp4");
    CHECK(k.flags == MARK_WATCHED && k.pos_ms == 0);
    /* while it plays the place is written every two minutes (the log says when) */
    play("periodic", "long.mp4",
         "110000 key 222\n110100 key 222\n110200 key 222\n110300 key 222\n110400 key 222\n"
         "121000 key 170\n",
         0);
    const char *save = strstr(said, "save pos=");
    CHECK(save && strstr(save + 1, "save pos=") &&
          !strstr(strstr(save + 1, "save pos=") + 1, "save pos="));
    NEAR(atoll(save + 9), 70000, 2500); /* the first at two minutes, 50 s back from 1:50 and on */
    NEAR(mark_of("long.mp4").pos_ms, atoll(save + 9) + 1000, 100);
}

/* Nothing the user does ends the run on a press: the release would reach the list under it. */
static void run_release(void) {
    videos("h264-aac.mp4,sidecar.srt=notes.mp4,long.mp4");
    /* Return: still playing while it is down */
    play("return", "h264-aac.mp4", "6000 k d 170\n6300 shot down\n6500 k u 170\n7000 shot never\n",
         0);
    CHECK(num("down", "pos") == 6300 && !num("down", "paused") && says("quit pos=6500") &&
          !shot("never"));
    /* a release whose press was the list's (the Centre that chose the video, a Return let go
     * late) is nobody's: nothing happens, in the video or on the message */
    play("stray", "h264-aac.mp4", "100 k u 218\n200 k u 170\n300 t u 100 100\n1000 shot on\n", 0);
    CHECK(!num("on", "seek") && !num("on", "controls") && says("end h264-aac.mp4 over"));
    play("stray-message", "notes.mp4",
         "500 k u 218\n600 t u 100 100\n900 shot still\n1000 key 170\n1100 shot never\n", 0);
    CHECK(num("still", "state") == ENG_FAILED && says("end notes.mp4 quit") && !shot("never"));
    /* the message: any key, or a touch, leaves it as it is let go */
    play("message-key", "notes.mp4",
         "1000 k d 218\n1400 shot down\n1500 k u 218\n1600 shot never\n", 0);
    CHECK(num("down", "state") == ENG_FAILED && says("end notes.mp4 quit") && !shot("never"));
    play("message-touch", "notes.mp4",
         "1000 t d 100 100\n1400 shot down\n1500 t u 100 100\n1600 shot never\n", 0);
    CHECK(num("down", "state") == ENG_FAILED && says("end notes.mp4 quit") && !shot("never"));
    /* a side button that skips past the end ends the video on its press: the run waits for it
     * to be let go, the last frame up; without a release, two seconds; while it repeats, on */
    play("past", "h264-aac.mp4",
         "25000 k d 223\n25500 shot waiting\n26000 k u 223\n26100 shot never\n", 0);
    CHECK(says("end h264-aac.mp4 over") && num("waiting", "held") == 1 << 4 && !shot("never"));
    play("past-lost", "h264-aac.mp4", "25000 k d 223\n26900 shot waiting\n27100 shot never\n", 0);
    CHECK(shot("waiting") && !shot("never"));
    play("past-held", "h264-aac.mp4",
         "25000 k d 223\n26900 k d 223\n28800 k d 223\n30500 shot waiting\n30700 k u 223\n"
         "30900 shot never\n",
         0);
    CHECK(shot("waiting") && !shot("never"));
    /* a video that ends by itself under a key held, or a finger */
    play("end-held", "h264-aac.mp4",
         "29900 k d 218\n30100 shot waiting\n30200 k u 218\n30300 shot never\n", 0);
    CHECK(says("end h264-aac.mp4 over") && num("waiting", "held") == 1 << 2 && !shot("never"));
    play("end-touch", "h264-aac.mp4",
         "29900 t d 100 100\n30100 shot waiting\n30200 t u 100 100\n30300 shot never\n", 0);
    CHECK(num("waiting", "held") == 1 << 5 && !shot("never"));
    /* Q2 Pod's own end: its Return is not waited for, another key a moment */
    play("term-return", "long.mp4", "5000 k d 170\n5700 term\n5750 shot never\n", 0);
    CHECK(says("end long.mp4 term") && !shot("never"));
    play("term-held", "long.mp4",
         "5000 k d 170\n5600 k d 171\n5700 term\n5900 shot waiting\n6100 shot never\n", 0);
    CHECK(num("waiting", "held") == 1 << 1 && !shot("never"));
    /* a key held from one video into the next is not a press in the next, nor its repeats */
    videos("mpeg4.avi=a.avi,vp9-opus.webm=b.webm");
    put(".videos/settings", "next=1\n");
    play("next-held", "a.avi",
         "4000 k d 223\n4300 k d 223\n4600 k d 223\n4900 k d 223\n5200 k u 223\n6000 shot b\n", 0);
    CHECK(says("end a.avi over pos=4000") && says("open b.webm") && num("b", "serial") == 0 &&
          !num("b", "badge"));
    NEAR(num("b", "pos"), 2000, 60);
}

/* Pause and resume; the panel dims a minute into a pause, and five in the player ends. */
static void run_pause(void) {
    videos("h264-aac.mp4");
    play("pause", "h264-aac.mp4",
         "1900 shot before\n2000 key 171\n2500 shot p1\n4000 shot p2\n4500 key 171\n"
         "5500 shot on\n6000 key 170\n",
         0);
    CHECK(num("p1", "paused") && num("p1", "controls") && !num("before", "paused"));
    NEAR(num("p1", "pos"), 2000, 30), NEAR(num("p1", "pic"), 2000, 45);
    /* nothing moves: the same place, the same frame, no more sound */
    CHECK(num("p2", "pos") == num("p1", "pos") && num("p2", "frames") == num("p1", "frames") &&
          !moved("p1", "p2"));
    CHECK(rgb("p1", 180, 160) == WHITE && rgb("p1", 195, 160) == WHITE &&
          rgb("p1", 187, 160) != WHITE); /* the two bars */
    CHECK(!num("on", "paused") && rgb("on", 180, 160) != WHITE);
    NEAR(num("on", "pos"), num("p1", "pos") + 1000, 30), NEAR(num("on", "tone"), 880, 15);
    CHECK(says("end h264-aac.mp4 quit pos=3"));

    play("idle", "h264-aac.mp4",
         "2000 key 171\n61500 shot lit\n62500 shot dim\n63000 w -28\n63500 shot lit2\n"
         "123500 shot dim2\n362500 shot there\n363500 shot never\n",
         0);
    CHECK(!num("lit", "dim") && num("dim", "dim") && !num("lit2", "dim") && num("dim2", "dim") &&
          num("there", "paused"));
    CHECK(says("end h264-aac.mp4 quit pos=2") && !shot("never") && access(TMP "/q2-videos.dim", 0));
}

/* The side buttons, both ways and past both ends, held; seek mode's wheel; a finger on the bar;
 * double taps. long.mp4 has a keyframe every two seconds. */
static void run_seek(void) {
    videos("long.mp4,h264-aac.mp4");
    play("buttons", "long.mp4",
         "1000 shot a\n2500 key 223\n2900 shot b\n3000 key 223\n3400 shot c\n5500 key 222\n"
         "5900 shot d\n7500 key 222\n7600 key 222\n8000 shot e\n9000 shot f\n9500 key 170\n",
         0);
    CHECK(num("a", "pos") == 1000 && !num("a", "serial") && !num("a", "badge"));
    /* ahead from 2.5 s, to the keyframe before 12.5 s; then from there to the one before 22.5 s */
    NEAR(num("b", "pos"), 12400, 60);
    CHECK(num("b", "serial") == 1 && num("b", "badge") == 10 && num("b", "controls"));
    NEAR(num("c", "pos"), 22400, 60);
    CHECK(num("c", "badge") == 20);             /* presses close together add up */
    NEAR(num("c", "pic"), num("c", "pos"), 45); /* and the picture is the place's */
    NEAR(num("d", "pos"), 14400, 60);
    CHECK(num("d", "badge") == -10); /* back from 24.5 s: the keyframe before 14.5 s */
    NEAR(num("e", "pos"), 400, 60), CHECK(num("e", "badge") == -20); /* past the start: the start */
    CHECK(!num("f", "badge") && num("f", "serial") == 5);
    NEAR(num("f", "tone"), 880, 15); /* and the sound is the place's too */
    CHECK(moved("a", "b") > 500 && moved("b", "c") > 500);

    /* held: again every quarter second after half a one */
    play("held", "long.mp4",
         "1000 k d 223\n1400 shot one\n2100 shot five\n2600 k u 223\n3000 shot after\n"
         "3500 key 170\n",
         0);
    CHECK(num("one", "serial") == 1 && num("one", "badge") == 10 && num("five", "serial") == 4 &&
          num("five", "badge") == 40);
    CHECK(num("after", "serial") == 6 && num("after", "pos") > 50000 &&
          num("after", "pos") < 62000);
    /* in 30 s steps once held two seconds; back, to the start */
    marked("long.mp4", 100000, 120022, 0, -1, -1);
    play("held-back", "long.mp4",
         "1000 k d 222\n2900 shot six\n3100 shot seven\n3300 k u 222\n4000 shot after\n"
         "4500 key 170\n",
         0);
    CHECK(num("six", "badge") == -70 && num("seven", "badge") == -100 &&
          num("after", "serial") == 9);
    NEAR(num("after", "pos"), 750, 60);

    /* past the end the video is over, as if it had played out */
    marked("long.mp4", 112000, 120022, 0, -1, -1);
    play("past-end", "long.mp4", "500 shot a\n1000 key 223\n1500 shot never\n", 0);
    NEAR(num("a", "pos"), 112500, 50);
    CHECK(says("end long.mp4 over") && !shot("never") && mark_of("long.mp4").flags == MARK_WATCHED);

    /* Centre: the wheel seeks, 5 s a tick, sent once it has rested; Centre again ends it */
    marked("long.mp4", 0, 120022, 0, -1, -1);
    play("wheel", "long.mp4",
         "1000 key 218\n1100 shot m0\n1200 w -28\n1300 shot m1\n1900 shot m2\n2000 w 28\n"
         "2100 w 28\n2200 shot m3\n2400 shot m4\n2500 key 218\n2600 shot m5\n2700 w -28\n"
         "2800 shot m6\n3000 key 218\n3100 w -2800\n3200 shot m7\n3600 shot m8\n7500 shot m9\n",
         0);
    CHECK(num("m0", "seek") && num("m0", "controls") && !num("m0", "serial"));
    NEAR(num("m1", "pos"), 6200, 45);
    CHECK(!num("m1", "serial")); /* the place chosen, not yet gone to */
    NEAR(num("m2", "pos"), 6500, 60), CHECK(num("m2", "serial") == 1 && num("m2", "seek"));
    CHECK(num("m3", "pos") == 0 &&
          num("m3", "serial") == 1); /* 5 s back, then a faster 10: the start */
    NEAR(num("m4", "pos"), 150, 50), CHECK(num("m4", "serial") == 2);
    CHECK(!num("m5", "seek") && num("m6", "serial") == 2 &&
          !num("m6", "seek"));         /* out of it, the wheel seeks no more */
    CHECK(num("m7", "pos") == 115022); /* never nearer the end than 5 s */
    NEAR(num("m8", "pos"), 114350, 50);
    CHECK(!num("m9", "seek") && says("end long.mp4 over"));
    /* the knob, on the bar where the place is; none out of seek mode */
    CHECK(count("m1", 28, 276, 48, 282, WHITE) > 10 && !count("m5", 0, 270, 374, 282, WHITE));
    /* Return leaves seek mode, going to the place chosen; the next ends the video */
    play("wheel-return", "long.mp4",
         "1000 key 218\n1100 w -56\n1150 key 170\n1500 shot a\n2000 key 170\n2500 shot never\n", 0);
    CHECK(!num("a", "seek") && num("a", "serial") == 1 && !shot("never"));
    NEAR(num("a", "pos"), 10350, 50);

    /* paused, a seek shows its place's picture, and nothing moves until it plays on from there */
    play("paused", "long.mp4",
         "1000 key 171\n1500 shot p0\n2000 key 223\n3000 shot p1\n4000 shot p2\n4100 key 171\n"
         "4600 shot on\n5000 key 170\n",
         0);
    CHECK(num("p0", "paused") && num("p1", "paused") && num("p1", "serial") == 1);
    CHECK(num("p1", "pic") == 10000 && num("p1", "pos") == 10000 && moved("p0", "p1") > 500);
    CHECK(!moved("p1", "p2") && num("p2", "pos") == num("p1", "pos"));
    NEAR(num("on", "pos"), 10500, 60);
    NEAR(num("on", "pic"), num("on", "pos"), 45);

    /* keyframes ten seconds apart (0, 10 and 20 s): ahead from 7 s, the one before 17 s is under
     * half the way on, so it is the one after; and with none after, the place itself */
    play("far-apart", "h264-aac.mp4",
         "7000 key 223\n7400 shot a\n8000 key 218\n8100 w -28\n8700 shot b\n9000 key 170\n"
         "9100 key 170\n",
         0);
    NEAR(num("a", "pos"), 20400, 60);
    NEAR(num("b", "pos"), 25024 + 450, 80); /* 5 s on, held back from the last 5 s: 25.024 s */
    NEAR(num("b", "pic"), num("b", "pos"), 45);
    CHECK(num("b", "serial") == 2);

    /* a finger on the bar takes the place along, and it is gone to as the finger lifts */
    play("drag", "long.mp4",
         "1000 tap 60 100\n1500 t d 187 285\n1600 shot d1\n1700 t m 250 290\n1800 shot d2\n"
         "1900 t u 250 290\n2300 shot d3\n3000 key 170\n",
         0);
    CHECK(num("d1", "pos") == 120022LL * 166 / 333 && num("d1", "seek") && !num("d1", "serial"));
    CHECK(num("d2", "pos") == 120022LL * 229 / 333 && num("d3", "serial") == 1);
    NEAR(num("d3", "pos"), 82400, 50);
    /* two taps at a side skip 10 s, and each tap after them at once another */
    play("taps", "long.mp4",
         "1000 tap 330 160\n1200 tap 330 160\n1300 shot t1\n1400 tap 330 160\n1500 shot t2\n"
         "3000 tap 40 160\n3200 tap 40 160\n3300 shot t3\n5000 tap 330 160\n5100 shot t4\n"
         "6000 key 170\n",
         0);
    CHECK(num("t1", "serial") == 1 && num("t1", "badge") == 10 && num("t2", "serial") == 2 &&
          num("t2", "badge") == 20);
    CHECK(num("t3", "serial") == 3 && num("t3", "badge") == -10 && num("t4", "serial") == 3 &&
          !num("t4", "paused"));
}

/* What a tap shows, and for how long; the bar, its chapters, the title. */
static void run_controls(void) {
    videos("h264-aac.mp4=A Rather Long Title for a Video That Will Not Fit.mp4");
    const char *rel = "A Rather Long Title for a Video That Will Not Fit.mp4";
    play("controls", rel,
         "1000 shot hidden\n1500 tap 187 160\n1600 shot shown\n4400 shot still\n"
         "4600 shot gone\n5000 tap 187 160\n5400 tap 187 160\n5500 shot paused\n"
         "5600 tap 187 160\n5700 shot on\n5800 tap 60 100\n5900 shot hid\n6200 tap 60 100\n"
         "6400 tap 330 24\n6500 shot menu\n6600 tap 20 160\n6700 shot closed\n7000 key 170\n",
         0);
    /* a tap on a bare picture shows them (and doesn't pause); three seconds on they go */
    CHECK(!num("hidden", "controls") && num("shown", "controls") && !num("shown", "paused") &&
          num("still", "controls"));
    CHECK(!num("gone", "controls"));
    /* the bar: played to where the place is, grey after, a gap where each chapter starts */
    CHECK(rgb("shown", 25, 285) == WHITE && rgb("shown", 300, 285) == GREY &&
          rgb("hidden", 300, 285) == 0);
    CHECK(rgb("shown", 21 + 333 * 10000 / 30024, 285) == 0 &&
          rgb("shown", 21 + 333 * 20000 / 30024, 285) == 0);
    CHECK(rgb("shown", 21 + 333 * 10000 / 30024 - 2, 285) == GREY);
    /* the times under it, the title cut to fit between the corners, the three dots */
    CHECK(ink("shown", 46, 296, 90, 310) > 60 && ink("shown", 285, 296, 329, 310) > 60);
    CHECK(ink("shown", 72, 12, 302, 36) > 600 && !ink("shown", 0, 0, 71, 43) &&
          !ink("hidden", 0, 0, 374, 43));
    CHECK(rgb("shown", 324, 24) == WHITE && rgb("shown", 331, 24) == WHITE &&
          rgb("shown", 338, 24) == WHITE && rgb("shown", 328, 24) != WHITE);
    /* with them up: the middle pauses and plays, elsewhere hides them, the dots open the options */
    CHECK(num("paused", "paused") && !num("on", "paused") && !num("hid", "controls"));
    CHECK(num("menu", "menu") == 1 &&
          !num("closed", "menu")); /* and a tap off the box closes them */
}

/* The wheel is the Q2's volume, by Q2 Pod's file, and Q2 Pod's word moves the bar. */
static void run_volume(void) {
    videos("h264-aac.mp4");
    play("volume", "h264-aac.mp4",
         "300 w -28\n400 shot v0\n500 v 41\n600 shot v1\n1000 w -56\n1100 shot v2\n1120 v 43\n"
         "1400 shot v3\n2700 shot v4\n3000 v 30\n3100 shot v5\n5000 w -280\n5050 v 35\n"
         "5100 shot v6\n5400 shot v7\n6000 v 35 20\n6100 shot v8\n6200 v 35 h35\n"
         "6300 shot v9\n7000 w 1120\n7100 shot v10\n7300 shot v11\n7500 key 170\n",
         0);
    CHECK(num("v0", "volume") == -1 &&
          num("v0", "told") == -1); /* not known yet: the wheel asks nothing */
    CHECK(num("v1", "told") == 41 &&
          num("v1", "volume") == -1); /* told at the start: no bar for that */
    CHECK(num("v2", "volume") == 43 && num("v2", "told") == 41); /* two ticks up, shown at once */
    CHECK(num("v3", "volume") == 43 && num("v3", "told") == 43 &&
          num("v4", "volume") == -1); /* gone 1.5 s on */
    CHECK(num("v5", "volume") == 30); /* a headset's own keys */
    CHECK(num("v6", "volume") == 40 && num("v7", "volume") == 35 &&
          num("v7", "told") == 35); /* the Maximum volume's word */
    CHECK(num("v2", "gain") == 65536 && num("v8", "gain") == bt_gain(20) &&
          num("v9", "gain") == 65536);
    /* an asking never answered falls back to Q2 Pod's last word */
    CHECK(num("v10", "volume") == 0 && num("v11", "volume") == 35);
    /* the speaker, and the bar filled to the level */
    CHECK(rgb("v2", 48, 286) == WHITE && rgb("v2", 72 + 257 * 43 / 100 - 3, 285) == WHITE &&
          rgb("v2", 72 + 257 * 43 / 100 + 3, 285) == GREY);
    CHECK(rgb("v4", 48, 286) == 0 && rgb("v10", 80, 285) == GREY);
    char *asked = file_get(TMP "/q2-videos.volume", 0);
    SAME(asked, "0");
    free(asked);

    /* the start asks where the volume stands; until Q2 Pod answers, the level is the volume
     * argument's */
    play("ask", "h264-aac.mp4", "300 key 170\n", 0);
    asked = file_get(TMP "/q2-videos.volume", 0);
    SAME(asked, "?");
    free(asked);
    play("argument", "h264-aac.mp4", "300 w -28\n400 shot a\n1000 key 170\n", "ARG2=37");
    CHECK(num("a", "volume") == 38 && num("a", "told") == -1 && num("a", "gain") == bt_gain(37));
}

/* The sound tracks: the default, another chosen in the options, and that one remembered. */
static void run_tracks(void) {
    videos("two-audio-subs.mkv");
    play("audio", "two-audio-subs.mkv",
         "1500 shot a\n2000 k d 171\n2800 k u 171\n2900 shot m1\n3000 key 218\n3050 shot m2\n"
         "3100 w -28\n3200 key 218\n3300 shot m3\n3400 key 170\n3450 shot m4\n6500 shot b\n"
         "7000 key 170\n",
         0);
    CHECK(says("naudio=2 nsubs=1") && !num("a", "audio"));
    NEAR(num("a", "tone"), 880, 15); /* English: 440 Hz */
    CHECK(num("m1", "menu") == 1 && !num("m1", "sel") &&
          !num("m1", "paused")); /* Options, on Audio; it plays on */
    CHECK(num("m2", "menu") == 2 && !num("m2", "sel") && num("m3", "menu") == 1 &&
          num("m3", "audio") == 1 && !num("m4", "menu"));
    NEAR(num("b", "tone"), 1760, 30);
    NEAR(num("b", "pos"), 6500, 10); /* 日本語: 880 Hz, mono at 44.1 kHz; the place as it was */
    CHECK(num("b", "frames") > num("m4", "frames") + RATE);
    CHECK(mark_of("two-audio-subs.mkv").audio == 1);
    play("audio-again", "two-audio-subs.mkv", "1500 shot a\n2000 key 170\n", 0);
    CHECK(num("a", "audio") == 1);
    NEAR(num("a", "tone"), 1760, 30);
}

/* Subtitles: the file's own of each kind, files beside the video, switched and remembered. */
static void run_subs(void) {
    videos("two-audio-subs.mkv,ass.mkv,movtext.mp4,webvtt.mkv,sidecar.mp4,sidecar.srt,sidecar-bom."
           "srt=sidecar.ja.srt");
    /* on by default: the first that can be shown; then Off in the options, and on again */
    play("srt", "two-audio-subs.mkv",
         "1500 shot a\n1600 k d 171\n2400 k u 171\n2450 w -28\n2500 key 218\n2550 shot list\n"
         "2600 w 28\n2650 key 218\n2700 key 170\n2800 shot off\n3000 k d 171\n3800 k u 171\n"
         "3850 w -28\n3900 key 218\n3950 w -28\n4000 key 218\n4050 key 170\n7500 shot on\n"
         "9500 shot over\n",
         0);
    CHECK(says("nsubs=1 nside=0 chapters=0 sub=0") && !num("a", "sub"));
    SAME(lines("a"), "Hello");
    CHECK(ink("a", 100, 266, 274, 305) > 100); /* drawn, under the picture */
    CHECK(num("list", "menu") == 3 &&
          num("list", "sel") == 1); /* Off, English: on the one that is on */
    CHECK(num("off", "sub") == -1 && !num("off", "menu") && !ink("off", 0, 266, 374, 319));
    SAME(lines("off"), "");
    CHECK(!num("on", "sub"));
    SAME(lines("on"), "こんにちは|Two lines"), SAME(lines("over"), "");
    CHECK(mark_of("two-audio-subs.mkv").sub == 1);
    /* the choice is the video's: Off stays off the next time, whatever the setting */
    play("srt-off", "two-audio-subs.mkv",
         "500 k d 171\n1300 k u 171\n1350 w -28\n1400 key 218\n1450 w 28\n1500 key 218\n"
         "1550 key 170\n2000 key 170\n",
         0);
    CHECK(mark_of("two-audio-subs.mkv").sub == 0);
    play("srt-stays-off", "two-audio-subs.mkv", "1500 shot a\n2000 key 170\n", 0);
    CHECK(num("a", "sub") == -1);
    /* the Subtitles setting Off: none by default, and still there to choose */
    put(".videos/settings", "subs=0\n");
    play("ass-setting-off", "ass.mkv", "1500 shot a\n2000 key 170\n", 0);
    CHECK(says("nsubs=1") && num("a", "sub") == -1);
    put(".videos/settings", "");

    /* ASS: its tags gone, \N a line's end, \h a space; MP4's own; WebVTT */
    play("ass", "ass.mkv", "1500 shot a\n4500 shot b\n7500 shot c\n", 0);
    SAME(lines("a"), "Italic and plain"), SAME(lines("b"), "First line|Second line");
    SAME(lines("c"), "Top, with a comma and a hard space");
    play("movtext", "movtext.mp4", "1500 shot a\n4500 shot b\n7500 shot c\n", 0);
    SAME(lines("a"), "Hello"), SAME(lines("b"), "Привет"), SAME(lines("c"), "こんにちは|Two lines");
    play("webvtt", "webvtt.mkv", "1500 shot a\n4500 shot b\n7500 shot c\n", 0);
    SAME(lines("a"), "Hello"), SAME(lines("b"), "Привет"), SAME(lines("c"), "こんにちは|Two lines");

    /* files beside the video: its own name's first, a Latin-1 line read as such; then the other,
     * chosen in the options (Audio's row is there and can't be chosen: one track) */
    play("sidecar", "sidecar.mp4",
         "1500 shot a\n4500 shot b\n5000 k d 171\n5800 k u 171\n5850 shot menu\n5900 key 218\n"
         "5950 shot list\n6000 w -28\n6050 key 218\n6100 key 170\n6200 shot c\n7500 shot d\n",
         0);
    CHECK(says("nsubs=0 nside=2 chapters=0 sub=0"));
    SAME(lines("a"), "First line|Second line"), SAME(lines("b"), "Привет, мир");
    CHECK(num("menu", "menu") == 1 && num("menu", "sel") == 1 && num("list", "menu") == 3 &&
          num("list", "sel") == 1);
    CHECK(num("c", "sub") == 1);
    SAME(lines("c"), ""), SAME(lines("d"), ""); /* the other file has nothing there */
    CHECK(mark_of("sidecar.mp4").sub == 2);
    play("sidecar-again", "sidecar.mp4", "1500 shot a\n4500 shot b\n", 0);
    SAME(lines("a"), "Café déjà vu"), SAME(lines("b"), "こんにちは"); /* a BOM, CRLF */
    marked("sidecar.mp4", 0, 10022, 0, -1, 1);
    play("sidecar-latin", "sidecar.mp4", "7500 shot a\n", 0);
    SAME(lines("a"),
         "Caf\xe9 d\xe9j\xe0 vu"); /* é and à as Latin-1's one byte each, drawn as such */

    /* the three sizes, and above the controls when they are up */
    int n[3];
    for (int i = 0; i < 3; ++i) {
        char set[32];
        snprintf(set, sizeof set, "subsize=%d\n", i);
        put(".videos/settings", set);
        marked("sidecar.mp4", 0, 10022, 0, -1, 1);
        play("size", "sidecar.mp4", "4500 shot a\n4600 tap 60 100\n4700 shot b\n5000 key 170\n",
             0);                                          /* a line of one row */
        n[i] = ink("a", 0, 266, 374, 319);                /* under the picture */
        CHECK(n[i] > 100 && !ink("b", 0, 266, 374, 279)); /* and none there with the band up */
    }
    CHECK(n[1] > n[0] * 5 / 4 && n[2] > n[1] * 5 / 4);
}

/* Q2 Pod's pop-up as it hands it over, in its three looks (assets/themes of Q2 Pod 1.9.0). */
#define POP_MODERN_ENV "Q2_POPUP=modern 1c1c1c 3a3a3a ffffff aaaaaa 2f7bf6 ffffff"
#define POP_MINIMAL_ENV "Q2_POPUP=minimal 000000 4a4a4a ffffff 8c8c8c ffffff ffffff"
#define POP_CLASSIC_ENV                                                                            \
    "Q2_POPUP=classic ffffff 5a595a 000000 000000 0081de ffffff ffffff fcfcfc f9f9f9 f6f6f6 "      \
    "f3f3f3 f0f0f0 ededed "                                                                        \
    "eaeaea e7e7e7 e4e3e4 e1e0e1 deddde dbd9db d8d6d8 d5d3d5 d2cfd2 cfcccf cbc8cb c7c4c7 c3c0c3 "  \
    "bfbcbf bbb8bb "                                                                               \
    "b7b4b7 b3b0b3 afacaf aba9ab a9a7a9 a6a5a6 a3a3a3 a1a1a1 9e9f9e 9c9e9c 94c7de 06a3e5 04a1e3 "  \
    "039fe1 019ce0 "                                                                               \
    "009ade 0098de 0095de 0093de 0090de 008ede 008bde 0089de 0087de 0085de 0083de 0081de 007fde "  \
    "007cdd 007adc "                                                                               \
    "0078db 0075da 0073d9 0070d7 006ed6 006cd6 006bd6 0069d6 0068d6 0067d6 0066d6 004184"

/* The options: chapters, Start Over, Fit to Screen; the keys, the wheel and a finger in them. */
static void run_menu(void) {
    settings_t set;
    videos("h264-aac.mp4");
    play("menu", "h264-aac.mp4",
         "1000 k d 171\n1800 k u 171\n1850 shot o1\n1900 w -28\n2000 key 218\n2050 shot c1\n"
         "2100 key 223\n2150 w -28\n2200 shot c2\n2250 key 170\n2300 shot o2\n2350 key 218\n"
         "2400 w -56\n2450 key 218\n2950 shot c3\n3000 k d 171\n3800 k u 171\n3850 w -56\n"
         "3900 key 218\n4400 shot s1\n5000 k d 171\n5800 k u 171\n5850 key 218\n5900 shot f1\n"
         "5950 key 170\n6200 shot f2\n7000 k d 171\n7800 k u 171\n7850 key 171\n7900 shot o3\n"
         "8000 k d 171\n8800 k u 171\n8900 shot shut\n9200 key 170\n",
         0);
    /* the rows that apply: Audio (one track: not to be chosen), Fit to Screen, Chapters, Start Over
     */
    CHECK(num("o1", "menu") == 1 && num("o1", "sel") == 1 && !num("o1", "paused"));
    CHECK(num("c1", "menu") == 4 && !num("c1", "sel")); /* the chapter playing */
    CHECK(num("c2", "sel") == 2);                       /* a side button, then the wheel */
    CHECK(num("o2", "menu") == 1 &&
          num("o2", "sel") == 2); /* Return: back on the row it came from */
    CHECK(!num("c3", "menu"));    /* to the chapter's very start, not the keyframe before */
    NEAR(num("c3", "pos"), 20500, 30), NEAR(num("c3", "pic"), 20480, 40);
    NEAR(num("s1", "pos"), 500, 40), CHECK(!num("s1", "menu")); /* Start Over */
    /* Fit to Screen: the menu stays, the setting is kept, the picture fills the screen */
    CHECK(num("f1", "menu") == 1 && num("f1", "fill") == 1 && num("f1", "sel") == 1);
    SAME(geom("f2"), "0,0,375,320");
    CHECK(count("f2", 0, 0, 374, 53, 0) < 375 * 54 / 4 && colour("f2", 20, 200) == 'g' &&
          colour("f2", 350, 200) == 'm'); /* its sides cut */
    settings_load(&set);
    CHECK(set.fill == 1);
    /* Play/Pause pressed in them does nothing; held, it closes them */
    CHECK(num("o3", "menu") == 1 && !num("o3", "paused") && !num("shut", "menu") &&
          !num("shut", "paused"));
    put(".videos/settings", "fill=1\n");
    play("fill", "h264-aac.mp4",
         "1000 shot a\n2000 key 171\n2500 shot p\n3000 k d 171\n3800 k u 171\n3850 key 218\n"
         "4000 shot fit\n4100 key 170\n",
         0);
    SAME(geom("a"), "0,0,375,320");
    SAME(geom("fit"), "0,54,375,211"); /* paused: the same picture again, in its other shape */
    CHECK(num("fit", "paused") && num("fit", "pic") == num("p", "pic") && !num("fit", "fill"));
    settings_load(&set);
    CHECK(set.fill == 0);

    /* a finger: a row is chosen, the box's own edge does nothing, outside it closes */
    play("menu-touch", "h264-aac.mp4",
         "1000 k d 171\n1800 k u 171\n1900 tap 187 191\n2000 shot list\n2100 tap 187 100\n"
         "2200 shot same\n2300 tap 187 175\n2800 shot there\n3000 k d 171\n3800 k u 171\n"
         "3900 tap 187 127\n4000 shot off\n4100 tap 10 10\n4200 shot shut\n4500 key 170\n",
         0);
    CHECK(num("list", "menu") == 4 && num("same", "menu") == 4 && !num("there", "menu"));
    NEAR(num("there", "pos"), 10500, 40);                   /* The Middle */
    CHECK(num("off", "menu") == 1 && !num("shut", "menu")); /* Audio's row can't be chosen */

    /* the box in each of Q2 Pod's looks: its colours where the look puts them */
    const char *script = "1000 k d 171\n1800 k u 171\n1900 shot box\n2000 key 170\n2100 key 170\n";
    play("modern", "h264-aac.mp4", script, POP_MODERN_ENV);
    CHECK(rgb("box", 60, 240) == 0x1c1c1c && rgb("box", 47, 160) == 0x3a3a3a &&
          rgb("box", 200, 146) == 0x2f7bf6 && rgb("box", 46, 160) != 0x3a3a3a);
    play("minimal", "h264-aac.mp4", script, POP_MINIMAL_ENV);
    CHECK(rgb("box", 60, 240) == 0 && rgb("box", 47, 160) == 0x4a4a4a &&
          rgb("box", 74, 159) == WHITE);
    play("classic", "h264-aac.mp4", script, POP_CLASSIC_ENV);
    CHECK(rgb("box", 60, 236) == WHITE && rgb("box", 46, 160) == 0x5a595a &&
          rgb("box", 60, 81) == 0xf9f9f9 && rgb("box", 52, 149) == 0x009ade);
    play("plain", "h264-aac.mp4", script,
         "Q2_POPUP=classic ffffff"); /* half a value: the modern box on grey */
    CHECK(rgb("box", 60, 240) == 0x1c1c1c && rgb("box", 200, 146) == 0x5a5a5a);
    /* under the box the picture is dimmed by half */
    CHECK((rgb("box", 20, 200) >> 16) > 0x78 && (rgb("box", 20, 200) >> 16) < 0x88 &&
          !(rgb("box", 20, 200) & 0xf0f0));

    /* a list longer than the screen scrolls with the selection */
    videos("chapters.mkv");
    play("long-list", "chapters.mkv",
         "500 k d 171\n1300 k u 171\n1350 w -84\n1400 key 218\n1450 shot top\n1500 w -280\n"
         "1550 shot bottom\n1600 key 218\n2100 shot there\n2200 key 170\n",
         0);
    CHECK(says("chapters=12") && num("top", "menu") == 4 && num("top", "sel") == 1 &&
          num("bottom", "sel") == 11);
    CHECK(moved("top", "bottom") > 2000);
    NEAR(num("there", "pos"), 11 * 800 + 500, 40);
}

/* Where the picture lies: turned, with pixels that aren't square, a still, a large one. */
static void run_shapes(void) {
    videos("rotated.mp4,anamorphic.mp4,cover.mp4,big-1080p.mp4,60fps.mp4");
    /* a phone's video held upright: shown upright, the test picture's bars side by side and its
     * clock at the top left */
    play("rotated", "rotated.mp4", "1000 shot a\n4000 shot b\n", 0);
    SAME(geom("a"), "97,0,180,320");
    CHECK(count("a", 0, 0, 96, 319, 0) == 97 * 320 && count("a", 277, 0, 374, 319, 0) == 98 * 320);
    CHECK(colour("a", 110, 200) == 'r' && colour("a", 265, 200) == 'c' &&
          colour("a", 110, 12) == 'k');
    CHECK(colour("a", 140, 120) == colour("a", 140, 220) &&
          colour("a", 140, 120) != colour("a", 235, 120)); /* bars run down */
    CHECK(num("b", "pic") == 4000 && moved("a", "b") > 500 && says("end rotated.mp4 over"));
    put(".videos/settings", "fill=1\n");
    play("rotated-fill", "rotated.mp4", "1000 shot a\n1500 key 170\n", 0);
    SAME(geom("a"), "0,0,375,320");
    CHECK(count("a", 0, 0, 374, 319, 0) < 2000 && colour("a", 30, 160) == 'r' &&
          colour("a", 345, 160) == 'c');
    put(".videos/settings", "");
    /* 720 x 576 with pixels 16:15 is 4:3 */
    play("anamorphic", "anamorphic.mp4", "1000 shot a\n1500 key 170\n", 0);
    SAME(geom("a"), "0,19,375,281");
    CHECK(count("a", 0, 0, 374, 18, 0) == 375 * 19 && colour("a", 20, 200) == 'r' &&
          colour("a", 350, 200) == 'c');
    /* a song with its cover: the still stays up while the sound plays, to its end */
    play("cover", "cover.mp4", "1000 shot a\n4000 shot b\n", 0);
    SAME(geom("a"), "27,0,320,320");
    CHECK(!num("a", "pic") && !num("b", "pic") && num("b", "pos") == 4000 && !moved("a", "b") &&
          says("end cover.mp4 over"));
    CHECK(colour("a", 60, 200) != 'k' && count("a", 0, 0, 26, 319, 0) == 27 * 320);
    /* above 720p the loop filter is left out from the start; 60 frames a second in step */
    play("large", "big-1080p.mp4", "1000 shot a\n4000 shot b\n", 0);
    CHECK(num("a", "level") == 1 && num("b", "pic") == 4000 && says("end big-1080p.mp4 over"));
    play("sixty", "60fps.mp4", "1010 shot a\n1020 shot b\n", 0);
    CHECK(!num("a", "level") && num("a", "pic") == 1000 && num("b", "pic") > 1000 &&
          num("b", "pic") <= 1020);
}

/* 1 when FFmpeg opens path with nothing held back: what a file could make the player follow. */
static int follows(const char *path) {
    AVFormatContext *ic = 0;
    av_log_set_level(AV_LOG_QUIET);
    int ok = !avformat_open_input(&ic, path, 0, 0);
    if (ok) avformat_close_input(&ic);
    return ok;
}

/* What can't be played says why, on a screen of its own, until a key or ten seconds. */
static void run_refused(void) {
    videos("big-1080p.mp4,av1.mkv,h264-aac.mp4,sidecar.srt=notes.mp4");
    play("too-large", "big-1080p.mp4", "500 shot a\n9900 shot b\n10600 shot never\n",
         "VIDEOS_MEM_KB=20000");
    CHECK(says(
        "fail 1920 × 1080 is too large for the Q2's memory. Convert it to 720p or smaller.\n"));
    CHECK(num("a", "state") == ENG_FAILED && shot("b") && !shot("never") &&
          says("end big-1080p.mp4 quit"));
    CHECK(ink("a", 24, 100, 350, 220) > 1000 &&
          ink("a", 0, 0, 374, 319) == ink("a", 24, 100, 350, 220)); /* said, within the screen */
    CHECK(mark_of("big-1080p.mp4").audio == -9 && !says("save "));  /* and no mark is made of it */
    play("no-decoder", "av1.mkv", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail The Q2 has no decoder for AV1 video.\n") && says("end av1.mkv quit"));
    play("text", "notes.mp4", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail It is not a video file the Q2 knows.\n"));
    play("missing", "gone.mp4", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail The file can't be read.\n") && num("a", "state") == ENG_FAILED);
    play("outside", "../.videos/marks", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail The file can't be read.\n"));
    /* a playlist or a script under a video's name is not followed: FFmpeg would play what each
     * of these names, and the player says they are no video */
    put("Videos/list.mp4", "ffconcat version 1.0\nfile h264-aac.mp4\n");
    put("Videos/stream.mp4", "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:31\n#EXTINF:30.0,"
                             "\nh264-aac.mp4\n#EXT-X-ENDLIST\n");
    CHECK(follows(CARD "/Videos/list.mp4") && follows(CARD "/Videos/stream.mp4"));
    play("list", "list.mp4", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail It is not a video file the Q2 knows.\n") && !says("ready"));
    play("stream", "stream.mp4", "500 shot a\n600 key 218\n", 0);
    CHECK(says("fail It is not a video file the Q2 knows.\n") && !says("ready"));
}

/* Play Next: the next in the folder when one plays out, and only then. */
static void run_next(void) {
    videos("mpeg4.avi=1 first.avi,vp9-opus.webm=2 second.webm,hevc.mp4=10 tenth.mp4");
    const char *script = "1000 shot a\n7000 shot b\n12000 shot c\n20000 shot never\n";
    play("off", "1 first.avi", script, 0);
    CHECK(says("end 1 first.avi over") && !says("open 2") && !shot("b"));
    put(".videos/settings", "next=1\n");
    play("on", "1 first.avi", script, 0);
    CHECK(says("end 1 first.avi over") && says("open 2 second.webm") &&
          says("end 2 second.webm over") && says("open 10 tenth.mp4"));
    CHECK(says("end 10 tenth.mp4 over") &&
          !shot("never")); /* by name, numbers as numbers; the last ends the run */
    NEAR(num("b", "pos"), 7000 - 5024, 80), NEAR(num("c", "pos"), 12000 - 5024 - 5021, 150);
    CHECK(mark_of("1 first.avi").flags == MARK_WATCHED &&
          mark_of("2 second.webm").flags == MARK_WATCHED);
    play("left", "1 first.avi", "1000 key 170\n7000 shot b\n", 0);
    CHECK(says("end 1 first.avi quit") && !says("open 2"));
    /* one that can't be played ends the run at its message */
    videos("mpeg4.avi=1 first.avi,av1.mkv=2 second.mkv,hevc.mp4=3 third.mp4");
    put(".videos/settings", "next=1\n");
    play("refused", "1 first.avi", "6000 shot b\n16000 shot never\n", 0);
    CHECK(says("open 2 second.mkv") && num("b", "state") == ENG_FAILED &&
          says("end 2 second.mkv quit") && !says("open 3") && !shot("never"));
}

/* The output fails: lost (a headset gone), stalled, or not there to begin with. */
static void run_faults(void) {
    videos("h264-aac.mp4");
    /* lost: paused, and said; Play tries the output again */
    play("lost", "h264-aac.mp4",
         "1000 shot a\n2000 sink lost\n2500 shot b\n3000 shot b2\n3200 sink back\n"
         "3500 key 171\n4500 shot c\n5000 key 170\n",
         0);
    CHECK(num("b", "lost") && num("b", "paused") && num("b", "pos") == num("b2", "pos"));
    SAME(notice("b"), "Sound output lost");
    CHECK(rgb("b", 131, 70) >> 16 < 0xa0 &&
          rgb("c", 131, 70) >> 16 > 0xc8); /* on the screen, over the yellow bar */
    CHECK(!num("c", "lost") && !num("c", "paused") && !num("c", "silent") &&
          num("c", "frames") > num("b", "frames") + RATE / 2);
    NEAR(num("c", "pos"), num("b", "pos") + 1000, 40), NEAR(num("c", "tone"), 880, 15);
    SAME(notice("c"), "");
    /* still gone at Play: on without sound, by the clock */
    play("lost-silent", "h264-aac.mp4",
         "2000 sink lost\n2500 shot b\n3500 key 171\n4500 shot c\n6000 shot d\n6500 key 170\n", 0);
    CHECK(num("c", "silent") && !num("c", "paused") && !num("c", "lost"));
    SAME(notice("c"), "Playing without sound");
    NEAR(num("c", "pos"), num("b", "pos") + 1000, 40);
    NEAR(num("d", "pos"), num("b", "pos") + 2500, 40);
    NEAR(num("d", "pic"), num("d", "pos"), 45);
    /* an output that takes no more: three seconds on it is left behind and the video plays on
     * silent, to its end; the run ends well though a thread never comes back */
    play("stall", "h264-aac.mp4", "2000 sink stall\n4500 shot held\n5500 shot c\n8000 shot d\n", 0);
    CHECK(!num("held", "silent") && num("c", "silent") && num("d", "silent") &&
          says("end h264-aac.mp4 over"));
    NEAR(num("d", "pos"), num("c", "pos") + 2500, 40), NEAR(num("d", "pic"), num("d", "pos"), 45);
    CHECK(mark_of("h264-aac.mp4").flags == MARK_WATCHED);
    /* no output at all: silent from the start, and said */
    play("gone", "h264-aac.mp4", "1000 shot a\n3000 shot b\n3500 key 170\n", "VIDEOS_SINK=gone");
    CHECK(num("a", "silent") && num("a", "pos") == 1000 && num("b", "pos") == 3000 &&
          num("b", "pic") == 3000);
    SAME(notice("a"), "Playing without sound");
}

/* A Q2 awake for 14 hours: its clock's milliseconds times the 48 frames in each is past 32 bits,
 * which is all the Q2's long has (so this one says most by the Q2's binary, under the emulator). */
static void run_awake(int device) {
    videos("h264-aac.mp4");
    finish(begin(device, "awake", "h264-aac.mp4", "1000 shot a\n3000 shot b\n3500 key 170\n",
                 "VIDEOS_CLOCK=50000000"),
           "awake");
    CHECK(num("a", "t") == 50001000 && num("a", "pos") == 1000 && num("b", "pos") == 3000 &&
          num("b", "pic") == 3000);
    NEAR(num("b", "frames"), 3000 * 48, RATE / 5), NEAR(num("b", "tone"), 880, 15);
}

/* Files that aren't simple: timestamps that start above nothing, start again or run out of
 * bits, frames at uneven times, no sound, no picture, a sound that ends early, a broken end. */
static void run_files(void) {
    videos("offset.ts,jump.ts,wrap.ts,vfr.mkv,no-audio.mp4,audio-only.mp4,short-sound.mkv,hevc.mp4,"
           "mpeg4.avi,vp9-opus.webm");
    static const char *const ts[] = { "offset.ts", "wrap.ts" };
    for (int i = 0; i < 2;
         ++i) { /* the place counts from the file's first timestamp, across the wrap */
        play("ts", ts[i],
             "1000 shot a\n2500 shot b\n3000 shot c\n4500 shot d\n4600 key 222\n4900 shot e\n", 0);
        CHECK(says("ready dur=5016 video=1 audio=1"));
        CHECK(num("a", "pos") == 1000 && num("b", "pos") == 2500 && num("c", "pos") == 3000 &&
              num("d", "pos") == 4500);
        NEAR(num("a", "pic"), 990, 40), NEAR(num("c", "pic"), 2990, 40);
        NEAR(num("d", "pic"), 4490, 40);
        NEAR(num("d", "frames"), 4500 * 48 + RATE / 5, 2048), NEAR(num("d", "tone"), 880, 15);
        CHECK(num("e", "serial") == 1 && num("e", "pos") < 600 &&
              says("over pos=5016")); /* and a seek goes by it too */
    }
    /* two recordings end to end, the second's timestamps starting again: one run of picture
     * and sound through both (the place waits at the length the file gave) */
    play("jump", "jump.ts", "4500 shot a\n5500 shot b\n6000 shot c\n9500 shot d\n", 0);
    NEAR(num("b", "pic"), 5490, 60), NEAR(num("c", "pic"), 5990, 60);
    NEAR(num("d", "pic"), 9490, 60);
    NEAR(num("d", "frames"), 9500 * 48 + RATE / 5, 4096), NEAR(num("d", "tone"), 880, 15);
    CHECK(moved("b", "c") > 500 && says("end jump.ts over") && !num("d", "level"));
    /* 25 frames in one second and 5 in the next: each is up for its own time */
    play("vfr", "vfr.mkv", "500 shot a\n1500 shot b\n1900 shot c\n2500 shot d\n3550 shot e\n", 0);
    CHECK(num("a", "pic") == 461 && num("b", "pic") == 1421 && num("c", "pic") == 1821 &&
          num("d", "pic") == 2461 && num("e", "pic") == 3421);
    /* no sound: by the clock alone; no picture: the title on a plain screen */
    play("no-audio", "no-audio.mp4", "1000 shot a\n4000 shot b\n", 0);
    CHECK(says("video=1 audio=0 naudio=0") && num("a", "pos") == 1000 && num("b", "pic") == 4000 &&
          !num("b", "frames") && !num("b", "silent"));
    CHECK(says("end no-audio.mp4 over pos=5000"));
    play("audio-only", "audio-only.mp4",
         "1000 shot a\n1100 tap 187 160\n1200 shot b\n4000 shot c\n", 0);
    CHECK(says("video=0 audio=1") && num("a", "pic") == -1 && num("c", "pos") == 4000 &&
          says("end audio-only.mp4 over"));
    NEAR(num("c", "tone"), 880, 15);
    CHECK(ink("a", 100, 148, 274, 172) > 100 &&
          !ink("a", 0, 0, 374, 140)); /* its name, in the middle */
    CHECK(num("b", "controls") && !ink("b", 100, 148, 274, 172) && ink("b", 72, 12, 302, 36) > 100);
    /* a sound that ends a second in: the picture goes on by the clock; and the megabytes read
     * ahead of a picture of noise stay under their limit while it plays and while it waits */
    play("short-sound", "short-sound.mkv",
         "500 shot a\n3000 key 171\n9000 shot p\n9500 key 171\n15000 shot b\n", 0);
    CHECK(num("a", "pos") == 499 || num("a", "pos") == 500);
    NEAR(num("b", "pos"), 15000 - 6500, 40), NEAR(num("b", "pic"), num("b", "pos"), 45);
    CHECK(says("end short-sound.mkv over pos=20021"));
    for (const char *p = said; (p = strstr(p, " queued=")); ++p)
        CHECK(atoll(p + 8) < (5 << 20) + (1 << 19));
    CHECK(num("p", "queued") > 4 << 20); /* and the limit is what stopped it */
    /* each decoder the Q2 has, and a sound of each kind through the resampler */
    static const char *const kind[] = { "hevc.mp4", "mpeg4.avi", "vp9-opus.webm" };
    for (int i = 0; i < 3; ++i) {
        play("kind", kind[i], "1000 shot a\n4000 shot b\n", 0);
        NEAR(num("a", "pos"), 1000, 10), NEAR(num("b", "pic"), 3990, 30);
        NEAR(num("b", "tone"), 880, 15);
        CHECK(colour("b", 20, 200) == 'r' && colour("b", 350, 200) == 'c' &&
              moved("a", "b") > 500 && says(" over pos=50"));
    }

    /* cut short: each plays what it has and ends there, as if it were its end */
    videos("h264-aac.mp4,two-audio-subs.mkv,mpeg4.avi,offset.ts");
    sh("cd " CARD "/Videos && head -c 1500000 h264-aac.mp4 >cut.mp4 && head -c 1200000 "
       "two-audio-subs.mkv >cut.mkv && "
       "head -c 600000 mpeg4.avi >cut.avi && head -c 400000 offset.ts >cut.ts && head -c 3000 "
       "h264-aac.mp4 >stub.mp4");
    static const char *const cut[] = { "cut.mp4", "cut.mkv", "cut.avi", "cut.ts" };
    for (int i = 0; i < 4; ++i) {
        play("cut", cut[i], "1000 shot a\n", 0);
        NEAR(num("a", "pos"), 1000, 10), NEAR(num("a", "pic"), 990, 30);
        CHECK(says(" over pos=") && !says(" over pos=0\n"));
    }
    play("stub", "stub.mp4", "500 shot a\n600 key 218\n", 0); /* its index never came */
    CHECK(num("a", "state") == ENG_FAILED);
}

/* Q2 Pod ends the run while the file is still being opened; and a run on the real clock. */
static void run_term(void) {
    videos("h264-aac.mp4,big-1080p.mp4");
    play("term-opening", "big-1080p.mp4", "0 term\n", 0);
    CHECK(says("end big-1080p.mp4 term"));
    for (int i = 0; i < 5; ++i) { /* wherever in the opening it lands */
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        play("term-real", "big-1080p.mp4", "0 term\n", "VIDEOS_CLOCK=real");
        clock_gettime(CLOCK_MONOTONIC, &b);
        CHECK(says("end big-1080p.mp4 term") &&
              (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < 2500);
    }
    play("real", "h264-aac.mp4", "0 shot loading\n400 shot a\n1200 shot b\n1500 key 170\n",
         "VIDEOS_CLOCK=real");
    /* a Loading screen until it is open (on a quick computer it may be open by the first frame),
     * then picture and sound in step with the real clock: 800 ms of both in 800 ms */
    if (num("loading", "state") == ENG_OPENING) CHECK(ink("loading", 24, 120, 350, 200) > 100);
    CHECK(num("a", "state") == ENG_READY && num("a", "pos") <= 400);
    NEAR(num("b", "pos") - num("a", "pos"), 800, 100);
    NEAR(num("b", "pic"), num("b", "pos"), 60);
    NEAR(num("b", "frames") - num("a", "frames"), 800 * 48, RATE / 10);
    NEAR(num("b", "tone"), 880, 15);
    CHECK(says("end h264-aac.mp4 quit pos=1"));
}

/* The lock that keeps the page's thumbnails off the Q2 while a video plays: waited for behind
 * the video, never before it, and held from then to the end. */
static void run_lock(void) {
    videos("h264-aac.mp4");
    int worker = lock_take(0); /* a thumbnail in the making */
    CHECK(worker >= 0);
    /* the video plays and ends all the same, the lock never had */
    play("lock-never", "h264-aac.mp4", "1000 shot a\n2000 key 170\n", 0);
    CHECK(num("a", "pos") == 1000 && num("a", "pic") == 1000 && says("end h264-aac.mp4 quit"));
    /* on the real clock: playing while the worker has it; the worker done, the player has it
     * (the worker's next ask is refused) until it ends */
    pid_t pid = begin(0, "lock", "h264-aac.mp4", "300 shot one\n1300 shot two\n1600 key 170\n",
                      "VIDEOS_CLOCK=real");
    for (int i = 0; i < 500 && access(OUT "/lock/one.ppm", 0); ++i) usleep(10000);
    int again = lock_take(0);
    CHECK(again >= 0); /* not the player's yet */
    close(again), close(worker);
    for (int i = 0; i < 500 && access(OUT "/lock/two.ppm", 0); ++i) usleep(10000);
    CHECK(lock_take(0) < 0);
    finish(pid, "lock");
    CHECK(num("one", "state") == ENG_READY && num("one", "pic") >= 0 &&
          num("two", "pos") > num("one", "pos") + 800);
    CHECK((worker = lock_take(0)) >= 0);
    close(worker);
}

/* The Q2's own binary, under the emulator against the Q2's FFmpeg, plays what the host build
 * plays and shows the same: each decoder, sound through the resampler, subtitles drawn. */
static void run_qemu(void) {
    static const char *const clip[] = { "h264-aac.mp4",  "hevc.mp4",           "mpeg4.avi",
                                        "vp9-opus.webm", "two-audio-subs.mkv", "sidecar.mp4",
                                        "rotated.mp4" };
    videos("h264-aac.mp4,hevc.mp4,mpeg4.avi,vp9-opus.webm,two-audio-subs.mkv,sidecar.mp4,sidecar."
           "srt,rotated.mp4");
    for (size_t i = 0; i < sizeof clip / sizeof *clip; ++i) {
        const char *script =
            "500 shot a\n1000 tap 187 160\n1500 shot b\n1600 k d 171\n2400 k u 171\n2500 shot c\n"
            "2600 key 170\n2700 key 170\n";
        char host[128], lines[3][2048];
        play("qemu", clip[i], script, POP_CLASSIC_ENV);
        snprintf(host, sizeof host, "%s", dir);
        for (int k = 0; k < 3; ++k)
            snprintf(lines[k], sizeof lines[k], "%s", at((const char *[]){ "a", "b", "c" }[k]));
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        finish(begin(1, "qemu", clip[i], script, POP_CLASSIC_ENV), clip[i]);
        clock_gettime(CLOCK_MONOTONIC, &b);
        for (int k = 0; k < 3; ++k) {
            const char *name = (const char *[]){ "a", "b", "c" }[k];
            int worst, n = differ(host, name, dir, name, &worst);
            /* what happened is the same to the line; the pixels to a rounding of the Q2's own
             * routines (its FFmpeg is built with them, the host's in plain C) */
            SAME(at(name), lines[k]);
            if (n > VIEW_W * VIEW_H / 100)
                fprintf(stderr, "%s %s: %d pixels differ, by up to %d\n", clip[i], name, n, worst),
                    abort();
            printf("qemu-check: %-20s %s  %5d pixels differ (most %3d)  %s\n", clip[i], name, n,
                   worst, strstr(lines[k], " text="));
        }
        printf("qemu-check: %-20s %.1f s under the emulator\n", clip[i],
               (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9);
    }
    run_awake(1);
}

int main(int argc, char **argv) {
    static char at_home[PATH_N], font[PATH_N + 80], cwd[PATH_N];
    if (argc > 2 && !strcmp(argv[1], "--qemu")) qemu = argv[2];
    clips = getenv("VIDEOS_CLIPS");
    if (!clips)
        snprintf(at_home, sizeof at_home, "%s/.cache/q2-videos/clips", getenv("HOME")),
            clips = at_home;
    if (access(clips, R_OK))
        return fprintf(stderr, "check_player: no clips in %s: run tools/clips.sh\n", clips), 1;
    if (!getenv("VIDEOS_FONT") && getcwd(cwd, sizeof cwd)) {
        snprintf(font, sizeof font, "%s/" FONT_ROOTFS, cwd);
        setenv("VIDEOS_FONT", font, 1);
    }
    if (!getenv("VIDEOS_FONT") || access(getenv("VIDEOS_FONT"), R_OK))
        return fprintf(stderr, "check_player: no font: make build/rootfs's, or "
                               "VIDEOS_FONT=path/to/default.ttf\n"),
               1;
    sh("rm -rf " CARD " " TMP " " OUT " && mkdir -p " CARD " " TMP " " OUT);
    setenv("VIDEOS_CARD", CARD, 1), setenv("VIDEOS_TMP", TMP, 1);
    paths_init();
    if (qemu) {
        run_qemu();
        puts("check_player: the Q2's binary shows what the host's does");
        return 0;
    }
    check_subs();
    check_draw();
    check_picture();
    check_sound();
    check_controls();
    run_plain();
    run_resume();
    run_release();
    run_pause();
    run_seek();
    run_controls();
    run_volume();
    run_tracks();
    run_subs();
    run_menu();
    run_shapes();
    run_refused();
    run_next();
    run_faults();
    run_awake(0);
    run_files();
    run_term();
    run_lock();
    puts("check_player: ok");
    return 0;
}
