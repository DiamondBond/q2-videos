/* What the check programs share: a check that says where it failed, a shell command, and files
 * made for a card in build/. */
#ifndef CHECK_H
#define CHECK_H
#include "videos.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) ((c) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c), abort()))
#define SAME(text, want)                                                                           \
    (!strcmp(text, want)                                                                           \
         ? (void)0                                                                                 \
         : (fprintf(stderr, "%s:%d: \"%s\" is not \"%s\"\n", __FILE__, __LINE__, text, want),      \
            abort()))

__attribute__((format(printf, 1, 2))) static inline void sh(const char *fmt, ...) {
    char cmd[4 * PATH_N];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd)) fprintf(stderr, "failed: %s\n", cmd), abort();
}
/* a file of size bytes (a hole: nothing is written), its folders made, changed at mtime */
static inline void file(const char *path, long long size, long long mtime) {
    sh("mkdir -p \"$(dirname '%s')\"", path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct timespec t[2] = { { mtime, 0 }, { mtime, 0 } };
    CHECK(fd >= 0 && !ftruncate(fd, size) && !futimens(fd, t) && !close(fd));
}
static inline void text(const char *path, const char *s) { CHECK(file_put(path, s, strlen(s))); }

#endif
