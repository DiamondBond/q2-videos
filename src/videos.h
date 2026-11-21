/* Videos, a Q2 Pod card app (README.md): one binary that is the page's server ("q2-videos serve",
 * started by run) and the full-screen player Q2 Pod runs for a video's row. This is what the two
 * share: where the files are, the settings and the marks on the card, and the Videos folder.
 * What only one of them needs stays in a header of its own. */
#ifndef VIDEOS_H
#define VIDEOS_H
#include <stddef.h>

/* VERSION, the app's ("1.0.0"), is the Makefile's, from the file VERSION. */
#define PATH_N 1024 /* a buffer for any path here */
/* A row's context at its longest: a video's or a folder's path, relative to the Videos folder. */
#define REL_MAX 400

/* ---- main.c, and the two roles ------------------------------------------------------------- */

/* The page's server: answers Q2SPOT/1 on tmp_dir/q2-videos.browse.sock until TERM or ten idle
 * minutes. Its exit status is the process's: 0 ends run's loop, anything else restarts it. */
int serve_main(void);
/* The player, for the row in $Q2_URI and $Q2_CONTEXT: argv is Q2 Pod's, DEVICE [VOLUME]. */
int player_main(int argc, char **argv);

/* ---- common.c ------------------------------------------------------------------------------ */

/* Where things are. On the Q2 the card is /mnt/mmc and Q2 Pod's files are in /tmp; $VIDEOS_CARD
 * and $VIDEOS_TMP move them (the checks). main calls paths_init() before either role starts. */
extern char card_dir[256];  /* the card */
extern char state_dir[272]; /* card_dir/.videos, the app's folder: settings, marks, cache/ */
extern char tmp_dir[256];   /* q2-videos.lock, q2-videos.art/, the sockets, q2-addon.caps */
void paths_init(void);

/* path's bytes, malloc'd and NUL-terminated, *n their count when n is given. 0 when it can't be
 * read or is over 8 MiB. Reads /proc files too. */
char *file_get(const char *path, size_t *n);
/* path written whole: as path.new, synced, then renamed over path, so a card pulled mid-write
 * keeps the old file. 1 when it was. */
int file_put(const char *path, const void *data, size_t n);

/* The lock on tmp_dir/q2-videos.lock that keeps the server's decoding off the Q2 while a video
 * plays. The player takes it exclusive, waiting for a thumbnail in the making, and holds it
 * until it exits. The server's worker takes it shared around each file and never waits: -1 says
 * the player runs, which is also how the server knows not to end itself. Returns the fd that
 * holds it (close() lets go), or -1. */
int lock_take(int exclusive);

/* The monotonic clock, ms. */
long long mono_ms(void);

/* /proc/meminfo's MemAvailable, kB; 0 unknown. */
unsigned mem_available(void);
/* 1 when decoding a w x h picture would hold more than avail_kb in frames alone (10 of them at
 * 12 bits a pixel): the Q2 has 128 MB and no swap, and stalls until the kernel kills us. An
 * unknown size or avail_kb (0) is never too large. */
int too_large(int w, int h, unsigned avail_kb);

/* The video at path opened, for the thumbnails and the player alike: as one of FORMATS or not at
 * all, and nothing but files for whatever it names in turn, so a playlist or a script named .mp4
 * is never followed; "file:" so that no name reads as a protocol's. libavformat asks stop(arg)
 * wherever it reads or waits and lets go when that says 1. 0 when it can't be opened, and
 * libavformat's error in *err when err is given. */
struct AVFormatContext;
struct AVStream;
struct AVFormatContext *input_open(const char *path, int (*stop)(void *), void *arg, int *err);
/* The turn a stream's container asks for, clockwise degrees, 0 to 359: a phone's video held
 * upright is stored on its side with a rotate tag (FFmpeg 4.2's mov reader sets the tag whenever
 * the display matrix is a rotation, and no other reader makes the matrix). */
int rotation(struct AVStream *st);

/* ---- store.c: the app's state on the card, in state_dir ------------------------------------ */
/* Nothing in store.c or library.c locks: a settings_t, marks_t or listing_t is one thread's. */

enum { SORT_NAME, SORT_NEWEST };
typedef struct {
    int next;    /* Play Next: 0 Off, 1 On */
    int subs;    /* Subtitles: 0 Off, 1 On */
    int subsize; /* Subtitle Size: 0 Small, 1 Medium, 2 Large */
    int fill;    /* Fit to Screen: 0 Off (all of the picture), 1 On (the screen filled) */
    int sort;    /* Sort: SORT_NAME By Name, SORT_NEWEST Newest First */
} settings_t;

/* The file "settings", key=value lines (next, subs, subsize, fill, sort; the values as above).
 * A missing file, key or a value out of range leaves the default: 0, 1, 1, 0, 0. */
void settings_load(settings_t *s);
/* 1 when it was written (file_put). */
int settings_save(const settings_t *s);
/* The setting named key stepped to its next value, the last wrapping to the first. 0 when there
 * is no such key. */
int settings_step(settings_t *s, const char *key);

#define MARKS_MAX 500
#define MARK_WATCHED 1u
typedef struct {
    long long pos_ms, dur_ms; /* the place, and the video's length (0 unknown) */
    long long size;           /* the file's bytes when marked: another size is another video */
    unsigned flags;           /* MARK_WATCHED */
    unsigned seq;             /* orders the marks, the highest the most recent: no clock needed */
    int audio, sub;           /* the tracks last chosen for it, -1 the default */
    char *rel;                /* its path, relative to the Videos folder */
} mark_t;
typedef struct { /* 24 kB: static, not a thread's stack */
    mark_t v[MARKS_MAX];
    int n;
    unsigned seq; /* the highest given so far */
} marks_t;

/* The file "marks", a line a video: pos_ms, dur_ms, flags, seq, audio, sub, size and the path,
 * tabs between. *m must be zeroed before its first load; a load replaces what it held. Torn and
 * garbage lines are skipped, and of two lines for one path the higher seq counts. The player and
 * the server each load before they use the marks: the other may have written. */
void marks_load(marks_t *m);
/* 1 when they were written (file_put). */
int marks_save(const marks_t *m);
/* The mark of the video at rel whose file is size bytes, or 0. A mark of another size is dropped
 * here. The pointer holds until the next marks_put, marks_drop or marks_load. */
mark_t *marks_find(marks_t *m, const char *rel, long long size);
/* The same, made when there is none (at the start, default tracks; the oldest mark goes when
 * there are MARKS_MAX), and stamped the most recent. 0 when rel is no path of ours (rel_ok). */
mark_t *marks_put(marks_t *m, const char *rel, long long size);
void marks_drop(marks_t *m, mark_t *k);
void marks_free(marks_t *m);
/* In progress: begun and not watched. The list says "23 min left" and Continue Watching has it. */
static inline int mark_unfinished(const mark_t *k) {
    return k && !(k->flags & MARK_WATCHED) && k->pos_ms > 0;
}

/* ---- library.c: the Videos folder ---------------------------------------------------------- */

typedef struct {
    char *name;      /* its name in the folder (lib_list), or its path from the top (lib_all) */
    int dir;         /* a folder; else a video */
    int count;       /* a folder: the videos in it, through its subfolders */
    long long size;  /* a video: its bytes */
    long long mtime; /* a video: when it was last changed, seconds */
} entry_t;
typedef struct {
    entry_t *v;  /* the folders, then the videos */
    int n, dirs; /* dirs: how many of them are folders */
    int room;    /* library.c's: entries v has room for */
} listing_t;

/* A video by its name's extension, case aside: mp4 m4v mkv webm avi mov mpg mpeg wmv flv ts m2ts
 * 3gp ogv. */
int is_video(const char *name);
/* The containers those extensions name, as libavformat calls their readers. The thumbnails and
 * the player open a file as one of these or not at all (format_whitelist): a playlist or a script
 * named .mp4 would have libavformat open whatever it lists. */
#define FORMATS "mov,matroska,avi,mpegts,mpeg,flv,asf,ogg,mpegvideo,m4v"
/* Natural order, case aside: digits count as numbers, so "Episode 2" is before "Episode 10". */
int natcmp(const char *a, const char *b);
/* 1 when rel is a path this app could have made: at most REL_MAX bytes, every part of it a name
 * that doesn't start with a dot (so no ".." either), no control bytes. A request's context is
 * checked with it before it reaches the card. */
int rel_ok(const char *rel);

/* The Videos folder into root: the card's top-level folder named Videos, case aside, as the card
 * spells it ("/mnt/mmc/videos"). 0 when there is none. It can appear while the server runs, so
 * ask again for each request. */
int lib_root(char *root, size_t n);
/* root and rel joined into out ("" is root itself). 0 when rel is not rel_ok or out is short. */
int lib_path(const char *root, const char *rel, char *out, size_t n);
/* The folder at rel ("" the top): its subfolders that hold a video somewhere inside, in natural
 * order, each with its count; then its videos, in natural order or (SORT_NEWEST) the newest
 * first. Dot entries, and paths over REL_MAX, are left out and not counted. 0 when the folder
 * can't be read; lib_free() l either way. */
int lib_list(const char *root, const char *rel, int sort, listing_t *l);
/* Every video under root, each named by its path from there, in natural order. 0 when root can't
 * be read (never an empty library: the caller mustn't take a pulled card for one). */
int lib_all(const char *root, listing_t *l);
void lib_free(listing_t *l);
/* A video's title into out: its file name less the extension. path may be relative. */
const char *lib_title(const char *path, char *out, size_t n);
/* Play Next: the video listed after the one at rel in its folder (sort as the list's), its path
 * into out. 0 when rel was the last, or is gone. */
int lib_next(const char *root, const char *rel, int sort, char *out, size_t n);
/* Continue Watching: the unfinished mark played last whose video is still on the card with the
 * size marked, or 0. */
const mark_t *lib_continue(const char *root, const marks_t *m);

/* A row's subtitle texts, into out, which is returned. A length: "1 h 42 min", "2 h", "24 min",
 * "45 sec"; "" when it is unknown (0). What is left: "23 min left", "1 h 5 min left", "Less than
 * a minute left". A size: "1.2 GB", "350 MB", "12 KB". */
const char *text_length(long long ms, char *out, size_t n);
const char *text_left(long long pos_ms, long long dur_ms, char *out, size_t n);
const char *text_size(long long bytes, char *out, size_t n);

/* FNV-1a over n bytes, from h: FNV32 or FNV64 to start, or an earlier result to go on. */
#define FNV32 2166136261u
#define FNV64 14695981039346656037ull
unsigned fnv32(unsigned h, const void *data, size_t n);
unsigned long long fnv64(unsigned long long h, const void *data, size_t n);
/* A row's cover is tmp_dir/q2-videos.art/<key>.jpg, the key as %08x: FNV-1a 32 of the row's uri,
 * a tab and its context, as Q2 Pod hashes them. */
unsigned art_key(const char *uri, const char *context);
/* A video's cache entry is state_dir/cache/<key>.jpg and its index line, the key as %016llx:
 * FNV-1a 64 of "rel<TAB>size<TAB>mtime", the numbers in decimal. A changed file is a new key. */
unsigned long long cache_key(const char *rel, long long size, long long mtime);

#endif
