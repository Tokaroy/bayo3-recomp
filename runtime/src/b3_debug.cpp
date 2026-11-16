// SPDX-License-Identifier: GPL-3.0-or-later
// The phase log and the debug commands (nx/b3_debug.h). Addresses from Witch-Time's docs/RE_NOTES.md sections 10-11,
// read again in this tree's generated code: the phase request main+0x66f464 stores {1, phase} at flow+0x50, copies
// the sub-phase name (0x20 bytes) to flow+0x58 and starts its own fade when w5 is -1; its callers load the flow
// object from main+0x249da60 (e.g. main+0x7bf850, the chapter select's 0xc20).
#include "nx/b3_debug.h"

#include <cstdio>
#include <string>
#include <vector>

#include <nx/cpu.h>
#include <nx/runtime.h>  // kModuleBase, log

#include "kernel.h"
#include "platform.h"

namespace nx::b3 {
namespace {

constexpr uint64_t kPhaseRecord = 0x4d72f0cull;   // u32 phase, u32 sub-phase hash, then the sub-phase's name
constexpr uint64_t kPhaseNameOffset = 8;
constexpr size_t kPhaseNameMax = 0x4c;            // the record is 0x54 bytes
constexpr uint64_t kPhaseRequest = 0x66f464ull;
constexpr uint32_t kPhaseRequestFirst = 0xa9bc7bfdu;  // stp x29, x30, [sp, #-0x40]!: the 1.2.0 entry
constexpr uint64_t kFlowObjectSlot = 0x249da60ull;

bool readable(uint64_t addr, size_t size) {
    const VmInfo v = vm_query(addr);
    return v.state != 0 && (v.perm & 1) && addr >= v.addr && addr - v.addr + size <= v.size;
}

std::string phase_name(uint64_t rec) {
    std::string s;
    for (size_t i = 0; i < kPhaseNameMax; i++) {
        const char ch = char(ld<uint8_t>(rec + kPhaseNameOffset + i));
        if (!ch) break;
        s.push_back(ch >= 0x20 && ch < 0x7f ? ch : '?');
    }
    return s;
}

void log_phase(uint64_t present) {
    static uint32_t last_phase = UINT32_MAX;
    static std::string last_name;
    const uint64_t rec = kModuleBase + kPhaseRecord;
    if (!readable(rec, 8 + kPhaseNameMax)) return;
    const uint32_t phase = ld<uint32_t>(rec);
    std::string name = phase_name(rec);
    if (phase == last_phase && name == last_name) return;
    log("phase: %03x %s (present %llu)", phase, name.empty() ? "-" : name.c_str(), (unsigned long long)present);
    last_phase = phase;
    last_name = std::move(name);
}

// The game's own request, as its callers make it: x0 the flow object, w1 the phase, x2 the sub-phase name, w3 0,
// w4 0, w5 -1 (a fade of its own). Every register the call may change is put back, so the present continues as if
// nothing ran; the request takes effect when the game moves asked -> current (main+0x8481fc).
bool request_phase(CPU& c, const DebugCommand& cmd) {
    const uint64_t fn = kModuleBase + kPhaseRequest;
    const uint64_t slot = kModuleBase + kFlowObjectSlot;
    if (!readable(fn, 4) || ld<uint32_t>(fn) != kPhaseRequestFirst || !readable(slot, 8)) {
        log("debug: phase %s not requested: this is not the 1.2.0 main the addresses were read from", cmd.name.c_str());
        return false;
    }
    const uint64_t flow = ld<uint64_t>(slot);
    if (!flow || !readable(flow, 0x80)) {
        log("debug: phase %s not requested: no flow object yet", cmd.name.c_str());
        return false;
    }
    static char name[0x20];  // the game copies it (strncpy to flow+0x58) before the call returns
    std::snprintf(name, sizeof name, "%s", cmd.name.c_str());
    CPU saved = c;
    c.x[0] = flow;
    c.x[1] = uint32_t(cmd.phase);
    c.x[2] = uint64_t(reinterpret_cast<uintptr_t>(name));
    c.x[3] = 0;
    c.x[4] = 0;
    c.x[5] = 0xffffffffu;
    call(c, fn);
    const uint64_t ret = c.x[0];
    c = saved;
    log("debug: asked the game for phase %03x %s (flow %llx, returned %llu)", cmd.phase, cmd.name.c_str(),
        (unsigned long long)flow, (unsigned long long)ret);
    return true;
}

void run_line(CPU& c, const std::string& line, const char* from) {
    DebugCommand cmd;
    if (debug_parse(line, cmd)) {
        request_phase(c, cmd);
        return;
    }
    const size_t a = line.find_first_not_of(" \t\r\n");
    if (a != std::string::npos && line[a] != '#')
        log("debug: %s: not a command: %s (use: phase <sub-phase name>, e.g. phase pc20_ChapterSelect)", from,
            line.c_str());
}

void run_file(CPU& c) {
    const std::string path = plat::data_root() + "/debug_cmd.txt";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 16384) text.append(buf, n);
    std::fclose(f);
    const std::string done = plat::data_root() + "/debug_cmd.done";
    std::remove(done.c_str());
    if (std::rename(path.c_str(), done.c_str()) != 0) std::remove(path.c_str());  // run each file once
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        run_line(c, text.substr(pos, end - pos), "debug_cmd.txt");
        pos = end + 1;
    }
}

struct Scheduled {
    uint64_t at;
    std::string line;
};

std::vector<Scheduled> scheduled() {
    std::vector<Scheduled> v;
    const char* e = std::getenv("NX_PHASE_AT");
    if (!e) return v;
    const std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        v.push_back({std::strtoull(item.c_str(), nullptr, 10), "phase " + item.substr(colon + 1)});
    }
    return v;
}

}  // namespace

void debug_frame(CPU& c, uint64_t present) {
    log_phase(present);
    static std::vector<Scheduled> plan = scheduled();
    for (auto& s : plan)
        if (s.at && present >= s.at) {
            run_line(c, s.line, "NX_PHASE_AT");
            s.at = 0;
        }
    if (present % 30 == 0) run_file(c);
}

}  // namespace nx::b3
