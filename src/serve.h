/* What only the page's server needs: Q2SPOT/1 (serve.c), and the worker that probes the videos and
 * makes their thumbnails (thumb.c). */
#ifndef SERVE_H
#define SERVE_H
#include "videos.h"

/* ---- serve.c ------------------------------------------------------------------------------- */

#define REQ_MAX (8 << 10) /* Q2SPOT/1's limits: a request's bytes, a reply's, a reply's rows */
#define REPLY_MAX (64 << 10)
#define ROWS_MAX 50
#define TEXT_MAX 255 /* a title's or a subtitle's bytes */
/* A level of this many rows at most says "refresh" and is asked again every 2 s while it is
 * shown; a longer one says "pages" and is fetched once, by cursor. */
#define LIVE_ROWS 48
/* A row's context at its longest, escaped. Q2 Pod keeps a level's arguments in 400 bytes
 * (addon.c lv_t.args): a tab, the context escaped, a NUL, and for a folder paged by cursor a tab
 * and the cursor between them, 7 digits here. A press on a row with more does nothing there. */
#define CTX_MAX 390

/* s with %, tab, LF and CR as %25, %09, %0A and %0D into out (room bytes, no NUL added), first
 * cut to max bytes on a character's boundary. Its length, -1 when it doesn't fit. */
int q2_escape(const char *s, char *out, size_t room, size_t max);
/* s's escapes undone in place. 0 when one is malformed (a % without two hex digits after it) or
 * stands for a NUL, which would cut the field. */
int q2_unescape(char *s);
/* The request in req's n bytes answered into out (REPLY_MAX bytes): the reply's length. */
size_t serve_reply(const char *req, size_t n, char *out);

/* ---- thumb.c ------------------------------------------------------------------------------- */

#define THUMB_PX 128           /* a thumbnail's sides: the most Q2 Pod takes for a row's cover */
#define THUMB_BYTES (64 << 10) /* and its bytes at most: one at q 3 is about 6 kB */
#define THUMB_MS 5000          /* a video's probe and picture may take this long, then are let go */

/* What the worker found out about a video: a line of the cache's index. */
enum { T_PICTURE = 'p', T_NONE = 'x', T_LATER = '~', T_TRYING = '?' };
typedef struct {
    unsigned long long key; /* cache_key(): its path, size and time of change */
    long long dur_ms;       /* its length, 0 unknown */
    int w, h;               /* its picture's size, 0 unknown */
    char state; /* T_PICTURE: the cache has its thumbnail; T_NONE: it has none to show (the
                   "no picture" tile); T_LATER: none for now, for a cause that is not the
                   file's and may pass (the same tile, in memory alone); T_TRYING, from the
                   worker alone: it is on this one */
} thumb_t;

/* The index, state_dir/cache/index, a line a video: "key16<TAB>dur_ms<TAB>WxH<TAB>state". In
 * memory it is one process's: the server's copy takes what the worker tells it (index_put), the
 * worker's own, which starts as the server's (it is forked from it), is the one saved. A
 * T_LATER line is never saved: it keeps the video from being tried again while this server
 * runs, and the next one tries it once more. Garbage and torn lines are skipped. */
void index_load(void);
const thumb_t *index_find(unsigned long long key);
void index_put(const thumb_t *t);
void index_free(void);
/* state_dir/cache/<key16>.jpg into path (PATH_N bytes), which is returned. */
const char *thumb_file(unsigned long long key, char *path);

/* The video at path probed, and its thumbnail made: a frame from 15% of its length (from 30%
 * when that one is near black), its middle square at THUMB_PX, turned as the video is shown, as
 * a JPEG in jpg (THUMB_BYTES of room), *n bytes. 1 when there is one; *t then says T_PICTURE,
 * else T_NONE, or T_LATER when what kept it from one was too little memory free or its time
 * running out, with as much of the length and size as was read. Never more than ms. */
int thumb_make(const char *path, int ms, thumb_t *t, unsigned char *jpg, size_t *n);
/* One round of the worker's: the cache tidied, then every video without a line in the index
 * probed (thumb_make), level's own first, each saved to the cache and told on out as it is done.
 * in says which level is shown now (level is updated) and ends the round when it is another.
 * 0 when the round is through, 1 when another is due (another level), -1 when the server is
 * gone. in and out may be -1: the checks run a round in their own process. */
int worker_pass(char *level, int in, int out);
/* The worker, a process of its own (the server's child): rounds until one is through, at nice
 * 19. Its exit status is 0, or WORKER_NO_AV when the FFmpeg loaded is not the one built for. */
#define WORKER_NO_AV 3
void worker_main(const char *level, int in, int out) __attribute__((noreturn));

#endif
