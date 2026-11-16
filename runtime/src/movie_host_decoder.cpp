// SPDX-License-Identifier: GPL-3.0-or-later
// The host movie::Decoder (movie_host_decoder.h). The protocol is Witch-Time's runtime/nnmovie_mf.c (vtable slots,
// DecoderEvents, Buffer fields, status codes, buffer counts, Start/Flush rules); the frames are this runtime's host
// decoders' (movie.cpp). Slot numbers are this tree's: the vtable's first method is slot 2 (hle_movie_api.cpp).
//
// What the game's player does with a video decoder (main+cd6318 the tick, main+cd786c the frame fetch):
//   Configure(track format) -> RegisterClientEvents -> Start; then, on the input event, AcquireInputBufferIndex /
//   GetInputBuffer / Extractor::Read into it / SendInputBufferForDecode(index, offset, size, timeUs, flags); on the
//   output event, AcquireOutputBufferIndex(index, timeUs, flags); when that frame is due, GetOutputBufferFormat(index,
//   md) (nv12-y-offset, nv12-y-stride, nv12-uv-offset, nv12-uv-stride), GetOutputFormat(md) (nv12-colorspace),
//   GetOutputBuffer(index, Buffer{its own memory}) and it copies the Y and UV planes out of that memory into its two
//   plane textures; then ReleaseOutputBufferIndex. A loop is SeekTo(0), Flush, Start.
//
// Here the compressed samples the game feeds are only counted: each sample's time becomes an output frame, and
// GetOutputBuffer asks the film's host decoder for the frame due at that time.
#include "movie_host_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nx/cpu.h>
#include <nx/runtime.h>

#include "movie.h"
#include "movie_host_queue.h"

namespace nx::movie_host {
namespace {

// movie::Buffer (0x40 bytes) [Witch-Time, subsdk 0x23a70..0x23d30]
constexpr uint64_t kBufOffset = 0x00, kBufSize = 0x04, kBufFlags = 0x0c, kBufCapacity = 0x18, kBufTime = 0x20,
                   kBufBase = 0x28;
// The controller (main-module object, hle_movie_api.cpp): +0x18 the film's file handle, +0x160 its video decoder.
constexpr uint64_t kCtlFileHandle = 0x18, kCtlDecoder = 0x160;
// The slot thunk is reached from main+cd79b0's caller with the controller in x19 (movie_frame_source.h).
constexpr uint64_t kFetchReturn = 0xcd79b0ull;

enum Slot {
    kDtor = 2, kDeletingDtor = 3, kConfigure = 4, kRegisterClientEvents = 5, kGetOutputFormat = 6, kStart = 7,
    kStop = 8, kReset = 9, kFlush = 10, kAcquireInput = 11, kGetInputBuffer = 12, kSendInput = 13,
    kAcquireOutput = 14, kGetOutputBuffer = 15, kGetOutputBufferSize = 16, kReleaseOutput = 17, kGetLastError = 18,
    kGetOutputBufferWithFormat = 19, kGetOutputBufferFormat = 20,
};

bool readable(uint64_t addr, size_t size) {
    if (!addr) return false;
    const VmInfo v = vm_query(addr);
    return v.state != 0 && (v.perm & 1) && addr >= v.addr && addr - v.addr + size <= v.size;
}

// A guest function called from inside a thunk: every register is put back afterwards, so the thunk's own caller
// sees only the x0 the thunk sets.
uint64_t gcall(CPU& c, uint64_t fn, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!fn) return 0;
    CPU saved = c;
    c.x[0] = a0, c.x[1] = a1, c.x[2] = a2, c.x[3] = a3;
    call(c, fn);
    const uint64_t r = c.x[0];
    c = saved;
    return r;
}

struct Guest {
    uint64_t signal_event = 0, find_int32 = 0, find_string = 0, set_int32 = 0, set_string = 0, buffer_set_data = 0;
    bool ok = false;
};
const Guest& guest() {
    static const Guest g = [] {
        Guest x;
        x.signal_event = find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        x.find_int32 = find_symbol("_ZNK5movie9MediaData9FindInt32EPKcPi");
        x.find_string = find_symbol("_ZNK5movie9MediaData10FindStringEPKcPS2_");
        x.set_int32 = find_symbol("_ZN5movie9MediaData8SetInt32EPKci");
        x.set_string = find_symbol("_ZN5movie9MediaData9SetStringEPKcS2_");
        x.buffer_set_data = find_symbol("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        x.ok = x.signal_event && x.find_int32 && x.find_string && x.set_int32 && x.buffer_set_data;
        if (!x.ok)
            log("movie: host decoder unavailable: SignalEvent %llx FindInt32 %llx FindString %llx SetInt32 %llx "
                "Buffer::SetDataAndCapacity %llx", (unsigned long long)x.signal_event,
                (unsigned long long)x.find_int32, (unsigned long long)x.find_string, (unsigned long long)x.set_int32,
                (unsigned long long)x.buffer_set_data);
        return x;
    }();
    return g;
}

bool md_int(CPU& c, uint64_t md, const char* key, int32_t& out) {
    int32_t v = 0;
    if (!md || !(gcall(c, guest().find_int32, md, uint64_t(uintptr_t(key)), uint64_t(uintptr_t(&v))) & 1))
        return false;
    out = v;
    return true;
}
std::string md_string(CPU& c, uint64_t md, const char* key) {
    uint64_t p = 0;
    if (!md || !(gcall(c, guest().find_string, md, uint64_t(uintptr_t(key)), uint64_t(uintptr_t(&p))) & 1) ||
        !readable(p, 1))
        return {};
    std::string s;
    for (size_t i = 0; i < 64 && readable(p + i, 1); i++) {
        const char ch = char(ld<uint8_t>(p + i));
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}
void md_set(CPU& c, uint64_t md, const char* key, int32_t v) {
    gcall(c, guest().set_int32, md, uint64_t(uintptr_t(key)), uint64_t(uint32_t(v)));
}

struct Decoder {
    std::mutex m;
    Queue q;
    uint64_t events = 0;  // DecoderEvents*: four nn::os::EventType*
    uint32_t w = 0, h = 0;
    std::string mime;
    size_t input_capacity = 0;
    std::vector<std::unique_ptr<uint8_t[]>> input;  // the compressed-sample buffers the game reads into
    uint64_t controller = 0, file_handle = 0;
    uint64_t samples = 0, frames = 0, missed = 0, flushes = 0;
};

std::mutex g_m;
std::map<uint64_t, std::shared_ptr<Decoder>> g_decoders;  // by guest Decoder object
std::map<uint64_t, uint64_t> g_events;                   // RegisterClientEvents seen before Configure
std::vector<uint64_t> g_controllers;                     // the newest few movie controllers

std::shared_ptr<Decoder> find(uint64_t self) {
    std::lock_guard lk(g_m);
    const auto it = g_decoders.find(self);
    return it == g_decoders.end() ? nullptr : it->second;
}

void signal(CPU& c, const Decoder& d, int events) {
    if (!d.events || !events) return;
    static const int order[] = {kEvFormat, kEvOutput, kEvInput};  // format first: the player sizes its planes on it
    static const int index[] = {2, 1, 0};
    for (int k = 0; k < 3; k++) {
        if (!(events & order[k])) continue;
        const uint64_t slot = d.events + uint64_t(index[k]) * 8;
        const uint64_t ev = readable(slot, 8) ? ld<uint64_t>(slot) : 0;
        if (ev) gcall(c, guest().signal_event, ev);
    }
}

// The film this decoder plays: the controller that holds it at +0x160, then that controller's file handle.
uint64_t film_of(CPU& c, Decoder& d, uint64_t self) {
    if (d.controller && readable(d.controller + kCtlDecoder, 8) && ld<uint64_t>(d.controller + kCtlDecoder) == self) {
        const uint64_t h = ld<uint64_t>(d.controller + kCtlFileHandle);
        if (h) return d.file_handle = h;
    }
    std::vector<uint64_t> candidates;
    {
        std::lock_guard lk(g_m);
        candidates = g_controllers;
    }
    if (c.x[30] == kModuleBase + kFetchReturn) candidates.push_back(c.x[19]);
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const uint64_t ctl = *it;
        if (!readable(ctl + kCtlDecoder, 8) || ld<uint64_t>(ctl + kCtlDecoder) != self) continue;
        const uint64_t h = ld<uint64_t>(ctl + kCtlFileHandle);
        if (!h) continue;
        d.controller = ctl;
        return d.file_handle = h;
    }
    return d.file_handle;
}

void fill_format(CPU& c, const Decoder& d, uint64_t md) {
    if (!md) return;
    const int32_t w = int32_t(d.w), h = int32_t(d.h);
    if (guest().set_string) gcall(c, guest().set_string, md, uint64_t(uintptr_t("mime")),
                                       uint64_t(uintptr_t("video/raw")));
    md_set(c, md, "width", w);
    md_set(c, md, "height", h);
    md_set(c, md, "stride", w);
    md_set(c, md, "slice-height", h);
    md_set(c, md, "crop-width", w);
    md_set(c, md, "crop-height", h);
    md_set(c, md, "color-format", 21);  // OMX_COLOR_FormatYUV420SemiPlanar, i.e. NV12
    md_set(c, md, "nv12-width", w);
    md_set(c, md, "nv12-height", h);
    md_set(c, md, "nv12-y-stride", w);
    md_set(c, md, "nv12-uv-stride", w);
    md_set(c, md, "nv12-y-offset", 0);
    md_set(c, md, "nv12-uv-offset", w * h);
    md_set(c, md, "nv12-colorspace", 0);
}

uint32_t output_buffer(CPU& c, Decoder& d, uint64_t self, int index, uint64_t buf) {
    if (!buf || !readable(buf, 0x40)) return kInvalid;
    int64_t t = 0;
    uint32_t flags = 0;
    {
        std::lock_guard lk(d.m);
        if (!d.q.held_output(index, t, flags)) return kInvalid;  // not acquired, or voided by a Flush
    }
    size_t n = 0;
    if (!(flags & kFlagEos)) {
        const uint64_t base = ld<uint64_t>(buf + kBufBase), cap = ld<uint64_t>(buf + kBufCapacity);
        const uint64_t film = film_of(c, d, self);
        if (base && film && d.w && d.h) n = movie_host_frame(film, t, d.w, d.h, reinterpret_cast<uint8_t*>(base), cap);
        std::lock_guard lk(d.m);
        if (n) {
            if (++d.frames == 1)
                log("movie: host decoder %llx: first frame to the game (%ux%u at %lld us, %llu-byte buffer)",
                    (unsigned long long)self, d.w, d.h, (long long)t, (unsigned long long)cap);
        } else if (++d.missed <= 3) {
            log("movie: host decoder %llx: no frame for %lld us (film handle %llu, buffer %llx, %llu bytes)",
                (unsigned long long)self, (long long)t, (unsigned long long)film, (unsigned long long)base,
                (unsigned long long)cap);
        }
    }
    st<uint32_t>(buf + kBufOffset, 0);
    st<uint32_t>(buf + kBufSize, uint32_t(n));
    st<uint64_t>(buf + kBufTime, uint64_t(t));
    st<uint32_t>(buf + kBufFlags, flags);
    return kOk;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_MOVIE_DECODER");
        return !(e && std::strcmp(e, "game") == 0);
    }();
    return on;
}

void note_controller(uint64_t controller) {
    if (!controller) return;
    std::lock_guard lk(g_m);
    for (uint64_t c : g_controllers)
        if (c == controller) return;
    g_controllers.push_back(controller);
    if (g_controllers.size() > 16) g_controllers.erase(g_controllers.begin());
}

bool decoder_call(CPU& c, int slot) {
    if (!enabled()) return false;
    const uint64_t self = c.x[0];
    if (!self) return false;

    if (slot == kDtor || slot == kDeletingDtor) {  // the game's destructor frees its own object and implementation
        std::shared_ptr<Decoder> d;
        {
            std::lock_guard lk(g_m);
            g_events.erase(self);
            if (auto it = g_decoders.find(self); it != g_decoders.end()) d = std::move(it->second), g_decoders.erase(it);
        }
        if (d)
            log("movie: host decoder %llx destroyed: %llu samples, %llu frames to the game, %llu without a frame, "
                "%llu flushes", (unsigned long long)self, (unsigned long long)d->samples,
                (unsigned long long)d->frames, (unsigned long long)d->missed, (unsigned long long)d->flushes);
        return false;
    }

    if (slot == kConfigure) {
        if (!guest().ok) return false;
        const uint64_t md = c.x[1];
        const std::string mime = md_string(c, md, "mime");
        if (mime.rfind("video/", 0) != 0) return false;  // audio: the game's own decoder
        auto d = std::make_shared<Decoder>();
        d->mime = mime;
        int32_t v = 0;
        if (md_int(c, md, "width", v) && v > 0) d->w = uint32_t(v);
        if (md_int(c, md, "height", v) && v > 0) d->h = uint32_t(v);
        int32_t max_input = 0;
        md_int(c, md, "max-input-size", max_input);
        d->input_capacity = size_t(std::max<int32_t>(max_input, 1 << 20));
        d->input.resize(size_t(d->q.inputs()));
        {
            std::lock_guard lk(g_m);
            if (auto it = g_events.find(self); it != g_events.end()) d->events = it->second;
            g_decoders[self] = d;
        }
        log("movie: host decoder %llx takes the video track (%s, %ux%u, input buffers of %zu bytes): the game's "
            "nvdec path is not used for it", (unsigned long long)self, mime.c_str(), d->w, d->h, d->input_capacity);
        movie_host_decoding();
        c.x[0] = kOk;
        return true;
    }

    if (slot == kRegisterClientEvents) {
        const uint64_t events = c.x[1];
        auto d = find(self);
        if (!d) {
            std::lock_guard lk(g_m);
            g_events[self] = events;
            return false;  // the game's decoder keeps them as well, until Configure says whose this is
        }
        int pending;
        {
            std::lock_guard lk(d->m);
            d->events = events;
            pending = d->q.pending_events();
        }
        signal(c, *d, pending);
        c.x[0] = kOk;
        return true;
    }

    auto d = find(self);
    if (!d) return false;
    uint32_t r = kOk;
    int ev = 0;
    switch (slot) {
    case kGetOutputFormat:
        fill_format(c, *d, c.x[1]);
        break;
    case kStart: {
        std::lock_guard lk(d->m);
        ev = d->q.start();
        break;
    }
    case kStop:
    case kReset:
    case kFlush: {
        std::lock_guard lk(d->m);
        d->q.halt(slot != kFlush);
        d->flushes++;
        break;
    }
    case kAcquireInput: {
        const uint64_t pi = c.x[1], pm = c.x[2];
        if (!pi || !pm) {
            r = kInvalid;
            break;
        }
        int index, more;
        {
            std::lock_guard lk(d->m);
            r = d->q.acquire_input(index, more);
        }
        st<uint32_t>(pi, uint32_t(index));
        st<uint32_t>(pm, uint32_t(more));
        break;
    }
    case kGetInputBuffer: {
        const int i = int(uint32_t(c.x[1]));
        const uint64_t buf = c.x[2];
        uint8_t* mem = nullptr;
        {
            std::lock_guard lk(d->m);
            if (!buf || !d->q.holds_input(i)) {
                r = buf ? kFailed : kInvalid;  // a stale index: the game waits a tick instead of reading into it
                break;
            }
            auto& slot_mem = d->input[size_t(i)];
            if (!slot_mem) slot_mem.reset(new uint8_t[d->input_capacity]);
            mem = slot_mem.get();
        }
        gcall(c, guest().buffer_set_data, buf, uint64_t(uintptr_t(mem)), d->input_capacity);
        st<uint32_t>(buf + kBufOffset, 0);
        st<uint32_t>(buf + kBufSize, 0);
        break;
    }
    case kSendInput: {
        const int i = int(uint32_t(c.x[1]));
        const int32_t size = int32_t(uint32_t(c.x[3]));
        const int64_t t = int64_t(c.x[4]);
        const uint32_t flags = uint32_t(c.x[5]);
        std::lock_guard lk(d->m);
        r = d->q.send_input(i, size, t, flags, ev);
        if (r == kOk && size > 0) d->samples++;
        break;
    }
    case kAcquireOutput: {
        const uint64_t pi = c.x[1], pt = c.x[2], pf = c.x[3], pm = c.x[4];
        if (!pi || !pt || !pf || !pm) {
            r = kInvalid;
            break;
        }
        int index, more;
        int64_t t;
        uint32_t flags;
        {
            std::lock_guard lk(d->m);
            r = d->q.acquire_output(index, t, flags, more);
        }
        st<uint32_t>(pi, uint32_t(index));
        st<uint64_t>(pt, uint64_t(t));
        st<uint32_t>(pf, flags);
        st<uint32_t>(pm, uint32_t(more));
        break;
    }
    case kGetOutputBuffer:
        r = output_buffer(c, *d, self, int(uint32_t(c.x[1])), c.x[2]);
        break;
    case kGetOutputBufferWithFormat:
        fill_format(c, *d, c.x[3]);
        r = output_buffer(c, *d, self, int(uint32_t(c.x[1])), c.x[2]);
        break;
    case kGetOutputBufferFormat:
        fill_format(c, *d, c.x[2]);
        break;
    case kGetOutputBufferSize: {
        if (!c.x[1]) {
            r = kInvalid;
            break;
        }
        st<uint32_t>(c.x[1], d->w * d->h + 2 * ((d->w + 1) / 2) * ((d->h + 1) / 2));
        if (c.x[2]) st<uint32_t>(c.x[2], 1);
        break;
    }
    case kReleaseOutput: {
        std::lock_guard lk(d->m);
        ev = d->q.release_output(int(uint32_t(c.x[1])));
        break;
    }
    case kGetLastError:
        r = kOk;
        break;
    default:
        return false;
    }
    signal(c, *d, ev);
    c.x[0] = r;
    return true;
}

}  // namespace nx::movie_host
