// The debug commands' parsing (runtime/include/nx/b3_debug.h), without the game.
#include <cstdio>
#include <cstdlib>

#include "nx/b3_debug.h"

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

int main() {
    using namespace nx::b3;
    check(debug_phase_of("p9a0_training") == 0x9a0, "p9a0");
    check(debug_phase_of("pc20_ChapterSelect") == 0xc20, "pc20");
    check(debug_phase_of("p120_ShibuyaScramble") == 0x120, "p120");
    check(debug_phase_of("training") == -1 && debug_phase_of("p9") == -1 && debug_phase_of("pxyz_a") == -1, "bad");
    check(debug_phase_of("p120ShibuyaScramble") == -1 && debug_phase_of("pC20_x") == -1, "no underscore, upper case");

    DebugCommand cmd;
    check(debug_parse("phase p120_ShibuyaScramble", cmd) && cmd.phase == 0x120 && cmd.name == "p120_ShibuyaScramble",
          "phase line");
    check(debug_parse("  phase\tpc20_ChapterSelect\r\n", cmd) && cmd.phase == 0xc20 && cmd.name == "pc20_ChapterSelect",
          "spaces, tab, CRLF");
    check(!debug_parse("", cmd) && !debug_parse("# phase p120_x", cmd) && !debug_parse("phase", cmd), "empty/comment");
    check(!debug_parse("phase training", cmd) && !debug_parse("warp p120_x", cmd), "not a phase");
    check(!debug_parse("phase p120_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", cmd), "longer than the game copies");
    std::puts("b3_debug: ok");
    return 0;
}
