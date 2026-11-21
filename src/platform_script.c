/* The player without a Q2: a script of timed events in place of Q2 Pod's input socket, no screen,
 * and frames written as files when the script asks. In both builds, so the Q2's own binary can be
 * run the same way under an emulator. $VIDEOS_SCRIPT names the script, a line an event:
 *
 *     <ms> k d 171        one of Q2 Pod's datagrams (k, t, w, v), <ms> after the start
 *     <ms> shot <name>    the frame on the screen, to $VIDEOS_OUT/<name>.ppm, and a line that says
 *                         where the player is, to $VIDEOS_OUT/log
 *     <ms> sink <fault>   the sound output fails: stall, lost; back mends it
 *     <ms> term           SIGTERM, as Q2 Pod's held Return sends it
 *
 * Time is the script's own (clock_virtual), so a run takes as long as its decoding and always
 * shows the same frames, from $VIDEOS_CLOCK ms when that is a number (a Q2 long awake);
 * $VIDEOS_CLOCK=real plays it on the real clock. */
#include "player.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *script, *next; /* the script's text, and its next line */
static long long began;
static int real;

static int script_open(void) {
    const char *path = getenv("VIDEOS_SCRIPT"), *clock = getenv("VIDEOS_CLOCK");
    if (!path || !(script = file_get(path, 0))) return 0;
    real = clock && !strcmp(clock, "real");
    if (!real) clock_virtual(clock ? atoi(clock) : 0);
    next = script, began = now_ms();
    return 1;
}

static int script_wait(int ms, event_t *e) {
    long long now = now_ms(), until = now + (ms < 0 ? 3600000 : ms);
    while (*next == '\n') ++next;
    long long at = *next ? began + atoll(next) : -1;
    if (at >= 0 && at < until) until = at;
    if (real) {
        struct pollfd p = { wake_fd(), POLLIN, 0 };
        if (until > now) poll(&p, 1, (int)(until - now));
        wake_drain();
    } else if (!clock_wait(until))
        return 0; /* the engine has something first */
    if (at < 0 || now_ms() < at) return 0;
    char *line = next + strspn(next, "0123456789"), *end = line + strcspn(line, "\n");
    next = *end ? end + 1 : end;
    *end = 0;
    line += strspn(line, " ");
    memset(e, 0, sizeof *e);
    if (!strncmp(line, "shot ", 5) || !strncmp(line, "sink ", 5)) {
        e->kind = line[1] == 'h' ? 's' : 'f';
        snprintf(e->arg, sizeof e->arg, "%s", line + 5);
        return 1;
    }
    if (!strcmp(line, "term")) return raise(SIGTERM), 0;
    return event_parse(line, e);
}

static void script_show(const pix *page) {}

static void script_close(void) {
    free(script);
    script = next = 0;
}

void script_log(const char *line) {
    char path[PATH_N];
    snprintf(path, sizeof path, "%s/log", getenv("VIDEOS_OUT") ? getenv("VIDEOS_OUT") : ".");
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

void script_shot(const char *name, const pix *page) {
    char path[PATH_N];
    snprintf(path, sizeof path, "%s/%s.ppm", getenv("VIDEOS_OUT") ? getenv("VIDEOS_OUT") : ".",
             name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", VIEW_W, VIEW_H);
    for (int y = 0; y < VIEW_H; ++y)
        for (int x = 0; x < VIEW_W; ++x) {
            pix p = *view_at((pix *)page, x, y);
            fputc((int)(p >> 16 & 255), f), fputc((int)(p >> 8 & 255), f), fputc((int)(p & 255), f);
        }
    fclose(f);
}

const plat_t plat_script = { script_open, script_wait, script_show, script_close };
