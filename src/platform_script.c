/* The player without a Q2: a script of timed events in place of Q2 Pod's input socket, no screen,
 * and frames written as files when the script asks. In both builds, so the Q2's own binary can be
 * run the same way under an emulator. $VIDEOS_SCRIPT names the script, a line an event:
 *
 *     <ms> k d 171        one of Q2 Pod's datagrams (k, t, w, v), <ms> after the start
 *     <ms> shot <name>    the frame on the screen, to $VIDEOS_OUT/<name>.ppm, and a line that says
 *                         where the player is, to $VIDEOS_OUT/log
 *     <ms> sink <fault>   the sound output fails: stall, lost; back mends it
 *     <ms> term           SIGTERM, as Q2 Pod's held Return sends it
 *     <ms> firmware 41 60 [bt|usb]
 *                         from here on Q2 Pod's side of the volume file is played too (fw, below):
 *                         the volume at 41, the user's Maximum volume 60, the output
 *     <ms> outside 30     the volume changed by other means (a headset's keys)
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

/* Q2 Pod's side of the volume file, as its docs/librespot.md has it: on each pass of its UI the
 * file's text is acted on once (the same text again does nothing until the file has held another
 * or been gone), a level set no higher than the Maximum volume and "?" asked, and either is
 * answered with a v datagram: the level as it now is, and on Bluetooth the gain to apply, on a
 * USB DAC the level to show. A change by other means is told the same way and the file removed. */
static struct {
    int on, level, max;
    char kind, last[8]; /* b Bluetooth, u USB, else the jack; the text last acted on */
} fw;

static void fw_path(char *path, size_t n) { snprintf(path, n, "%s/q2-videos.volume", tmp_dir); }

static int fw_says(event_t *e) {
    char msg[32];
    int n = snprintf(msg, sizeof msg, "v %d", fw.level);
    if (fw.kind == 'b') snprintf(msg + n, sizeof msg - (size_t)n, " %d", fw.level);
    if (fw.kind == 'u') snprintf(msg + n, sizeof msg - (size_t)n, " h%d", fw.level);
    return event_parse(msg, e);
}

static int fw_pass(event_t *e) { /* 1 with its answer in e when the file asks for one */
    char path[PATH_N], text[8] = "";
    fw_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    int there = f != 0;
    if (f) {
        text[fread(text, 1, 6, f)] = 0; /* six bytes are read, a line end after the text or none */
        fclose(f);
        text[strcspn(text, "\n")] = 0;
    }
    if (!strcmp(text, fw.last)) return 0;
    snprintf(fw.last, sizeof fw.last, "%s", text);
    if (!there) return 0;
    if (strcmp(text, "?")) {
        if (!*text || text[strspn(text, "0123456789")]) return 0; /* neither: never answered */
        fw.level = atoi(text) > fw.max ? fw.max : atoi(text);
    }
    return fw_says(e);
}

static int script_wait(int ms, event_t *e) {
    if (fw.on && fw_pass(e)) return 1;
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
    if (sscanf(line, "firmware %d %d %c", &fw.level, &fw.max, &fw.kind) >= 2) return fw.on = 1, 0;
    if (fw.on && sscanf(line, "outside %d", &fw.level) == 1) {
        char path[PATH_N];
        fw_path(path, sizeof path);
        unlink(path);
        fw.last[0] = 0;
        return fw_says(e);
    }
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
