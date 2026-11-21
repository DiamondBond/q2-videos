/* STUBS, to be deleted. The server and the player are not written yet: these weak stand-ins let
 * the binary link until src/ has a serve_main and a player_main, which take their place without
 * a change here. Delete this file, and STUB_SRC in the Makefile, once both are there. */
#include "videos.h"
#include <stdio.h>

/* the server's: 0, so run's loop ends instead of starting it again */
__attribute__((weak)) int serve_main(void) {
    fprintf(stderr, "q2-videos: this build has no server\n");
    return 0;
}

/* the player's: says whether the Q2's FFmpeg is the one it was built for, then ends */
__attribute__((weak)) int player_main(int argc, char **argv) {
    char why[80];
    fprintf(stderr, "q2-videos: this build has no player (FFmpeg %s%s)\n",
            av_ok(why, sizeof why) ? "as built for" : "differs: ", why);
    return 0;
}
