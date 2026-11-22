/* The full-screen player's own declarations (videos.h has what it shares with the page's server).
 * Its parts: draw.c (the page it draws on, shapes, text), subs.c (subtitle text), picture.c (where
 * a picture goes on the screen), engine.c (demuxing, decoding, the clock), sound.c (what is done
 * to the sound on its way out), osd.c (what is drawn over the picture), player.c (the keys, the
 * wheel, touches and what they do) and a platform: the Q2's screen, input socket and ALSA
 * (platform_q2.c), or a script of timed events with frames written as files (platform_script.c). */
#ifndef PLAYER_H
#define PLAYER_H
#include "videos.h"
#include <stdint.h>

/* The screen: the Q2's panel is portrait, 320 x 375, and the player's view of it is the panel
 * turned, 375 x 320. Everything is drawn in the view's coordinates onto a page, which is the
 * panel's own layout, so a page goes to the framebuffer row by row. */
#define VIEW_W 375
#define VIEW_H 320
#define PAGE_W 320
#define PAGE_H 375
#define PAGE_PX (PAGE_W * PAGE_H)
typedef uint32_t pix; /* 0xffRRGGBB, the framebuffer's BGRA */
/* view (x, y) on a page: the view is the panel turned anticlockwise, so its x is the panel's y */
static inline pix *view_at(pix *page, int x, int y) { return page + x * PAGE_W + PAGE_W - 1 - y; }

/* ---- draw.c -------------------------------------------------------------------------------- */

/* The pixels the glass hides at each end of the view's row y: its corners are rounded 80 px. */
int inset(int y);
/* The first x that is clear of the corners, 4 px to spare, in every row from y0 to y1. */
int clear_x(int y0, int y1);

/* Shapes, clipped to the view. alpha is 0 to 255. */
void fill(pix *page, int x, int y, int w, int h, pix rgb, int alpha);
/* A box with corners rounded r, anti-aliased. rows, when given, is a colour for each of its h
 * rows in place of rgb (the Classic pop-up's strip and band). */
void rbox(pix *page, int x, int y, int w, int h, float r, pix rgb, int alpha, const pix *rows);
void disc(pix *page, float cx, float cy, float r, pix rgb, int alpha);
/* A convex polygon of n corners (x, y pairs), anti-aliased. */
void poly(pix *page, const float *xy, int n, pix rgb, int alpha);

/* Text, from the Q2's own font ($VIDEOS_FONT, else /release/assets/default/raw/fonts/default.ttf),
 * UTF-8; a byte that is no UTF-8 reads as Latin-1. px is the text's pixel height as Q2 Pod sizes
 * its text (stb_truetype's), track more px after each letter. Without the font nothing is drawn
 * and every width is 0. One thread's (the main one). */
int font_open(void);
unsigned utf8_next(const char **s); /* the code point at *s, which moves past it; 0 at the end */
int text_w(const char *s, int px, int track);
/* s with its top left at (x, y): the line is px high. */
void text_at(pix *page, int x, int y, const char *s, int px, int track, pix rgb, int alpha);
/* The letters' coverage, 0 to 255, into mask (w x h bytes) with s's top left at (x, y) of it:
 * where letters meet, the larger stays. */
void text_mask(unsigned char *mask, int w, int h, int x, int y, const char *s, int px);
/* A w x h picture of greys onto the page at (x, y), line bytes a row: each pixel covers the page
 * by cover of 255 and is white of 255 over black, white no more than cover. */
void mask_over(pix *page, int x, int y, int w, int h, int line, const unsigned char *cover,
               const unsigned char *white);
/* s into out (n bytes) as it fits in w px: whole, or cut at a letter with an ellipsis. */
const char *text_fit(const char *s, int px, int track, int w, char *out, size_t n);
/* s into at most max lines no wider than w: its own line ends are kept, a longer line breaks at a
 * space, or between any two CJK letters, or inside a word too long for a line; what is left after
 * the last line ends it with an ellipsis. Returns the lines used. */
#define LINE_N 256
int text_wrap(const char *s, int px, int w, char lines[][LINE_N], int max);

/* ---- subs.c -------------------------------------------------------------------------------- */

typedef struct {
    long long start, end; /* ms from the video's start */
    char *text;           /* UTF-8, its lines parted by \n */
} cue_t;
typedef struct {
    cue_t *v;
    int n, room;
} cues_t;
/* A SubRip file's text (n bytes) into c, in order of start: a BOM is taken, line ends may be
 * CRLF, and tags (<i>, {\an8}) are left out; the bytes are kept as they are (text_at draws one
 * that is no UTF-8 as Latin-1's letter). Returns the cues added. */
int srt_parse(const char *data, size_t n, cues_t *c);
void cues_free(cues_t *c);
/* The cues on show at ms, the earliest first, up to max of them; *next, when given, is the next
 * time after ms at which the answer changes (a start or an end), or -1. */
int cues_at(const cues_t *c, long long ms, const cue_t **out, int max, long long *next);
/* A subtitle decoder's text (libavcodec's ASS event: "ReadOrder,Layer,Style,Name,MarginL,MarginR,
 * MarginV,Effect,Text") into out as plain lines: override tags gone, \N a line end, \h a space. */
void ass_plain(const char *event, char *out, size_t n);
/* Plain text's tags left out in place: <i> and its like, {\...}. */
void tags_strip(char *text);
/* The subtitle files beside the video at path: "name.srt", then every "name.<lang>.srt" in order
 * of name, case aside; each one's path and what to call it ("English" for en or eng, any other
 * word itself with a capital, "" for none). Returns how many, at most max. */
typedef struct {
    char path[PATH_N], name[48];
} sidecar_t;
int sidecars(const char *path, sidecar_t *out, int max);
/* A language as a track's name: "eng" or "en" is "English"; an unknown code is itself. */
const char *lang_name(const char *code);

/* ---- picture.c ----------------------------------------------------------------------------- */

/* Where a picture goes. The source's w x h pixels, each sar wide for 1 high, are shown turned
 * rot degrees clockwise (0, 90, 180, 270: a phone's video). Fit shows all of it, bars beside or
 * above; Fill crops it to the screen's shape. */
typedef struct {
    int sx, sy, sw, sh; /* the part of the source that is shown */
    int dw, dh;         /* what it is scaled to, before the turn */
    int vx, vy, vw, vh; /* where it lies in the view */
    int rot;
} geom_t;
void geometry(int w, int h, int sar_num, int sar_den, int rot, int fill, geom_t *g);
/* A scaled picture (g->dw x g->dh pixels, line pixels a row) onto a page, turned and placed as g
 * says; the rest of the page black. */
void picture_place(pix *page, const pix *img, int line, const geom_t *g);

/* Falling behind: the level a picture is decoded at. 0 all of it, 1 the loop filter skipped, 2
 * frames nothing refers to skipped as well, 3 everything up to the next keyframe. Three late
 * frames with no second on time between them step it up, the next step no sooner than half a
 * second on; hold ms of frames on time step it down again, hold doubling (to a minute) when
 * easing off was followed by lateness within five seconds. */
typedef struct {
    int level, floor; /* floor: 1 for a picture above 720p */
    int late;
    long long stepped_at, ahead_since, eased_at, hold;
} ladder_t;
void ladder_init(ladder_t *l, int w, int h, long long now);
/* A frame came out of the decoder at now, late or not: the level for the next. */
int ladder_frame(ladder_t *l, long long now, int late, int keyframe);

/* ---- sound.c ------------------------------------------------------------------------------- */

#define RATE 48000 /* what goes out: 48 kHz stereo S16 */
/* hciplayer's soft volume for the Q2's volume v, 0 to 100, in 1/65536: Bluetooth's, and a USB
 * DAC's without a volume control. */
int bt_gain(int v);
/* The gain for Q2 Pod's volume argument: "NN" is bt_gain's, "hNN" (the DAC applies it), none or
 * anything else unity. */
int arg_gain(const char *arg);
/* The level that argument names, "NN" or "hNN": where the volume stands until Q2 Pod says; -1
 * without one. */
int arg_level(const char *arg);
void scale(short *s, unsigned n, int g); /* n samples times g / 65536 */
void mono(short *s, unsigned n);         /* n stereo frames, each channel their mean */
void swap_lr(short *s, unsigned n);      /* n stereo frames, left and right exchanged */
/* "usb", the USB DAC, as plughw:N,0: N from cards, /proc/asound/cards' text. out has 16 bytes. */
const char *usb_device(const char *cards, char *out);

/* Where the sound goes on the Q2: ALSA (sound_q2.c; elsewhere, and without a device, engine.c
 * counts the time instead). All are called from one thread, the engine's audio thread. */
#ifndef HOST
/* 1 when dev is open; patient: wait as long as the Q2's player may take to let go of it */
int alsa_open(const char *dev, int patient, const volatile int *quit);
long alsa_write(const short *s, long frames); /* all of them, waiting; -1 when it is lost */
long alsa_unheard(long long written); /* of the frames written since open or drop: not yet heard */
void alsa_drop(void);                 /* what it holds is let go; ready for more */
void alsa_close(void);
/* The headphone DAC muted, if this run unmuted it. Any thread's. */
void alsa_mute(void);
#endif

/* ---- engine.c ------------------------------------------------------------------------------ */

/* Milliseconds: the monotonic clock, or the script's own (clock_virtual), which moves only when
 * the main thread waits and every other thread has done what it can. */
long long now_ms(void);
void clock_virtual(int from); /* from ms, a second at least */
/* The script's wait, on its own clock: the engine's threads settle, then the clock moves to
 * until, in steps that let them work at each of their own times. Returns early, 0, when the
 * engine has something for the main thread (eng_poll). */
int clock_wait(long long until);
/* The read end of a pipe that has a byte when the engine has something for the main thread, or a
 * signal came (wake_main): the Q2's wait polls it beside its socket. */
int wake_fd(void);
void wake_main(void); /* safe in a signal handler */
void wake_drain(void);

#define TRACKS 16
#define CHAPTERS 100
typedef struct {
    char name[64];
    int index; /* the stream's */
    int ok;    /* 0: listed, not playable (subtitles that are pictures) */
} track_t;
typedef struct {      /* an opened video */
    long long dur_ms; /* 0 unknown; it grows when the file plays on past the length it gave */
    int video, sound; /* it has a picture, a sound */
    int w, h;
    int naudio, audio_cur; /* audio_cur: audio[]'s on, -1 none */
    track_t audio[TRACKS];
    int nsubs; /* the file's own subtitles; the player puts the subtitle files before them */
    track_t subs[TRACKS];
    int nchapters;
    struct {
        long long start_ms;
        char title[64];
    } chapter[CHAPTERS];
    char note[80]; /* something to say once playing ("No decoder for its sound"), or "" */
} media_t;

enum { ENG_OPENING, ENG_READY, ENG_FAILED };
/* To the keyframe before the place; ahead, to that one when it is at least half the way on from
 * where the video is, else to the one after, so a step ahead always moves on; to the very place,
 * what is before it decoded and dropped; ahead but never past the place: as ahead, with the very
 * place where that would take the keyframe after. */
enum { SEEK_BACK, SEEK_AHEAD, SEEK_EXACT, SEEK_UPTO };
typedef struct {
    int state;        /* ENG_ */
    int fresh;        /* a new picture is in *cur: draw it */
    int picture;      /* there has been one */
    long long pos_ms; /* the place: the clock's, or where a seek is going */
    long long pic_ms; /* the picture's own time */
    long long due_ms; /* the next picture is due in this long; -1 when none waits */
    int ended;        /* all of it was played */
    int lost;         /* the output failed: nothing plays until eng_output */
    int silent;       /* the output takes no sound: playing on without */
    int serial;       /* counts the seeks */
    geom_t geom;      /* where the picture lies */
    long long frames; /* sound frames sent out, silence aside */
    int tone;         /* their last tenth of a second's zero crossings, times ten: 2 x Hz */
    int level;        /* the ladder's */
    long long queued; /* bytes of packets read and waiting to be decoded */
    int spliced;      /* its timestamps have started again: seeks go by bytes */
} snap_t;

/* Starts playing the video at path from start_ms: its threads begin, and eng_poll says when it
 * is open (ENG_READY, eng_media filled in) or why it isn't (ENG_FAILED, eng_why). known_ms is the
 * length the video's mark has (0 none), for a file that proved longer than it says. device is
 * Q2 Pod's ("plughw:0,0", "plug:bluealsa", "usb"), 0 for none; audio the sound track wanted (the
 * nth, -1 the default); sub the file's own subtitles to decode from the first packet on (the nth,
 * -1 none); fill Fit to Screen. */
void eng_open(const char *path, long long start_ms, long long known_ms, const char *device,
              int gain, int audio, int sub, int fill);
const char *eng_why(void);
const media_t *eng_media(void);
/* What the main thread needs each pass, at once. cur is its picture: when one is due it is
 * exchanged for the new one (fresh). */
void eng_poll(snap_t *s, pix **cur);
void eng_seek(long long ms, int how);
void eng_pause(int on);
void eng_audio(int nth);   /* another sound track, from about the place playing */
void eng_subs(int stream); /* the file's own subtitles to decode, by stream; -1 none */
void eng_fill(int on);
void eng_gain(int gain);
/* After lost, or playing on silent: the device is tried once more, as patiently as at the start;
 * silent again (eng_poll says so) when it can't be had. */
void eng_output(void);
/* The file's own subtitles on show at ms (cues_at's answer; the texts are copied into buf). */
int eng_cues(long long ms, char buf[][LINE_N], int max, long long *next);
/* Ends it: every thread is told and waited for, a second at most. 0 when one did not come (it is
 * in the card's or ALSA's hands): nothing of the engine's may be used again, and the process
 * must end by _exit. After 1 another video can be opened. */
int eng_close(void);
/* The checks': mem_kb in place of MemAvailable (0: the real one); the output made to fail
 * ("stall": it takes no more, "lost": it refuses, "back": works again, "gone": it can't be
 * opened, to be said before eng_open); each picture made slow_ms slower to decode, on the real
 * clock (0: as it was). */
void eng_test(unsigned mem_kb, const char *sink, int slow_ms);

/* ---- osd.c --------------------------------------------------------------------------------- */

/* Q2 Pod's pop-up, as $Q2_POPUP gives it. */
#define POP_ROW 32
#define POP_W 281
enum { POP_MINIMAL = 1, POP_MODERN, POP_CLASSIC };
typedef struct {
    int style;
    pix box, frame, title, text, pick, pick_text, head[POP_ROW], band[POP_ROW];
} pop_t;
/* "<style> <box> <frame> <title> <text> <pick> <pick_text>" and Classic's 64 more, RRGGBB each;
 * anything short of a whole value (and no value) is the modern look with a neutral accent. */
void pop_read(pop_t *p, const char *s);

#define MENU_ROWS 104 /* Options' few, or a list: every chapter and Off */
#define MENU_SHOWN 7  /* rows on the screen at once */
typedef struct {
    char label[64], value[48];
    int off;   /* can't be chosen */
    int named; /* a name from the file, drawn as it is written: 1 the label, 2 the value */
} row_t;
typedef struct {
    const char *title;
    row_t row[MENU_ROWS];
    int n, sel, top; /* top: the first row shown */
} menu_t;
void menu_move(menu_t *m, int by); /* the selection, kept on the screen */
/* the row at a touch; -1 the box itself; -2 outside it */
int menu_hit(const pop_t *p, const menu_t *m, int x, int y);

typedef struct { /* what is drawn over the picture */
    const pop_t *pop;
    const char *title;
    long long pos_ms, dur_ms;
    int controls, knob;
    int paused; /* the pause's mark between the times */
    int disc;   /* and its disc in the middle, for the moment after pausing */
    int target; /* the place being chosen with the wheel, said in the middle */
    int nchapters;
    const long long *chapter; /* their starts, ms */
    int volume;               /* the volume band: the level, -1 not shown */
    int battery, charging;    /* the battery's charge, 0 to 100, -1 not known; and it charges */
    int badge;                /* a side button's seek so far, seconds: "+30 s", "+1:40"; 0 none */
    const char *notice;       /* a line to say, 0 none */
    const char *subs;         /* the subtitles on show, lines parted by \n; 0 none */
    int subsize;              /* Small, Medium or Large */
    const menu_t *menu;
    const char *message, *reason; /* a whole screen: "Loading…", or why it can't be played */
    int plain;                    /* no picture under it: a black page */
} osd_t;
/* Subtitles into lines, each no wider than the view less 12 px a side, and the size they are
 * drawn at: Small 16, Medium 20, Large 24, and smaller (18, 16, 14) for a cue that takes more
 * than three lines, until it does; at 14 as many as it takes (SUB_LINES at most). Returns the
 * lines. Nothing of a cue is left out. */
#define SUB_LINES 12 /* what the picture holds between the bands, at 14 px */
int subs_layout(const char *text, int subsize, char lines[][LINE_N], int *px);
/* 1 when o draws anything: else the picture goes to the screen as it is. */
int osd_any(const osd_t *o);
void osd_draw(pix *page, const osd_t *o);
/* "12:34", "1:02:03", and what is left, a time below zero: "-21:10". */
const char *time_text(long long ms, char *out, size_t n);
/* What a run of skips has moved: "+10 s", "-50 s", and from a minute up "+1:40", "-2:10". */
const char *skip_text(int secs, char *out, size_t n);

enum { HIT_NONE, HIT_MENU, HIT_BAR, HIT_MIDDLE };
int hit(int x, int y);                     /* what a tap at (x, y) is on, the controls shown */
long long bar_ms(int x, long long dur_ms); /* the place a touch at x on the bar means */

/* ---- player.c: the pure parts, for the checks ---------------------------------------------- */

#define KEY_RETURN 170
#define KEY_PLAY 171
#define KEY_CENTRE 218
#define KEY_BACK 222
#define KEY_FWD 223
#define WHEEL_TICK 28 /* the wheel's travel for one tick: a seventh of a turn */
#define HOLD_MS 700

/* The wheel's travel into ticks, clockwise positive: *acc keeps what is short of one. */
int wheel_ticks(int *acc, int travel);
/* Seconds a tick moves the place in seek mode: 5, times up to 12 for a fast run (gap ms since the
 * tick before, the run's nth tick). */
int seek_ramp(int gap_ms, int nth);
/* A side button held for held ms: how many steps it has made after the press's own. */
int hold_repeats(long long held_ms);
int hold_step(long long held_ms); /* a step's seconds: 10, and 30 once held two seconds */
/* The mark for a stop at pos of dur (0 unknown): watched in the last 30 s or 5% (a short video:
 * its second half), and then at the start again; the start too within 30 s of it. */
void mark_place(mark_t *k, long long pos_ms, long long dur_ms);
/* The fuel gauge's text as a charge: the first run of digits in its first 32 bytes, 0 to 100;
 * -1 for anything else. */
int battery_level(const char *text);
/* The power supply's status as charging: its first word is "Charging" or "Full". */
int battery_charging(const char *text);

/* ---- The platform -------------------------------------------------------------------------- */

typedef struct {
    char kind; /* 'k' a key (what 'd' or 'u', a its code), 't' a touch (what 'd', 'm' or 'u', at
                  a, b), 'w' the wheel (a its travel), 'v' the volume (a the level, arg Q2 Pod's
                  volume argument or ""); the script's: 's' a frame to write (arg its name), 'f'
                  a fault (arg) */
    char what;
    int a, b;
    char arg[40];
} event_t;
/* One of Q2 Pod's datagrams into e: 0 when it is none. */
int event_parse(const char *msg, event_t *e);
typedef struct {
    int (*open)(void);               /* the screen and the input: 0 when this is no Q2 */
    int (*wait)(int ms, event_t *e); /* 1 with an event; 0 when ms passed, or the engine woke it */
    void (*show)(const pix *page);
    void (*close)(void);
} plat_t;
extern const plat_t plat_script;
#ifndef HOST
extern const plat_t plat_q2;
#endif
/* The script's: page as a 375 x 320 PPM, $VIDEOS_OUT/name.ppm; a line added to $VIDEOS_OUT/log. */
void script_shot(const char *name, const pix *page);
void script_log(const char *line);

#endif
