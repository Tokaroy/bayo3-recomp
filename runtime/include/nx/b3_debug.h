// SPDX-License-Identifier: GPL-3.0-or-later
// Bayonetta 3 1.2.0 debug commands and the phase log. The phase record and the phase request are documented in
// Witch-Time's docs/RE_NOTES.md sections 10 and 11 (runtime/debug_cmd.h, game_shim_eh.c debug_tick); see
// docs/WITCH-TIME-NOTICES.md. The parsing below is pure, so tests/b3_debug_test.cpp checks it without the game.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace nx {
struct CPU;
}

namespace nx::b3 {

// A phase's number from its sub-phase's name: "p9a0_training" -> 0x9a0, "pc20_ChapterSelect" -> 0xc20. -1 = not one.
inline int debug_phase_of(const char* name) {
    if (!name || name[0] != 'p' || std::strlen(name) < 5 || name[4] != '_') return -1;
    int v = 0;
    for (int i = 1; i < 4; i++) {
        const char ch = name[i];
        const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

// One command line: "phase <sub-phase name>" asks the game for that phase (its number is the name's p<hex>).
// Returns false for anything else; blank lines and '#' comments are not commands.
struct DebugCommand {
    int phase = -1;
    std::string name;
};
inline bool debug_parse(const std::string& line, DebugCommand& out) {
    size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || line[a] == '#') return false;
    const size_t b = line.find_first_of(" \t", a);
    if (b == std::string::npos || line.compare(a, b - a, "phase") != 0) return false;
    const size_t c = line.find_first_not_of(" \t", b);
    if (c == std::string::npos) return false;
    const size_t d = line.find_first_of(" \t\r\n", c);
    std::string name = line.substr(c, d == std::string::npos ? std::string::npos : d - c);
    // The game copies at most 0x20 bytes of the name (main+0x66f4bc); a longer one cannot be a sub-phase.
    if (name.size() >= 0x20) return false;
    const int phase = debug_phase_of(name.c_str());
    if (phase < 0) return false;
    out.phase = phase;
    out.name = std::move(name);
    return true;
}

// Called by nvnQueuePresentTexture on the game's thread, once per present: logs every phase change, and runs the
// commands of <data>/debug_cmd.txt (read every 30 presents, then renamed to debug_cmd.done) and of
// NX_PHASE_AT=<present>:<sub-phase>[;<present>:<sub-phase>...] for scripted runs.
void debug_frame(CPU& c, uint64_t present);

}  // namespace nx::b3
