/* Videos' host checks of what the server and the player share (make check, under ASan and UBSan):
 * whole files and the lock, the settings and the marks, the names and their order, the rows'
 * texts and keys, and a library made in build/check-card. */
#include "check.h"

#define CARD "build/check-card"
#define TMP "build/check-tmp"
/* ---- Files, the lock, memory --------------------------------------------------------------- */

static void check_common(void) {
    SAME(card_dir, "/mnt/mmc"), SAME(state_dir, "/mnt/mmc/.videos"), SAME(tmp_dir, "/tmp");
    setenv("VIDEOS_CARD", CARD, 1), setenv("VIDEOS_TMP", TMP, 1);
    paths_init();
    SAME(card_dir, CARD), SAME(state_dir, CARD "/.videos"), SAME(tmp_dir, TMP);

    size_t n = 99;
    char big[100000], *got;
    for (size_t i = 0; i < sizeof big; ++i) big[i] = (char)(i * 31 >> 3); /* zeros among them */
    CHECK(file_put(TMP "/blob", big, sizeof big) && access(TMP "/blob.new", 0));
    CHECK((got = file_get(TMP "/blob", &n)) && n == sizeof big && !memcmp(got, big, n) && !got[n]);
    free(got);
    CHECK(file_put(TMP "/blob", "", 0) && (got = file_get(TMP "/blob", &n)) && !n && !*got);
    free(got);
    CHECK(!file_get(TMP "/none", &n) && !file_get(TMP, 0)); /* missing; a folder */
    CHECK(!file_put(TMP "/no/such/file", "x", 1));          /* nowhere to write */
    CHECK((got = file_get("/proc/meminfo", &n)) && n > 0);  /* a file that says 0 bytes */
    free(got);
    CHECK(mem_available() > 0);

    int held = lock_take(1); /* the player's */
    CHECK(held >= 0 && lock_take(0) < 0);
    close(held);
    int a = lock_take(0), b = lock_take(0); /* the worker's, and the server's asking */
    CHECK(a >= 0 && b >= 0);
    close(a), close(b);

    CHECK(too_large(1920, 1080, 30374) && !too_large(1920, 1080, 30375)); /* 10 frames, 12 bpp */
    CHECK(!too_large(640, 360, 3375) && too_large(640, 360, 3374));
    CHECK(!too_large(0, 1080, 1) && !too_large(1920, -1, 1) && !too_large(7680, 4320, 0));
    CHECK(too_large(32768, 32768, 4000000u)); /* no overflow on the way */
}

/* ---- Settings ------------------------------------------------------------------------------ */

static void check_settings(void) {
    settings_t s = { 9, 9, 9, 9, 9 }, t;
    settings_load(&s); /* no file: the defaults */
    CHECK(!s.next && s.subs == 1 && s.subsize == 1 && !s.fill && s.sort == SORT_NAME);
    CHECK(settings_save(&s));
    char *got = file_get(CARD "/.videos/settings", 0);
    SAME(got, "next=0\nsubs=1\nsubsize=1\nfill=0\nsort=0\n");
    free(got);

    CHECK(settings_step(&s, "next") && s.next == 1 && settings_step(&s, "subs") && !s.subs);
    CHECK(settings_step(&s, "subsize") && s.subsize == 2 && settings_step(&s, "subsize"));
    CHECK(s.subsize == 0 && settings_step(&s, "subsize") && s.subsize == 1); /* round again */
    CHECK(settings_step(&s, "fill") && s.fill == 1 && settings_step(&s, "fill") && !s.fill);
    CHECK(settings_step(&s, "sort") && s.sort == SORT_NEWEST);
    CHECK(!settings_step(&s, "volume") && !settings_step(&s, "") && !settings_step(&s, "nex"));
    CHECK(settings_save(&s));
    settings_load(&t);
    CHECK(!memcmp(&s, &t, sizeof s) && access(CARD "/.videos/settings.new", 0));

    /* out of range, not a number, no value, another key, a CR: each line stands alone */
    text(CARD "/.videos/settings", "next=7\nsubs=-1\nsubsize=2\r\nfill=x\nsort=1 \nbogus=1\n=\n"
                                   "noequals\nnext=\nsort=1\n\n");
    settings_load(&t);
    CHECK(!t.next && t.subs == 1 && t.subsize == 2 && !t.fill && t.sort == SORT_NEWEST);
    text(CARD "/.videos/settings", "");
    settings_load(&t);
    CHECK(!t.next && t.subs == 1 && t.subsize == 1 && !t.fill && t.sort == SORT_NAME);
}

/* ---- Marks --------------------------------------------------------------------------------- */

static marks_t m; /* 24 kB: not the stack's */

static void check_marks(void) {
    marks_load(&m);
    CHECK(!m.n && !m.seq && !marks_find(&m, "Movies/Film.mkv", 734003200));

    mark_t *k = marks_put(&m, "Movies/Film.mkv", 734003200);
    CHECK(k && !k->pos_ms && !k->dur_ms && !k->flags && k->audio == -1 && k->sub == -1);
    CHECK(k->seq == 1 && k->size == 734003200 && !mark_unfinished(k) && !mark_unfinished(0));
    k->pos_ms = 61000, k->dur_ms = 5400000, k->flags = MARK_WATCHED, k->audio = 2;
    CHECK(!mark_unfinished(k)); /* watched, whatever the place */
    CHECK(marks_save(&m) && access(CARD "/.videos/marks.new", 0));
    char *got = file_get(CARD "/.videos/marks", 0);
    SAME(got, "61000\t5400000\t1\t1\t2\t-1\t734003200\tMovies/Film.mkv\n");
    free(got);

    /* a name that isn't ASCII, a file over 4 GiB */
    k = marks_put(&m, "Сериал/Серия 1.mkv", 5000000000ll);
    k->pos_ms = 1, k->sub = 3;
    CHECK(k->seq == 2 && mark_unfinished(k));
    CHECK((k = marks_put(&m, "Movies/Film.mkv", 734003200)) && k->seq == 3 && k->audio == 2);
    CHECK(m.n == 2 && marks_save(&m)); /* the same video: its mark again, now the most recent */
    marks_free(&m);
    CHECK(!m.n);
    marks_load(&m);
    CHECK(m.n == 2 && m.seq == 3);
    k = marks_find(&m, "Сериал/Серия 1.mkv", 5000000000ll);
    CHECK(k && k->pos_ms == 1 && !k->dur_ms && !k->flags && k->seq == 2 && k->audio == -1);
    CHECK(k->sub == 3 && k->size == 5000000000ll);
    k = marks_find(&m, "Movies/Film.mkv", 734003200);
    CHECK(k && k->pos_ms == 61000 && k->dur_ms == 5400000 && k->flags == MARK_WATCHED);
    CHECK(k->seq == 3 && k->audio == 2 && k->sub == -1);
    CHECK(!marks_find(&m, "movies/film.mkv", 734003200) && !marks_find(&m, 0, 0));

    /* another size is another video: the mark goes, for good once saved */
    CHECK(!marks_find(&m, "Movies/Film.mkv", 734003201) && m.n == 1);
    CHECK(!marks_find(&m, "Movies/Film.mkv", 734003200));
    CHECK((k = marks_put(&m, "Сериал/Серия 1.mkv", 7)) && !k->pos_ms && k->sub == -1 && m.n == 1);
    CHECK(k->seq == 4 && k->size == 7 && marks_save(&m));
    marks_load(&m);
    CHECK(m.n == 1 && m.seq == 4 && marks_find(&m, "Сериал/Серия 1.mkv", 7));

    /* paths that are none of ours get no mark */
    char longest[REL_MAX + 2];
    memset(longest, 'x', sizeof longest - 1), longest[REL_MAX + 1] = 0;
    CHECK(!marks_put(&m, "", 1) && !marks_put(&m, "../x.mp4", 1) && !marks_put(&m, "/x.mp4", 1));
    CHECK(!marks_put(&m, "a//b.mp4", 1) && !marks_put(&m, ".hid/x.mp4", 1) && !marks_put(&m, 0, 1));
    CHECK(!marks_put(&m, "a\nb.mp4", 1) && !marks_put(&m, "a/", 1) && !marks_put(&m, longest, 1));
    longest[REL_MAX] = 0;
    CHECK(m.n == 1 && marks_put(&m, longest, 1) && m.n == 2);
    marks_drop(&m, marks_find(&m, longest, 1));
    marks_drop(&m, marks_find(&m, "Сериал/Серия 1.mkv", 7));
    CHECK(!m.n && m.seq == 5);

    /* the cap: the oldest goes for a new one, a mark played again is not the oldest */
    char name[32];
    for (int i = 0; i < MARKS_MAX; ++i) {
        snprintf(name, sizeof name, "v/%d.mp4", i);
        CHECK((k = marks_put(&m, name, i)) && k->seq == 6u + (unsigned)i);
    }
    CHECK(m.n == MARKS_MAX && marks_put(&m, "v/0.mp4", 0) && m.n == MARKS_MAX);
    CHECK(marks_put(&m, "v/new.mp4", 1) && m.n == MARKS_MAX);
    CHECK(marks_find(&m, "v/0.mp4", 0) && !marks_find(&m, "v/1.mp4", 1));
    CHECK(marks_find(&m, "v/2.mp4", 2) && marks_find(&m, "v/499.mp4", 499));
    CHECK(marks_save(&m));
    marks_load(&m);
    CHECK(m.n == MARKS_MAX && m.seq == 507 && marks_find(&m, "v/new.mp4", 1)->seq == 507);

    /* a file by another hand: garbage and torn lines are passed over, the rest are read */
    text(CARD "/.videos/marks", "5\t9\t0\t3\t-1\t-1\t100\tgood.mp4\n"
                                "garbage\n"
                                "\n"
                                "5\t9\t0\t4\t-1\t-1\tsix fields.mp4\n"
                                "5\tx\t0\t4\t-1\t-1\t100\tnot a number.mp4\n"
                                "-5\t9\t0\t4\t-1\t-1\t100\tbefore the start.mp4\n"
                                " 5\t9\t0\t4\t-1\t-1\t100\ta blank first.mp4\n"
                                "5\t\t9\t0\t4\t-1\t-1\t100\tan empty field.mp4\n"
                                "5\t9\t0\t4\t-2\t-1\t100\tno such track.mp4\n"
                                "5\t9\t0\t4\t-1\t-1\t-100\tno such size.mp4\n"
                                "5\t9\t0\t99999999999\t-1\t-1\t100\tno such seq.mp4\n"
                                "5\t9\t0\t4\t-1\t-1\t100\t../../etc/passwd\n"
                                "5\t9\t0\t4\t-1\t-1\t100\t\n"
                                "5\t9\t0\t4\t-1\t-1\t100\tdos.mp4\r\n"
                                "7\t9\t1\t8\t0\t1\t100\ttwice.mp4\n"
                                "6\t9\t0\t2\t-1\t-1\t100\ttwice.mp4\n"
                                "1\t2\t0\t6\t-1\t-1\t3\tan eighth\tfield.mp4\n"
                                "5\t9\t0\t12\t-1\t-1\t100\ttorn.mp");
    marks_load(&m);
    CHECK(m.n == 2 && m.seq == 8);
    CHECK((k = marks_find(&m, "good.mp4", 100)) && k->pos_ms == 5 && k->dur_ms == 9 && k->seq == 3);
    CHECK((k = marks_find(&m, "twice.mp4", 100)) && k->pos_ms == 7 && k->seq == 8 && k->sub == 1);

    /* a file past the cap: the most recent stay */
    FILE *f = fopen(CARD "/.videos/marks", "w");
    for (int i = 600; i >= 1; --i) fprintf(f, "1\t2\t0\t%d\t-1\t-1\t%d\tv/%d.mp4\n", i, i, i);
    CHECK(!fclose(f));
    marks_load(&m);
    CHECK(m.n == MARKS_MAX && m.seq == 600 && marks_find(&m, "v/101.mp4", 101));
    CHECK(marks_find(&m, "v/600.mp4", 600) && !marks_find(&m, "v/100.mp4", 100));
    marks_free(&m);
    unlink(CARD "/.videos/marks");
}

/* ---- Names, texts and keys ----------------------------------------------------------------- */

static int by_nat(const void *a, const void *b) {
    return natcmp(*(const char *const *)a, *(const char *const *)b);
}

static void check_names(void) {
    static const char *const yes[] = { "a.mp4",   "A.MP4",    "x.MkV",  "clip.webm", "a.m4v",
                                       "a.avi",   "a.mov",    "a.mpg",  "a.mpeg",    "a.wmv",
                                       "a.flv",   "a.ts",     "a.m2ts", "a.3gp",     "a.ogv",
                                       "a.b.mkv", "dir/a.mp4" };
    static const char *const no[] = { "a.mp3", "a.srt", "mp4",     ".mp4", "a.mp4.txt", "a.",
                                      "a",     "",      "a.mp4/x", "a.mp", "a.mp44",    "amp4" };
    for (size_t i = 0; i < sizeof yes / sizeof *yes; ++i) CHECK(is_video(yes[i]));
    for (size_t i = 0; i < sizeof no / sizeof *no; ++i) CHECK(!is_video(no[i]));

    CHECK(natcmp("Episode 2", "Episode 10") < 0 && natcmp("Episode 10", "Episode 2") > 0);
    CHECK(natcmp("episode 2", "Episode 10") < 0 && natcmp("a", "B") < 0 && natcmp("B", "c") < 0);
    CHECK(natcmp("9", "10") < 0 && natcmp("x2", "x10") < 0 && natcmp("x10", "x10a") < 0);
    CHECK(natcmp("a", "a1") < 0 && natcmp("", "a") < 0 && natcmp("a1b", "a1c") < 0);
    CHECK(!natcmp("", "") && !natcmp("Same 1", "Same 1") && natcmp("a 2 b 3", "a 2 b 10") < 0);
    CHECK(natcmp("007", "7") < 0 && natcmp("7", "007") > 0); /* equal numbers: still an order */
    CHECK(natcmp("a", "A") > 0 && natcmp("A", "a") < 0);
    CHECK(natcmp("s99999999999999999999", "s100000000000000000000") < 0); /* past any integer */
    CHECK(natcmp("0", "00") < 0 && natcmp("a0", "a") > 0 && natcmp("1.mp4", "1a.mp4") < 0);
    const char *v[] = { "Episode 10.mkv", "episode 2.mkv", "Extras",    "Episode 1.mkv",
                        "episode 03.mkv", "2 Fast",        "10 Things", "Épisode" };
    qsort(v, sizeof v / sizeof *v, sizeof *v, by_nat);
    SAME(v[0], "2 Fast"), SAME(v[1], "10 Things"), SAME(v[2], "Episode 1.mkv");
    SAME(v[3], "episode 2.mkv"), SAME(v[4], "episode 03.mkv"), SAME(v[5], "Episode 10.mkv");
    SAME(v[6], "Extras"), SAME(v[7], "Épisode");

    CHECK(rel_ok("a.mp4") && rel_ok("Series/Season 1/a.mkv") && rel_ok("Сериал/Серия 1.mkv"));
    CHECK(rel_ok("a..b/c") && rel_ok("a b/c.d.e") && !rel_ok("") && !rel_ok(0) && !rel_ok("/"));
    CHECK(!rel_ok("..") && !rel_ok("../a") && !rel_ok("a/../b") && !rel_ok("a/..") && !rel_ok("."));
    CHECK(!rel_ok("/a") && !rel_ok("a/") && !rel_ok("a//b") && !rel_ok("a/.b") && !rel_ok(".a/b"));
    CHECK(!rel_ok("a\tb") && !rel_ok("a\n") && !rel_ok("a/b\x1f"));

    char t[64], tiny[5];
    SAME(lib_title("Movies/The Film (2019).mkv", t, sizeof t), "The Film (2019)");
    SAME(lib_title("noext", t, sizeof t), "noext");
    SAME(lib_title("a.b.c.mp4", t, sizeof t), "a.b.c");
    SAME(lib_title("/mnt/mmc/Videos/x.MP4", t, sizeof t), "x");
    SAME(lib_title("Season 1.5/e.mkv", t, sizeof t), "e");
    SAME(lib_title("Long Name.mkv", tiny, 5), "Long");

    SAME(text_length(0, t, sizeof t), ""), SAME(text_length(-5, t, sizeof t), "");
    SAME(text_length(400, t, sizeof t), "1 sec"), SAME(text_length(45000, t, sizeof t), "45 sec");
    SAME(text_length(59499, t, sizeof t), "59 sec"), SAME(text_length(59500, t, sizeof t), "1 min");
    SAME(text_length(89000, t, sizeof t), "1 min"), SAME(text_length(90000, t, sizeof t), "2 min");
    SAME(text_length(24 * 60000, t, sizeof t), "24 min");
    SAME(text_length(59 * 60000 + 29000, t, sizeof t), "59 min");
    SAME(text_length(59 * 60000 + 30000, t, sizeof t), "1 h"); /* never "60 min" */
    SAME(text_length(3600000, t, sizeof t), "1 h");
    SAME(text_length(3660000, t, sizeof t), "1 h 1 min");
    SAME(text_length(6120000, t, sizeof t), "1 h 42 min");
    SAME(text_length(7200000, t, sizeof t), "2 h");
    SAME(text_length(36000000ll * 3, t, sizeof t), "30 h");

    SAME(text_left(600000, 1980000, t, sizeof t), "23 min left");
    SAME(text_left(0, 3900000, t, sizeof t), "1 h 5 min left");
    SAME(text_left(0, 60000, t, sizeof t), "1 min left");
    SAME(text_left(1, 60000, t, sizeof t), "Less than a minute left");
    SAME(text_left(5000, 5000, t, sizeof t), "Less than a minute left");
    SAME(text_left(9000, 5000, t, sizeof t), "Less than a minute left"); /* past the end */
    SAME(text_left(5000, 0, t, sizeof t), "Less than a minute left");    /* no length known */

    const long long KB = 1024, MB = KB * KB, GB = MB * KB;
    SAME(text_size(0, t, sizeof t), "0 KB"), SAME(text_size(-1, t, sizeof t), "0 KB");
    SAME(text_size(1023, t, sizeof t), "1 KB"), SAME(text_size(12 * KB, t, sizeof t), "12 KB");
    SAME(text_size(999 * KB, t, sizeof t), "999 KB");
    SAME(text_size(1000 * KB, t, sizeof t), "1 MB");
    SAME(text_size(350 * MB, t, sizeof t), "350 MB");
    SAME(text_size(999 * MB, t, sizeof t), "999 MB");
    SAME(text_size(1000 * MB, t, sizeof t), "1.0 GB"), SAME(text_size(GB, t, sizeof t), "1.0 GB");
    SAME(text_size(GB + GB / 5, t, sizeof t), "1.2 GB");
    SAME(text_size(5 * GB, t, sizeof t), "5.0 GB");
    SAME(text_size(64 * GB - 1, t, sizeof t), "64.0 GB");

    CHECK(fnv32(FNV32, "a", 1) == 0xe40c292c && fnv32(FNV32, "", 0) == 0x811c9dc5);
    CHECK(fnv32(FNV32, "foobar", 6) == 0xbf9cf968);
    CHECK(fnv32(fnv32(FNV32, "foo", 3), "bar", 3) == 0xbf9cf968); /* goes on from a result */
    CHECK(fnv64(FNV64, "a", 1) == 0xaf63dc4c8601ec8cull && fnv64(FNV64, "", 0) == FNV64);
    CHECK(fnv64(FNV64, "foobar", 6) == 0x85944171f73967e8ull);
    CHECK(art_key("play", "Movies/Film.mp4") == 0xd8a9498b);
    CHECK(art_key("dir", "Series/Season 1") == 0x2b99f1a9 && art_key("settings", "") == 0xc333a293);
    CHECK(cache_key("Movies/Film.mp4", 1234567890, 1700000000) == 0x10857ad255096f2full);
    CHECK(cache_key("Сериал/Серия 1.mkv", 5000000000ll, 0) == 0x0507fab0f1046577ull);
    CHECK(cache_key("a.mp4", 1, 2) != cache_key("a.mp4", 2, 1)); /* a changed file: a new key */
}

/* ---- The library --------------------------------------------------------------------------- */

static void check_library(void) {
    char root[PATH_N], p[PATH_N], small[8];
    listing_t l;
    CHECK(!lib_root(root, sizeof root)); /* a card without the folder */
    file(CARD "/videos", 1, 0);
    CHECK(!lib_root(root, sizeof root)); /* a file of that name is not it */
    unlink(CARD "/videos");

#define V CARD "/VIDEOS/"
    file(V "Episode 10.mkv", 10, 1000), file(V "episode 2.mkv", 20, 3000);
    file(V "Episode 1.MP4", 30, 2000), file(V "notes.txt", 1, 0), file(V ".hidden.mp4", 1, 0);
    file(V ".trash/secret.mp4", 1, 0), file(V "Docs/readme.txt", 1, 0);
    file(V "Series/Season 1/a.mkv", 1, 0), file(V "Series/Season 1/b.avi", 2, 0);
    file(V "Series/Season 2/c.webm", 3, 0), file(V "Series/Season 2/.thumbs/x.mp4", 1, 0);
    file(V "Series/poster.jpg", 1, 0), file(V "Movies/Film.mkv", 5ll << 30, 4000);
    file(V "Movies/Film.srt", 1, 0), file(V "Deep/a/b/c/d.mov", 4, 0);
    file(CARD "/Other/x.mp4", 1, 0);
    sh("mkdir -p '" V "Empty/Emptier' '" V "movie.mp4'"); /* and a folder named as a video */

    CHECK(lib_root(root, sizeof root) && !lib_root(small, sizeof small));
    SAME(root, CARD "/VIDEOS"); /* as the card spells it */
    CHECK(lib_path(root, "Movies/Film.mkv", p, sizeof p)), SAME(p, V "Movies/Film.mkv");
    CHECK(lib_path(root, "", p, sizeof p)), SAME(p, CARD "/VIDEOS");
    CHECK(!lib_path(root, "../Other/x.mp4", p, sizeof p) && !lib_path(root, 0, p, sizeof p));
    CHECK(!lib_path(root, "/etc/passwd", p, sizeof p) && !lib_path(root, "a.mp4", small, 8));

    /* the top: the folders that hold a video, with their counts, then the videos */
    CHECK(lib_list(root, "", SORT_NAME, &l) && l.n == 6 && l.dirs == 3);
    SAME(l.v[0].name, "Deep"), SAME(l.v[1].name, "Movies"), SAME(l.v[2].name, "Series");
    CHECK(l.v[0].dir && l.v[0].count == 1 && l.v[1].count == 1 && l.v[2].count == 3);
    SAME(l.v[3].name, "Episode 1.MP4"), SAME(l.v[4].name, "episode 2.mkv");
    SAME(l.v[5].name, "Episode 10.mkv");
    CHECK(!l.v[3].dir && l.v[3].size == 30 && l.v[3].mtime == 2000 && l.v[5].size == 10);
    lib_free(&l);
    CHECK(!l.v && !l.n);
    CHECK(lib_list(root, "", SORT_NEWEST, &l) && l.n == 6 && l.dirs == 3);
    SAME(l.v[0].name, "Deep"), SAME(l.v[2].name, "Series"); /* folders keep their order */
    SAME(l.v[3].name, "episode 2.mkv"), SAME(l.v[4].name, "Episode 1.MP4");
    SAME(l.v[5].name, "Episode 10.mkv");
    lib_free(&l);

    CHECK(lib_list(root, "Series", SORT_NAME, &l) && l.n == 2 && l.dirs == 2);
    SAME(l.v[0].name, "Season 1"), SAME(l.v[1].name, "Season 2");
    CHECK(l.v[0].count == 2 && l.v[1].count == 1); /* the dot folder's video is not counted */
    lib_free(&l);
    CHECK(lib_list(root, "Series/Season 2", SORT_NAME, &l) && l.n == 1 && !l.dirs);
    SAME(l.v[0].name, "c.webm");
    lib_free(&l);
    CHECK(lib_list(root, "Movies", SORT_NAME, &l) && l.n == 1 && l.v[0].size == 5ll << 30);
    CHECK(l.v[0].mtime == 4000); /* over 4 GiB, whole */
    lib_free(&l);
    CHECK(lib_list(root, "Empty", SORT_NAME, &l) && !l.n); /* there, with nothing to show */
    lib_free(&l);
    CHECK(!lib_list(root, "Nope", SORT_NAME, &l) && !l.n);
    lib_free(&l); /* harmless after a 0 */
    CHECK(!lib_list(root, "../Other", SORT_NAME, &l) && !lib_list(root, ".trash", SORT_NAME, &l));
    CHECK(!lib_list(root, "Episode 1.MP4", SORT_NAME, &l) && !lib_list(root, 0, SORT_NAME, &l));

    CHECK(lib_all(root, &l) && l.n == 8 && !l.dirs);
    SAME(l.v[0].name, "Deep/a/b/c/d.mov"), SAME(l.v[1].name, "Episode 1.MP4");
    SAME(l.v[2].name, "episode 2.mkv"), SAME(l.v[3].name, "Episode 10.mkv");
    SAME(l.v[4].name, "Movies/Film.mkv"), SAME(l.v[5].name, "Series/Season 1/a.mkv");
    SAME(l.v[6].name, "Series/Season 1/b.avi"), SAME(l.v[7].name, "Series/Season 2/c.webm");
    CHECK(l.v[4].size == 5ll << 30 && l.v[4].mtime == 4000 && l.v[0].size == 4);
    lib_free(&l);
    CHECK(!lib_all(CARD "/Nope", &l) && !l.n); /* no folder is not an empty library */
    CHECK(lib_all(CARD "/VIDEOS/Empty", &l) && !l.n);

    /* Play Next: the one after it in its folder's list, in the list's order */
    CHECK(lib_next(root, "Episode 1.MP4", SORT_NAME, p, sizeof p)), SAME(p, "episode 2.mkv");
    CHECK(lib_next(root, "episode 2.mkv", SORT_NAME, p, sizeof p)), SAME(p, "Episode 10.mkv");
    CHECK(!lib_next(root, "Episode 10.mkv", SORT_NAME, p, sizeof p)); /* the last */
    CHECK(lib_next(root, "episode 2.mkv", SORT_NEWEST, p, sizeof p)), SAME(p, "Episode 1.MP4");
    CHECK(lib_next(root, "Series/Season 1/a.mkv", SORT_NAME, p, sizeof p));
    SAME(p, "Series/Season 1/b.avi");
    CHECK(!lib_next(root, "Series/Season 1/b.avi", SORT_NAME, p, sizeof p));
    CHECK(!lib_next(root, "gone.mp4", SORT_NAME, p, sizeof p));
    CHECK(!lib_next(root, "Series", SORT_NAME, p, sizeof p) && !lib_next(root, "", 0, p, sizeof p));
    CHECK(!lib_next(root, "../x.mp4", SORT_NAME, p, sizeof p));
    CHECK(!lib_next(root, "Episode 1.MP4", SORT_NAME, small, sizeof small)); /* no room */

    /* a path of 400 bytes is a row, one of 401 is left out and not counted */
    char deep[320], part[100], name[100];
    memset(part, 'd', 99), part[99] = 0, memset(name, 'n', 99), name[99] = 0;
    int at = snprintf(deep, sizeof deep, "Long/%s/%s/%s", part, part, part); /* 304 bytes */
    snprintf(p, sizeof p, V "%s/%.91s.mp4", deep, name), file(p, 1, 0);      /* 305 + 95 */
    snprintf(p, sizeof p, V "%s/%.92s.mp4", deep, name), file(p, 1, 0);      /* 305 + 96 */
    CHECK(at == 304 && lib_list(root, "", SORT_NAME, &l) && l.dirs == 4);
    SAME(l.v[1].name, "Long");
    CHECK(l.v[1].count == 1);
    lib_free(&l);
    CHECK(lib_list(root, deep, SORT_NAME, &l) && l.n == 1 && strlen(l.v[0].name) == 95);
    lib_free(&l);
    CHECK(lib_all(root, &l) && l.n == 9 && strlen(l.v[4].name) == REL_MAX);
    lib_free(&l);

    /* Continue Watching: the unfinished video played last that is still there */
    CHECK(!lib_continue(root, &m));
    marks_put(&m, "Episode 1.MP4", 30)->pos_ms = 5000;
    marks_put(&m, "episode 2.mkv", 20)->pos_ms = 7000;
    mark_t *k = marks_put(&m, "Movies/Film.mkv", 5ll << 30);
    k->pos_ms = 9000, k->flags = MARK_WATCHED;          /* watched */
    marks_put(&m, "Series/Season 1/a.mkv", 1);          /* never begun */
    marks_put(&m, "gone.mp4", 1)->pos_ms = 100;         /* no longer on the card */
    marks_put(&m, "Episode 10.mkv", 999)->pos_ms = 100; /* another file by now */
    marks_put(&m, "notes.txt", 1)->pos_ms = 100;        /* not a video */
    const mark_t *c = lib_continue(root, &m);
    CHECK(c && c->seq == 2), SAME(c->rel, "episode 2.mkv");
    CHECK(m.n == 7); /* and nothing was dropped */
    marks_find(&m, "episode 2.mkv", 20)->flags = MARK_WATCHED;
    CHECK((c = lib_continue(root, &m))), SAME(c->rel, "Episode 1.MP4");
    marks_find(&m, "Episode 1.MP4", 30)->pos_ms = 0;
    CHECK(!lib_continue(root, &m));
    marks_free(&m);

    /* a spelling in small letters, where the card has that one */
    sh("mv " CARD "/VIDEOS " CARD "/videos");
    CHECK(lib_root(root, sizeof root)), SAME(root, CARD "/videos");
    CHECK(lib_list(root, "Series/Season 1", SORT_NAME, &l) && l.n == 2);
    lib_free(&l);
}

int main(void) {
    sh("rm -rf " CARD " " TMP " && mkdir -p " CARD " " TMP);
    check_common();
    check_settings();
    check_marks();
    check_names();
    check_library();
    puts("check: ok");
    return 0;
}
