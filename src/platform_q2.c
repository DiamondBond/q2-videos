/* The Q2: its screen and Q2 Pod's input socket, as a card game has them (q2-pod docs/librespot.md,
 * game=1; q2-groovebox's platform_q2.c is the same in another app).
 *
 * The screen is /dev/fb0: the portrait panel, 320 x 375, 32 bits a pixel with red at bit 16, two
 * pages one above the other. A frame is copied into the page not on show and the display panned
 * to it, so no frame is seen half drawn; at the end it is panned back to the page Q2 Pod was
 * showing, which Q2 Pod draws on again. A framebuffer that says anything else is not drawn on.
 *
 * Input is datagrams on a socket this binds, tmp_dir/q2-videos.input.sock (player.c's
 * event_parse reads them). */
#include "player.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fb = -1, sock = -1;
static struct fb_var_screeninfo var;
static pix *mem;
static unsigned line, first, back; /* a row's pixels; the page Q2 Pod showed; the one not on show */
static size_t mem_len;
static struct sockaddr_un addr = { .sun_family = AF_UNIX };

static int q2_open(void) {
    struct fb_fix_screeninfo fix;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
        return 0;
    line = fix.line_length / 4, first = var.yoffset, back = first ? 0 : PAGE_H;
    if (var.xres != PAGE_W || var.yres != PAGE_H || var.yres_virtual < 2 * PAGE_H ||
        var.bits_per_pixel != 32 || var.red.offset != 16 || line < PAGE_W ||
        2 * PAGE_H * line * 4 > fix.smem_len)
        return 0;
    mem_len = fix.smem_len;
    mem = mmap(0, mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) return mem = 0, 0;

    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/q2-videos.input.sock", tmp_dir);
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    unlink(addr.sun_path);
    if (sock >= 0 && bind(sock, (struct sockaddr *)&addr, sizeof addr)) close(sock), sock = -1;
    return 1; /* without the socket there is still Q2 Pod's held Return to end it */
}

static int q2_wait(int ms, event_t *e) {
    struct pollfd p[2] = { { wake_fd(), POLLIN, 0 }, { sock, POLLIN, 0 } };
    char msg[64];
    if (poll(p, sock >= 0 ? 2 : 1, ms) <= 0) return 0;
    if (p[0].revents) wake_drain();
    long n = sock >= 0 && p[1].revents ? recv(sock, msg, sizeof msg - 1, MSG_DONTWAIT) : 0;
    if (n <= 0) return 0;
    msg[n] = 0;
    return event_parse(msg, e);
}

static void q2_show(const pix *page) {
    pix *to = mem + back * line;
    for (int y = 0; y < PAGE_H; ++y) memcpy(to + y * line, page + y * PAGE_W, PAGE_W * sizeof(pix));
    var.yoffset = back;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    back = back ? 0 : PAGE_H;
}

static void q2_close(void) {
    if (mem && var.yoffset != first) var.yoffset = first, ioctl(fb, FBIOPAN_DISPLAY, &var);
    if (mem) munmap(mem, mem_len);
    if (fb >= 0) close(fb);
    if (sock >= 0) close(sock), unlink(addr.sun_path);
    mem = 0, fb = sock = -1;
}

const plat_t plat_q2 = { q2_open, q2_wait, q2_show, q2_close };
