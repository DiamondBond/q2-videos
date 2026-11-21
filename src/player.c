/* The full-screen player: Q2 Pod runs it over the whole screen for a video's row, as it runs a
 * card game that has the sound. This is its main thread: Q2 Pod's keys, wheel and touches and
 * what they do, the marks, and each frame put together (the engine's picture, then osd.c's
 * drawing over it) and shown.
 *
 * Q2 Pod's conventions: the wheel is the volume and Centre turns it to seeking, Play/Pause
 * pauses and held opens the options, the side buttons skip, Return goes back. Q2 Pod sends no
 * long presses, so holds are timed here from a key's down to its up; a key's up counts only
 * when its down was seen, as the Centre that chose the video is released after we start.
 *
 * Nothing the user does ends the run on a press: once this process is gone Q2 Pod gives the
 * input to what is under it, where the release of a press made here would act on its own
 * (Play/Pause's starts the music). So a key or a touch ends the run as it is let go, and a run
 * that ends by itself waits a moment for what is down (let_go). */
#include "player.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libavutil/log.h>

#define CONTROLS_MS 3000 /* the controls stay, and seek mode after the wheel's last tick */
#define VOLUME_MS 1500
#define BADGE_MS 800
#define DISC_MS 800    /* the pause's disc, in the middle of the picture */
#define CENTRE_GAP 250 /* a second Centre this soon is a double click's: not taken */
#define NOTICE_MS 4000
#define SEEK_SEND_MS 150  /* the wheel's seek is sent once it has rested this long */
#define VOLUME_SETTLE 150 /* Q2 Pod's word on the volume counts once ours is this old */
#define WHEEL_REST 300    /* a turn ends when the wheel has been still this long */
#define TAP_MS 300        /* a second tap within this is a double tap */
#define DIM_MS 60000      /* paused and untouched: the panel dims */
#define IDLE_MS 300000    /* and then the player ends, so the Q2 can sleep */
#define SAVE_MS 120000    /* the place is written this often while it plays */
#define ERROR_MS 10000    /* a video that can't be played says so this long */
#define END_KEEP 5000     /* the wheel, a finger and a held button go no nearer the end */
#define HOLD_MAX 30000    /* a side button down this long has lost its release */
#define LET_GO_MS 2000    /* at the end, what is down is waited for this long */
#define LET_GO_TERM 300   /* and this long when Q2 Pod ends the run */
/* ponytail: of more subtitle files beside a video than this, the first the folder lists */
#define SIDES 8

/* ---- The pure parts ------------------------------------------------------------------------ */

int wheel_ticks(int *acc, int travel) {
    *acc -= travel; /* the travel rises anticlockwise */
    int ticks = *acc / WHEEL_TICK;
    *acc -= ticks * WHEEL_TICK;
    return ticks;
}

int seek_ramp(int gap_ms, int nth) {
    int by = gap_ms > 0 ? 600 / gap_ms : 12; /* ticks 600 ms apart are slow: one step each */
    if (by > nth) by = nth;                  /* and a turn works up to its speed */
    return 5 * (by < 1 ? 1 : by > 12 ? 12 : by);
}

int hold_repeats(long long held_ms) { return held_ms < 500 ? 0 : 1 + (int)((held_ms - 500) / 250); }
int hold_step(long long held_ms) { return held_ms >= 2000 ? 30 : 10; }

void mark_place(mark_t *k, long long pos_ms, long long dur_ms) {
    /* watched from the last 30 s or 5% on, whichever begins first; a video too short for that
     * to leave any middle, from its half */
    long long edge = dur_ms - 30000 < dur_ms * 95 / 100 ? dur_ms - 30000 : dur_ms * 95 / 100;
    if (edge < dur_ms / 2) edge = dur_ms / 2;
    if (dur_ms > 0) k->dur_ms = dur_ms;
    if (dur_ms > 0 && pos_ms >= edge)
        k->flags |= MARK_WATCHED, k->pos_ms = 0;
    else if (pos_ms < 30000)
        k->pos_ms = 0; /* hardly begun: from the start, and watched stays as it was */
    else
        k->pos_ms = pos_ms, k->flags &= ~MARK_WATCHED;
}

int event_parse(const char *msg, event_t *e) {
    memset(e, 0, sizeof *e);
    if (sscanf(msg, "k %c %d", &e->what, &e->a) == 2 && (e->what == 'd' || e->what == 'u'))
        e->kind = 'k';
    else if (sscanf(msg, "t %c %d %d", &e->what, &e->a, &e->b) == 3 && strchr("dmu", e->what))
        e->kind = 't';
    else if (sscanf(msg, "w %d", &e->a) == 1)
        e->kind = 'w';
    else if (sscanf(msg, "v %d %39s", &e->a, e->arg) >= 1)
        e->kind = 'v';
    return e->kind != 0;
}

/* ---- The run, and the video playing -------------------------------------------------------- */

enum { MENU_NONE, MENU_OPTIONS, MENU_AUDIO, MENU_SUBS, MENU_CHAPTERS };
enum { ROW_AUDIO, ROW_SUBS, ROW_FIT, ROW_CHAPTERS, ROW_START }; /* what a row of Options does */
enum { K_RETURN, K_PLAY, K_CENTRE, K_BACK, K_FWD, KEYS, FINGER = KEYS };
enum { END_NOT, END_OVER, END_QUIT }; /* how a video ends: played out, or left */

static struct { /* the run's */
    const plat_t *plat;
    settings_t set;
    pop_t pop;
    char root[PATH_N], volume_path[PATH_N], dim_path[PATH_N];
    const char *device;
    int gain;
    int volume, told;   /* the Q2's volume as shown, and as Q2 Pod last said it; -1 not yet */
    long long asked_at; /* when a level was last asked for; 0 once Q2 Pod's word is taken */
    int dim, stuck;
    unsigned held;   /* what went down here and has not come up: a bit a K_, and FINGER */
    pix *cur, *page; /* the picture on show, and the frame drawn over a copy of it */
    const pix *out;  /* which of them the screen has */
} P;

static struct { /* the video's: zeroed for each */
    char rel[REL_MAX + 1], path[PATH_N], title[256];
    long long size, start_ms;
    mark_t *mark;
    const media_t *m;
    int state, end;
    const char *why;
    snap_t s;
    long long at_ms; /* the place: the engine's, or where the last seek went */
    sidecar_t side[SIDES];
    int nside;
    cues_t cues;   /* the subtitle file on show */
    int want, sub; /* subtitles: wanted at the start, and on now: -1 none, else their place in
                      the list (the files beside the video, then the file's own) */
    int own;       /* the subtitles are the video's default, not the user's choice */
    int audio;     /* the sound track on, the nth */
    char subtext[3 * LINE_N];
    long long sub_next;
    int paused, lost, silent;
    int resume;          /* the options paused it: it plays on when they close */
    int hidden;          /* paused, and the bands tapped away */
    int autoplay, began; /* it follows the one before by itself; its first picture is up */

    /* what is on the screen, and until when (0: not) */
    long long controls_until, seek_until, volume_until, badge_until, notice_until, error_until;
    long long disc_until, target_until;
    int badge, rev; /* rev: counts changes to what the screen says */
    char notice[80];
    int level, act[8]; /* the menu: MENU_, and what each row of Options does */
    menu_t menu;

    /* input */
    long long down[KEYS]; /* when each key went down; 0: up, or its press is spent */
    int repeats;          /* a held side button's steps so far */
    int wheel_acc, run;   /* the wheel: short of a tick, and the ticks of this turn */
    long long wheel_at, tick_at, centre_at;
    long long pend_ms, pend_at; /* a place being chosen (-1 none), sent at pend_at */
    int touching, drag, moved, tx, ty, tap_third;
    long long touch_at, tap_at;
    long long input_at, saved_at;
    const char *shot; /* the script's: this pass's frame is written as this */
} V;

static volatile sig_atomic_t term;
static void on_term(int sig) {
    term = 1;
    wake_main();
}

/* a line for the script's log: the checks read what happened from it */
static void say(const char *fmt, ...) {
    char line[1200];
    va_list ap;
    if (P.plat != &plat_script) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    script_log(line);
}

/* ---- The card ------------------------------------------------------------------------------ */

enum { SAVE_MARKS = 1, SAVE_SETTINGS = 2 };
typedef struct {
    int what; /* SAVE_ */
    settings_t set;
    marks_t marks;
} job_t;
static marks_t marks;
static pthread_t saver;
static int saving, unsaved; /* unsaved: what a save left out, for the next */

static void *save_main(void *arg) {
    job_t *j = arg;
    if (j->what & SAVE_SETTINGS) settings_save(&j->set);
    if (j->what & SAVE_MARKS) marks_save(&j->marks);
    marks_free(&j->marks);
    free(j);
    return 0;
}

/* 1 when no save is on its way, waiting ms at most for the one that is. */
static int save_done(int ms) {
    struct timespec by;
    clock_gettime(CLOCK_REALTIME, &by);
    by.tv_sec += ms / 1000, by.tv_nsec += ms % 1000 * 1000000L;
    if (by.tv_nsec >= 1000000000L) by.tv_sec += 1, by.tv_nsec -= 1000000000L;
    if (saving && !pthread_timedjoin_np(saver, 0, &by)) saving = 0;
    return !saving;
}

/* The marks or the settings to the card, by a thread of their own from a copy: the card's sync
 * takes a moment the picture hasn't got. One at a time: when the last is still at the card wait
 * ms on, this one is left for the next to take along (the last has nearly the same place, or
 * the card is stuck and this would be). */
static void save(int what, int wait) {
    unsaved |= what;
    job_t *j = save_done(wait) ? calloc(1, sizeof *j) : 0;
    if (!j) return;
    j->what = unsaved, j->set = P.set;
    if (unsaved & SAVE_MARKS) {
        j->marks = marks;
        for (int i = 0; i < marks.n; ++i)
            if (!(j->marks.v[i].rel = strdup(marks.v[i].rel))) { /* no memory: the file stays */
                j->marks.n = i;
                marks_free(&j->marks);
                free(j);
                return;
            }
    }
    unsaved = 0;
    saving = !pthread_create(&saver, 0, save_main, j);
    if (!saving) save_main(j);
}

/* The video's mark as it stands, to the card: watched at its end, else the place. */
static void place_save(long long now, int wait) {
    V.saved_at = now;
    if (!V.mark) return;
    if (V.end == END_OVER)
        V.mark->flags |= MARK_WATCHED, V.mark->pos_ms = 0;
    else
        mark_place(V.mark, V.at_ms, V.m->dur_ms);
    save(SAVE_MARKS, wait);
    say("save pos=%lld watched=%u", V.mark->pos_ms, V.mark->flags & MARK_WATCHED);
}

/* ---- Subtitles ----------------------------------------------------------------------------- */

static const char *sub_name(int n) {
    return n < 0 ? "Off" : n < V.nside ? V.side[n].name : V.m->subs[n - V.nside].name;
}

/* The subtitles on: the nth of the list, or none for -1 and for one that can't be shown. */
static void sub_set(int n) {
    cues_free(&V.cues);
    V.sub = -1, ++V.rev;
    if (n >= 0 && n < V.nside) {
        size_t len;
        char *text = file_get(V.side[n].path, &len);
        if (text) srt_parse(text, len, &V.cues), V.sub = n;
        free(text);
    } else if (n >= V.nside && n - V.nside < V.m->nsubs && V.m->subs[n - V.nside].ok)
        V.sub = n;
    eng_subs(V.sub >= V.nside ? V.m->subs[V.sub - V.nside].index : -1);
}

/* The lines on show at pos into V.subtext; V.sub_next is when they next change. */
static void subs_now(long long pos) {
    char text[sizeof V.subtext] = "", own[3][LINE_N];
    const cue_t *cue[3];
    int n = 0;
    V.sub_next = -1;
    if (V.sub >= 0 && V.sub < V.nside)
        n = cues_at(&V.cues, pos, cue, 3, &V.sub_next);
    else if (V.sub >= 0)
        n = eng_cues(pos, own, 3, &V.sub_next);
    for (int i = 0; i < n; ++i) {
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%s", len ? "\n" : "",
                 V.sub < V.nside ? cue[i]->text : own[i]);
    }
    if (strcmp(text, V.subtext)) strcpy(V.subtext, text), ++V.rev;
}

/* ---- What the input does ------------------------------------------------------------------- */

static void notice(const char *text, long long now) {
    snprintf(V.notice, sizeof V.notice, "%s", text);
    V.notice_until = now + NOTICE_MS, ++V.rev;
}

/* Up while it is paused, until a tap puts them away: not for the options' own pause. */
static int controls_shown(long long now) {
    return (V.paused && !V.hidden && !V.resume) || now < V.controls_until || now < V.seek_until;
}

static void seek(long long ms, int how, long long now) {
    if (V.state != ENG_READY) return;
    if (ms < 0) ms = 0;
    eng_seek(ms, how);
    V.at_ms = ms, V.pend_ms = -1;
    V.controls_until = now + CONTROLS_MS;
}

/* A place the wheel or a finger may choose: never so near the end that letting go ends the video.
 */
static long long keep(long long ms) {
    long long dur = V.m->dur_ms, max = dur > 2 * END_KEEP ? dur - END_KEEP : dur;
    return ms < 0 ? 0 : dur > 0 && ms > max ? max : ms;
}

/* The chosen place sent, ahead to the next keyframe or back to the one before; the nearest the
 * end that may be chosen, to no keyframe after it. */
static void seek_send(long long now) {
    if (V.pend_ms < 0) return;
    seek(V.pend_ms,
         V.pend_ms < V.at_ms                ? SEEK_BACK
         : keep(V.pend_ms + 1) == V.pend_ms ? SEEK_UPTO
                                            : SEEK_AHEAD,
         now);
}

/* A side button's step, or a double tap's: secs on or back, what the run has moved shown beside
 * it. A press (fresh) that goes past the end ends the video, as if it had played out: that is
 * one in its last ten seconds. A held button's repeats never do: they stop short of the end
 * where the wheel does. */
static void step(int secs, int fresh, long long now) {
    if (V.state != ENG_READY) return;
    long long dur = V.m->dur_ms, to = V.at_ms + secs * 1000LL;
    int how = secs > 0 ? SEEK_AHEAD : SEEK_BACK;
    if (to < 0) to = 0;
    if (!fresh && secs > 0 && keep(to) < to) to = keep(to), how = SEEK_UPTO;
    if (dur > 0 && to >= dur && fresh) {
        V.end = END_OVER;
        return;
    }
    if ((dur > 0 && to >= dur) || (secs > 0 && to <= V.at_ms)) return; /* as far as it goes */
    if (now >= V.badge_until || (V.badge < 0) != (secs < 0)) V.badge = 0;
    V.badge += (int)((to - V.at_ms) / 1000), V.badge_until = now + BADGE_MS;
    seek(to, how, now);
}

static void pause_set(int on, long long now) {
    if (V.state != ENG_READY) return;
    if (!on && V.s.lost) eng_output(); /* Play after the output was lost: the device once more */
    V.paused = on, V.hidden = 0;
    eng_pause(on);
    if (!V.level) /* the options' own pause says nothing */
        V.controls_until = now + CONTROLS_MS, V.disc_until = on ? now + DISC_MS : 0;
    if (on) place_save(now, 300);
}

/* The volume file, whole: a level for Q2 Pod to set, or "?" to be told it. */
static void volume_ask(const char *text) { file_put(P.volume_path, text, strlen(text)); }

static void volume_turn(int ticks, long long now) {
    char text[8];
    if (P.volume < 0) return; /* Q2 Pod hasn't said where it is yet */
    P.volume += ticks;
    P.volume = P.volume < 0 ? 0 : P.volume > 100 ? 100 : P.volume;
    snprintf(text, sizeof text, "%d", P.volume);
    volume_ask(text);
    P.asked_at = now, V.volume_until = now + VOLUME_MS;
}

/* Q2 Pod's word on the volume: the level as it is (ours, or less at the Maximum volume, or a
 * headset's own change), and on Bluetooth or USB the gain to apply. While our own asking is
 * fresh the bar stays where the wheel put it: answers to a fast turn come one behind another. */
static void volume_told(const event_t *e, long long now) {
    int first = P.told < 0; /* its answer to the start's asking: no bar for that */
    P.told = e->a < 0 ? 0 : e->a > 100 ? 100 : e->a;
    if (*e->arg) eng_gain(P.gain = arg_gain(e->arg));
    if (P.asked_at) return;
    if (!first && P.volume != P.told) V.volume_until = now + VOLUME_MS;
    P.volume = P.told;
}

/* ---- The menu ------------------------------------------------------------------------------ */

static void row(const char *label, const char *value, int off) {
    row_t *r = &V.menu.row[V.menu.n++];
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->value, sizeof r->value, "%s", value);
    r->off = off;
}

static void menu_open(int level, int sel) {
    const media_t *m = V.m;
    char time[16];
    V.level = level, ++V.rev;
    V.menu.n = V.menu.sel = V.menu.top = 0;
    switch (level) {
    case MENU_OPTIONS: /* only the rows that apply */
        V.menu.title = "Options";
        if (m->naudio > 1) V.act[V.menu.n] = ROW_AUDIO, row("Audio", m->audio[V.audio].name, 0);
        if (V.nside + m->nsubs) V.act[V.menu.n] = ROW_SUBS, row("Subtitles", sub_name(V.sub), 0);
        if (m->video) V.act[V.menu.n] = ROW_FIT, row("Fit to Screen", P.set.fill ? "On" : "Off", 0);
        if (m->nchapters) V.act[V.menu.n] = ROW_CHAPTERS, row("Chapters", "", 0);
        V.act[V.menu.n] = ROW_START, row("Start Over", "", 0);
        for (int i = 0; i < V.menu.n; ++i)
            if (V.act[i] == sel) V.menu.sel = i;
        break;
    case MENU_AUDIO:
        V.menu.title = "Audio";
        for (int i = 0; i < m->naudio; ++i)
            row(m->audio[i].name, i == V.audio ? "\xe2\x9c\x93" : "", 0);
        V.menu.sel = V.audio;
        break;
    case MENU_SUBS:
        V.menu.title = "Subtitles";
        row("Off", V.sub < 0 ? "\xe2\x9c\x93" : "", 0);
        for (int i = 0; i < V.nside + m->nsubs; ++i)
            row(sub_name(i), i == V.sub ? "\xe2\x9c\x93" : "",
                i >= V.nside && !m->subs[i - V.nside].ok);
        V.menu.sel = V.sub + 1;
        break;
    case MENU_CHAPTERS:
        V.menu.title = "Chapters";
        for (int i = 0; i < m->nchapters; ++i) {
            row(m->chapter[i].title, time_text(m->chapter[i].start_ms, time, sizeof time), 0);
            if (m->chapter[i].start_ms <= V.at_ms) V.menu.sel = i;
        }
        break;
    }
    if (V.menu.row[V.menu.sel].off) menu_move(&V.menu, 1); /* the first row that can be chosen */
    menu_move(&V.menu, 0);
}

/* The options: the video waits while they are open. */
static void options_open(long long now) {
    menu_open(MENU_OPTIONS, 0);
    if (!V.paused) V.resume = 1, pause_set(1, now);
}

/* And plays on when they close, if it was playing when they opened. */
static void menu_close(long long now) {
    if (V.resume) pause_set(0, now);
    V.resume = 0, V.level = MENU_NONE, ++V.rev;
}

static void menu_choose(long long now) {
    int at = V.menu.sel;
    if (at < 0 || at >= V.menu.n || V.menu.row[at].off) return;
    switch (V.level) {
    case MENU_OPTIONS:
        switch (V.act[at]) {
        case ROW_AUDIO:
            menu_open(MENU_AUDIO, 0);
            break;
        case ROW_SUBS:
            menu_open(MENU_SUBS, 0);
            break;
        case ROW_CHAPTERS:
            menu_open(MENU_CHAPTERS, 0);
            break;
        case ROW_FIT:
            P.set.fill = !P.set.fill;
            save(SAVE_SETTINGS, 0);
            eng_fill(P.set.fill);
            /* paused, no new picture comes to take the shape: the one on show is decoded again */
            if (V.paused && V.s.pic_ms >= 0) eng_seek(V.at_ms = V.s.pic_ms, SEEK_EXACT);
            menu_open(MENU_OPTIONS, ROW_FIT);
            break;
        case ROW_START:
            V.level = MENU_NONE, V.resume = 0;
            seek(0, SEEK_BACK, now);
            pause_set(0, now);
            break;
        }
        break;
    case MENU_AUDIO:
        if (at != V.audio) eng_audio(V.audio = at);
        if (V.mark) V.mark->audio = at;
        menu_open(MENU_OPTIONS, ROW_AUDIO);
        break;
    case MENU_SUBS:
        sub_set(at - 1);
        if (V.mark) V.mark->sub = V.sub + 1; /* 0 is Off; the default is -1 */
        V.own = 0;
        /* and the next video's default: Off stays off, a track turns them on */
        if (P.set.subs != (at > 0)) P.set.subs = at > 0, save(SAVE_SETTINGS, 0);
        menu_open(MENU_OPTIONS, ROW_SUBS);
        break;
    case MENU_CHAPTERS:
        menu_close(now);
        seek(V.m->chapter[at].start_ms, SEEK_EXACT, now);
        break;
    }
    ++V.rev;
}

/* Return in a menu, or Play/Pause held: a list goes back to Options, on the row that opened it;
 * Options closes. */
static void menu_back(long long now) {
    static const int from[] = { 0, 0, ROW_AUDIO, ROW_SUBS, ROW_CHAPTERS };
    if (V.level > MENU_OPTIONS)
        menu_open(MENU_OPTIONS, from[V.level]);
    else
        menu_close(now);
}

/* ---- Keys, the wheel, touches -------------------------------------------------------------- */

static void side(int dir, int secs, int fresh, long long now) {
    if (V.level)
        menu_move(&V.menu, dir);
    else
        step(dir * secs, fresh, now);
}

static int key_of(int code) { /* K_, -1 for a key that is none of ours */
    static const int codes[KEYS] = { KEY_RETURN, KEY_PLAY, KEY_CENTRE, KEY_BACK, KEY_FWD };
    int k = KEYS - 1;
    while (k >= 0 && codes[k] != code) --k;
    return k;
}

static void key(const event_t *e, long long now) {
    int k = key_of(e->a);
    if (k < 0) return;
    if (e->what == 'd') {
        if (P.held >> k & 1) return; /* a held key's repeat, whichever video it went down in */
        P.held |= 1u << k;
        V.down[k] = now, V.repeats = 0;
        if (k == K_BACK || k == K_FWD) side(k == K_FWD ? 1 : -1, 10, 1, now);
        return;
    }
    /* not ours: its press was the list's or the video's before, or a hold has spent it */
    int ours = V.down[k] != 0;
    P.held &= ~(1u << k), V.down[k] = 0;
    if (!ours) return;
    if (V.state != ENG_READY) { /* any key leaves the message; Return a video still opening */
        if (V.state == ENG_FAILED || k == K_RETURN) V.end = END_QUIT;
        return;
    }
    if (k == K_PLAY && V.level)
        menu_close(now);
    else if (k == K_PLAY)
        pause_set(!V.paused, now);
    else if (k == K_CENTRE && V.level)
        menu_choose(now);
    else if (k == K_CENTRE) {                       /* seek mode on, or off with the place chosen */
        if (now - V.centre_at < CENTRE_GAP) return; /* not in and out again */
        V.centre_at = now;
        seek_send(now);
        V.seek_until = now < V.seek_until ? 0 : now + CONTROLS_MS;
    } else if (k == K_RETURN && V.level)
        menu_back(now);
    else if (k == K_RETURN && now < V.seek_until)
        seek_send(now), V.seek_until = 0;
    else if (k == K_RETURN)
        V.end = END_QUIT;
}

static void wheel(int travel, long long now) {
    if (now - V.wheel_at > WHEEL_REST) V.wheel_acc = V.run = 0; /* a new turn */
    V.wheel_at = now;
    int ticks = wheel_ticks(&V.wheel_acc, travel);
    if (!ticks || V.state != ENG_READY) return;
    if (V.level)
        menu_move(&V.menu, ticks);
    else if (now < V.seek_until) {
        int secs = seek_ramp((int)(now - V.tick_at > 10000 ? 10000 : now - V.tick_at), ++V.run);
        V.pend_ms = keep((V.pend_ms >= 0 ? V.pend_ms : V.at_ms) + ticks * secs * 1000LL);
        V.pend_at = now + SEEK_SEND_MS, V.seek_until = now + CONTROLS_MS;
        V.target_until = now + BADGE_MS;
    } else
        volume_turn(ticks, now);
    V.tick_at = now;
}

static void tap(int x, int y, long long now) {
    if (V.level) { /* a row is chosen; outside the box closes it */
        int at = menu_hit(&P.pop, &V.menu, x, y);
        if (at >= 0) V.menu.sel = at, menu_choose(now);
        if (at == -2) menu_close(now);
        return;
    }
    int third = x * 3 / VIEW_W;
    if (third != 1 && third == V.tap_third && now - V.tap_at <= TAP_MS) {
        V.tap_at = now; /* a double tap at a side, and every tap that follows it at once */
        step(third ? 10 : -10, 1, now);
        return;
    }
    V.tap_at = now, V.tap_third = third;
    if (!controls_shown(now)) {
        V.controls_until = now + CONTROLS_MS, V.hidden = 0;
        return;
    }
    switch (hit(x, y)) {
    case HIT_MENU:
        options_open(now);
        break;
    case HIT_MIDDLE:
        pause_set(!V.paused, now);
        break;
    case HIT_BAR:
        break; /* the touch's down took it */
    default:   /* a tap on the picture puts the bands away, paused or not */
        V.controls_until = V.seek_until = 0, V.hidden = 1;
    }
}

static void touch(const event_t *e, long long now) {
    int x = e->a, y = e->b;
    if (e->what == 'd') {
        P.held |= 1u << FINGER;
        V.touching = 1, V.moved = 0, V.tx = x, V.ty = y, V.touch_at = now;
        /* a finger on the bar takes the place along with it */
        if (V.state == ENG_READY && !V.level && controls_shown(now) && hit(x, y) == HIT_BAR &&
            V.m->dur_ms > 0)
            V.drag = 1, V.pend_ms = keep(bar_ms(x, V.m->dur_ms));
        return;
    }
    if (e->what == 'u') P.held &= ~(1u << FINGER);
    if (!V.touching) return; /* it began before the video did */
    if (abs(x - V.tx) > 12 || abs(y - V.ty) > 12) V.moved = 1;
    if (V.drag) V.pend_ms = keep(bar_ms(x, V.m->dur_ms));
    if (e->what != 'u') return;
    V.touching = 0;
    if (V.state == ENG_FAILED)
        V.end = END_QUIT; /* a touch leaves the message */
    else if (V.drag) {
        V.drag = 0;
        seek_send(now);
    } else if (!V.moved && now - V.touch_at <= 500 && V.state == ENG_READY)
        tap(x, y, now);
}

static void input(const event_t *e, long long now) {
    if (e->kind == 'v') {
        volume_told(e, now);
        return;
    }
    if (e->kind == 's') V.shot = e->arg;
    if (e->kind == 'f') eng_test(0, e->arg, 0);
    if (e->kind != 'k' && e->kind != 't' && e->kind != 'w') return;
    V.input_at = now;
    if (P.dim) unlink(P.dim_path), P.dim = 0; /* any input brings the panel back */
    if (e->kind == 'k') key(e, now);
    if (e->kind == 'w') wheel(e->a, now);
    if (e->kind == 't') touch(e, now);
}

/* ---- Time ---------------------------------------------------------------------------------- */

/* What has come due. */
static void timers(long long now) {
    /* Play/Pause held: the options, or back a level in them */
    if (V.down[K_PLAY] && now - V.down[K_PLAY] >= HOLD_MS) {
        V.down[K_PLAY] = 0;
        if (V.level)
            menu_back(now);
        else if (V.state == ENG_READY)
            options_open(now);
    }
    for (int k = K_BACK; k <= K_FWD; ++k) { /* a side button held: its steps */
        long long held = V.down[k] ? now - V.down[k] : 0;
        if (held > HOLD_MAX) V.down[k] = 0, P.held &= ~(1u << k), held = 0;
        while (V.repeats < hold_repeats(held))
            side(k == K_FWD ? 1 : -1, hold_step(500 + 250 * V.repeats), 0, now), ++V.repeats;
    }
    if (V.pend_ms >= 0 && !V.drag && now >= V.pend_at) seek_send(now); /* the wheel rested */
    /* our asking is old: Q2 Pod's word counts */
    if (P.asked_at && now - P.asked_at >= VOLUME_SETTLE) {
        P.asked_at = 0;
        if (P.told >= 0 && P.told != P.volume) P.volume = P.told, V.volume_until = now + VOLUME_MS;
    }
    if (V.paused && !P.dim && now - V.input_at >= DIM_MS)
        P.dim = 1, close(open(P.dim_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
    if (V.paused && now - V.input_at >= IDLE_MS) V.end = END_QUIT;
    if (V.state == ENG_READY && !V.paused && now - V.saved_at >= SAVE_MS) place_save(now, 300);
    if (V.error_until && now >= V.error_until) V.end = END_QUIT;
}

/* How long the main thread may wait: until the next thing above, or on the screen, is due. */
static int wait_ms(long long now) {
    long long at = now + 3600000;
    const long long due[] = {
        V.controls_until,
        V.seek_until,
        V.volume_until,
        V.badge_until,
        V.notice_until,
        V.error_until,
        V.disc_until,
        V.target_until,
        V.pend_ms >= 0 && !V.drag ? V.pend_at : 0,
        P.asked_at ? P.asked_at + VOLUME_SETTLE : 0,
        V.down[K_PLAY] ? V.down[K_PLAY] + HOLD_MS : 0,
        V.down[K_BACK] ? V.down[K_BACK] + 500 + 250 * V.repeats : 0,
        V.down[K_FWD] ? V.down[K_FWD] + 500 + 250 * V.repeats : 0,
        V.paused ? V.input_at + (P.dim ? IDLE_MS : DIM_MS) : 0,
        V.state == ENG_READY && !V.paused ? V.saved_at + SAVE_MS : 0,
        /* while it plays, a look at the engine each second at least: an output that takes no
         * more sound is noticed there (eng_poll), whatever else is or isn't due */
        V.state == ENG_READY && !V.paused ? now + 1000 : 0,
        V.state == ENG_READY && !V.paused && V.s.due_ms >= 0 ? now + V.s.due_ms : 0,
        /* the lines change, and the time the controls say */
        V.state == ENG_READY && !V.paused && V.sub_next >= 0 ? now + V.sub_next - V.s.pos_ms + 1
                                                             : 0,
        V.state == ENG_READY && !V.paused && controls_shown(now) ? now + 1000 - V.s.pos_ms % 1000
                                                                 : 0,
    };
    for (size_t i = 0; i < sizeof due / sizeof *due; ++i)
        if (due[i] > now && due[i] < at) at = due[i]; /* what is past was seen to this pass */
    return (int)(at - now);
}

/* ---- The frame ----------------------------------------------------------------------------- */

static void frame(long long now, int force) {
    static struct {
        int v[20];
    } drawn;
    static long long chapter[CHAPTERS];
    int ready = V.state == ENG_READY, seeking = ready && (now < V.seek_until || V.drag);
    /* before the first picture; a video whose picture never comes plays as sound alone */
    int loading = V.state == ENG_OPENING ||
                  (ready && V.m->video && !V.s.picture && V.s.pos_ms < V.start_ms + 2000);
    osd_t o = { .pop = &P.pop, .title = V.title, .volume = -1, .subsize = P.set.subsize };
    if (ready && !loading && !V.began) { /* its first picture: where it is, when that is news */
        V.began = 1;
        if (V.start_ms > 0 || V.autoplay) V.controls_until = now + CONTROLS_MS;
    }
    o.pos_ms = V.pend_ms >= 0 ? V.pend_ms : V.at_ms, o.dur_ms = ready ? V.m->dur_ms : 0;
    o.controls = ready && !loading && controls_shown(now), o.knob = seeking,
    o.paused = ready && V.paused;
    o.target = seeking && now < V.target_until;
    o.disc = o.paused && now < V.disc_until && !V.level && !o.target;
    if (ready)
        for (o.chapter = chapter; o.nchapters < V.m->nchapters; ++o.nchapters)
            chapter[o.nchapters] = V.m->chapter[o.nchapters].start_ms;
    if (ready && now < V.volume_until) o.volume = P.volume;
    o.menu = ready && V.level ? &V.menu : 0;
    if (!o.menu) { /* nothing shows through a menu */
        if (now < V.badge_until) o.badge = V.badge;
        o.notice = V.s.lost ? "Sound output lost" : now < V.notice_until ? V.notice : 0;
        o.subs = *V.subtext ? V.subtext : 0;
    }
    o.plain = !V.s.picture || V.state == ENG_FAILED || loading;
    if (V.state == ENG_FAILED)
        o.message = "Can't play this video", o.reason = V.why;
    else if (loading)
        o.message = "Loading\xe2\x80\xa6";

    /* drawn again for a new picture, or when what is over it differs */
    const int sig[20] = { o.controls,    o.knob,     o.paused,
                          o.volume,      o.badge,    o.controls ? (int)(o.pos_ms / 1000) : 0,
                          V.level,       V.menu.sel, V.menu.top,
                          o.notice != 0, o.plain,    V.state,
                          loading,       V.rev,      V.s.lost,
                          P.set.subsize, P.dim,      o.controls ? (int)(o.dur_ms / 1000) : 0,
                          o.disc,        o.target };
    if (!V.s.fresh && !force && !memcmp(sig, drawn.v, sizeof sig)) return;
    memcpy(drawn.v, sig, sizeof sig);
    const pix *out = P.cur;
    if (osd_any(&o)) {
        if (!o.plain) memcpy(P.page, P.cur, PAGE_PX * sizeof(pix));
        osd_draw(P.page, &o);
        out = P.page;
    }
    P.plat->show(P.out = out);
    if (V.shot) {
        char text[sizeof V.subtext];
        snprintf(text, sizeof text, "%s", V.subtext);
        for (char *c = text; *c; ++c)
            if (*c == '\n') *c = '|';
        script_shot(V.shot, out);
        say("shot=%s t=%lld state=%d pos=%lld dur=%lld pic=%lld paused=%d controls=%d seek=%d "
            "volume=%d "
            "told=%d menu=%d sel=%d "
            "sub=%d audio=%d fill=%d geom=%d,%d,%d,%d frames=%lld tone=%d level=%d lost=%d "
            "silent=%d dim=%d badge=%d disc=%d target=%d "
            "serial=%d gain=%d queued=%lld notice=%s| text=%s",
            V.shot, now, V.state, o.pos_ms, o.dur_ms, V.s.pic_ms, V.paused, o.controls, o.knob,
            o.volume, P.told, V.level, V.menu.sel, V.sub, V.audio, P.set.fill, V.s.geom.vx,
            V.s.geom.vy, V.s.geom.vw, V.s.geom.vh, V.s.frames, V.s.tone, V.s.level, V.s.lost,
            V.s.silent, P.dim, o.badge, o.disc, o.target, V.s.serial, P.gain, V.s.queued,
            o.notice ? o.notice : "", text);
        V.shot = 0;
    }
}

/* ---- A video ------------------------------------------------------------------------------- */

/* The run is over: what went down here is waited for (see the top), the last frame left up. A
 * release can be lost, so no longer than LET_GO_MS after a key's last repeat; and when it is
 * Q2 Pod that ends the run, by a held Return whose release it keeps to itself, the other keys
 * get a moment at most, as it kills what is still there three seconds on. */
static void let_go(void) {
    event_t e;
    for (long long until = now_ms() + LET_GO_MS;;) {
        long long now = now_ms();
        if (term) {
            P.held &= ~(1u << K_RETURN);
            if (until > now + LET_GO_TERM) until = now + LET_GO_TERM;
        }
        if (!P.held || now >= until) return;
        if (!P.plat->wait((int)(until - now), &e)) continue;
        if (e.kind == 's') script_shot(e.arg, P.out), say("shot=%s held=%u", e.arg, P.held);
        int k = e.kind == 'k' ? key_of(e.a) : e.kind == 't' ? FINGER : -1;
        if (k < 0 || e.what == 'm') continue;
        if (e.what == 'u')
            P.held &= ~(1u << k);
        else if (!term) {
            if (P.held >> k & 1) until = now_ms() + LET_GO_MS; /* a repeat: it is down yet */
            P.held |= 1u << k;
        }
    }
}

/* Every way out: the panel no longer dimmed, the headphone DAC muted as Q2 Pod's own player
 * leaves it (the audio thread does that when it closes its output, but one left hanging in the
 * output never will), and the screen given back. At exit() too, for the ways out that are not
 * the end of player_main. */
static void leave(void) {
    static int left;
    if (left++) return;
    if (P.dim) unlink(P.dim_path);
#ifndef HOST
    alsa_mute();
#endif
    P.plat->close();
}

static void *lock_main(void *unused) {
    lock_take(1); /* its fd is the process's to the end */
    return 0;
}

/* The engine says the video is open: its mark, its tracks, the subtitles wanted. */
static void opened(long long now) {
    const media_t *m = V.m = eng_media();
    if ((V.mark = marks_put(&marks, V.rel, V.size)) && m->dur_ms > 0) V.mark->dur_ms = m->dur_ms;
    V.audio = m->audio_cur < 0 ? 0 : m->audio_cur;
    /* by default the first subtitles that can be shown; a remembered choice as it was */
    if (V.own && V.want >= V.nside) {
        V.want = -1;
        for (int i = m->nsubs - 1; i >= 0; --i)
            if (m->subs[i].ok) V.want = V.nside + i;
    }
    sub_set(V.want);
    if (*m->note) notice(m->note, now);
    V.saved_at = V.input_at = now;
    say("ready dur=%lld video=%d audio=%d naudio=%d nsubs=%d nside=%d chapters=%d sub=%d",
        m->dur_ms, m->video, m->sound, m->naudio, m->nsubs, V.nside, m->nchapters, V.sub);
}

/* Plays the video at rel to its end, or until it is left: END_OVER or END_QUIT. */
static int play(const char *rel, int autoplay) {
    struct stat st;
    long long now = now_ms();
    cues_free(&V.cues);
    memset(&V, 0, sizeof V);
    snprintf(V.rel, sizeof V.rel, "%s", rel);
    lib_title(rel, V.title, sizeof V.title);
    V.pend_ms = V.sub_next = -1, V.sub = -1, V.input_at = now, V.autoplay = autoplay;
    V.state = ENG_FAILED, V.why = "The file can't be read.";
    say("open %s", rel);
    if (lib_path(P.root, rel, V.path, sizeof V.path) && !stat(V.path, &st) && S_ISREG(st.st_mode)) {
        const mark_t *k = marks_find(&marks, rel, st.st_size);
        int sub = k ? k->sub : -1;
        V.size = st.st_size;
        V.start_ms = V.at_ms = k && !(k->flags & MARK_WATCHED) ? k->pos_ms : 0;
        V.nside = sidecars(V.path, V.side, SIDES);
        V.own = sub < 0;
        V.want = sub > 0 ? sub - 1 : sub < 0 && P.set.subs ? 0 : -1;
        V.state = ENG_OPENING, V.why = 0; /* the engine's to say */
        eng_open(V.path, V.start_ms, k ? k->dur_ms : 0, P.device, P.gain, k ? k->audio : -1,
                 V.want >= V.nside ? V.want - V.nside : -1, P.set.fill);
    }
    frame(now, 1);
    for (int was = -1; !V.end;) {
        event_t e;
        int got = P.plat->wait(wait_ms(now), &e);
        now = now_ms();
        if (term) {
            V.end = END_QUIT;
            break;
        }
        /* all that wait, not one a pass: Q2 Pod's socket holds about ten, and a release that
         * found it full while this thread drew would be lost */
        for (int n = 0; got && n < 32; ++n, got = P.plat->wait(0, &e)) input(&e, now);
        timers(now);
        if (V.state != ENG_FAILED) {
            eng_poll(&V.s, &P.cur);
            V.state = V.s.state;
            if (V.pend_ms < 0 || V.state != ENG_READY) V.at_ms = V.s.pos_ms;
        }
        if (V.state != was) { /* it opened, or can't be */
            if (V.state == ENG_READY) opened(now);
            if (V.state == ENG_FAILED) {
                if (was == ENG_READY) place_save(now, 300); /* it stopped midway: from there */
                if (!V.why) V.why = eng_why();
                V.error_until = now + ERROR_MS;
                say("fail %s", V.why);
            }
            was = V.state;
        }
        if (V.state == ENG_READY) {
            if (V.s.lost && !V.lost) pause_set(1, now); /* the output went: stop, and say so */
            if (V.s.silent && !V.silent) notice("Playing without sound", now);
            V.lost = V.s.lost, V.silent = V.s.silent;
            subs_now(V.s.pos_ms);
            if (V.s.ended) V.end = END_OVER;
        }
        frame(now, V.shot != 0);
    }
    /* the last save waits for one still at the card, so a video's end is never the one left
     * out; the sound stops first. Not that long when Q2 Pod is ending the run: it kills what
     * is still here three seconds on. */
    if (V.state == ENG_READY) eng_pause(1), place_save(now, term ? 300 : 1200);
    say("end %s %s pos=%lld", rel, V.end == END_OVER ? "over" : term ? "term" : "quit", V.at_ms);
    P.stuck = !eng_close();
    return V.end;
}

int player_main(int argc, char **argv) {
    const char *uri = getenv("Q2_URI"), *rel = getenv("Q2_CONTEXT");
    char next[REL_MAX + 1];
    av_log_set_level(AV_LOG_QUIET); /* what FFmpeg has to say of a file, the screen says */
    /* alsa-lib 1.1.6 deadlocks in its own locking otherwise */
    setenv("LIBASOUND_THREAD_SAFE", "0", 1);
#if defined(__mips__)
    /* The Q2's FPU traps on a denormal and the kernel then does the sum by hand, a thousand
     * times slower: a decoder's filter fading to silence is full of them. Flushed to zero
     * instead (FCSR's FS bit); every thread started from here on takes the setting along. */
    unsigned fcsr;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
    fcsr |= 1u << 24;
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcsr));
#endif
    if (!uri || strcmp(uri, "play") || !rel) return 0; /* no row of ours */
#ifdef HOST
    P.plat = &plat_script;
#else
    P.plat = getenv("VIDEOS_SCRIPT") ? &plat_script : &plat_q2;
#endif
    wake_fd(); /* before a signal could have something to say */
    struct sigaction sa = { .sa_handler = on_term, .sa_flags = SA_RESTART };
    sigaction(SIGTERM, &sa, 0), sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    /* The lock that keeps the page's thumbnails off the Q2 while a video plays, held from when
     * it is had to the end. Taken by a thread of its own that is never waited for: a thumbnail
     * in the making keeps it for seconds, and the video shouldn't start that much later. */
    pthread_t locker;
    if (!pthread_create(&locker, 0, lock_main, 0)) pthread_detach(locker);
    if (!P.plat->open()) {
        fprintf(stderr, "q2-videos: no Q2 screen%s\n",
                P.plat == &plat_script ? ", and no script in $VIDEOS_SCRIPT" : "");
        return 0;
    }
    atexit(leave);
    settings_load(&P.set);
    marks_load(&marks);
    pop_read(&P.pop, getenv("Q2_POPUP"));
    font_open();
    lib_root(P.root, sizeof P.root);
    P.cur = malloc(PAGE_PX * sizeof(pix)), P.page = malloc(PAGE_PX * sizeof(pix));
    if (!P.cur || !P.page) return 0;
    for (int i = 0; i < PAGE_PX; ++i) P.cur[i] = P.page[i] = 0xff000000u;
    snprintf(P.volume_path, sizeof P.volume_path, "%s/q2-videos.volume", tmp_dir);
    snprintf(P.dim_path, sizeof P.dim_path, "%s/q2-videos.dim", tmp_dir);
    /* The sound is Q2 Pod's to hand over: the device, and on Bluetooth or USB the volume to
     * apply. Where the volume stands it says when asked, now that the socket can hear it;
     * until it has, the level is that argument's, and without one the wheel waits. */
    P.device = P.plat == &plat_script ? 0 : argc > 1 ? argv[1] : 0;
    P.gain = arg_gain(argc > 2 ? argv[2] : 0);
    P.volume = arg_level(argc > 2 ? argv[2] : 0), P.told = -1;
    volume_ask("?");
    if (P.plat == &plat_script)
        eng_test((unsigned)atoi(getenv("VIDEOS_MEM_KB") ? getenv("VIDEOS_MEM_KB") : "0"),
                 getenv("VIDEOS_SINK"), atoi(getenv("VIDEOS_SLOW") ? getenv("VIDEOS_SLOW") : "0"));

    snprintf(next, sizeof next, "%s", rel);
    /* a video played out is followed by the next in its folder, when Autoplay says so */
    for (int nth = 0; play(next, nth > 0) == END_OVER && !P.stuck && !term && P.set.next &&
                      lib_next(P.root, V.rel, P.set.sort, next, sizeof next);
         ++nth) {}
    if (!P.stuck) let_go();
    if (unsaved) save(0, 1200); /* a setting no save has taken along */
    save_done(1200);            /* the place reaches the card before the screen is given back */
    leave();
    if (P.stuck || !save_done(0)) _exit(0); /* a thread is still in the card's or ALSA's hands */
    cues_free(&V.cues);
    marks_free(&marks);
    free(P.cur), free(P.page);
    return 0;
}
