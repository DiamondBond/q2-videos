/* q2-videos: with "serve" the page's server, which run starts; with a row in the environment
 * (Q2_URI) the player, which Q2 Pod runs over the whole screen as DEVICE [VOLUME]. */
#include "videos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) return puts(VERSION), 0;
    paths_init();
    if (argc > 1 && !strcmp(argv[1], "serve")) return serve_main();
    if (getenv("Q2_URI")) return player_main(argc, argv);
    fprintf(stderr, "q2-videos %s: a Q2 Pod card app; \"serve\" is its page\n", VERSION);
    return 2;
}
