/* What is done to the sound on its way out, ported from Q2 Pod's own video helper (q2-pod
 * patch/video.c): the soft volume Bluetooth and a plain USB DAC get, Mono Audio and Swap L/R,
 * and which ALSA card the USB DAC is. The outputs themselves are sound_q2.c (ALSA) and the
 * engine's silent one. */
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* hciplayer's Bluetooth soft volume (its mixer_setvolume) for Q2 Pod's volume v, in 1/65536:
 * 0.002 v under 50, then 0.1 + 0.018 (v - 50), 1 at 100. */
int bt_gain(int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    return 65536 * (v < 50 ? 2 * v : 18 * v - 800) / 1000;
}

int arg_gain(const char *arg) {
    if (!arg || *arg < '0' || *arg > '9') return 65536; /* none (the jack), or hNN: the DAC's */
    return bt_gain(atoi(arg));
}

int arg_level(const char *arg) {
    if (arg && *arg == 'h') ++arg;
    if (!arg || *arg < '0' || *arg > '9') return -1;
    int v = atoi(arg);
    return v > 100 ? 100 : v;
}

void scale(short *s, unsigned n, int g) { /* g is at most 65536, so no sample overflows */
    for (unsigned i = 0; i < n; ++i) s[i] = (short)(s[i] * g >> 16);
}

void mono(short *s, unsigned n) {
    for (unsigned i = 0; i < 2 * n; i += 2) s[i] = s[i + 1] = (short)((s[i] + s[i + 1]) >> 1);
}

void swap_lr(short *s, unsigned n) {
    for (unsigned i = 0; i < 2 * n; i += 2) {
        short left = s[i];
        s[i] = s[i + 1], s[i + 1] = left;
    }
}

/* The card /proc/asound/cards lists as "N [id]: USB-Audio - name": 1 behind the headphone DAC's
 * 0 when the list doesn't say. */
const char *usb_device(const char *cards, char *out) {
    const char *u = cards ? strstr(cards, ": USB-Audio - ") : 0;
    if (!u) return strcpy(out, "plughw:1,0");
    while (u > cards && u[-1] != '\n') --u;
    snprintf(out, 16, "plughw:%d,0", atoi(u));
    return out;
}
