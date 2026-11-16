// SPDX-License-Identifier: GPL-3.0-or-later
// The buffer-index protocol of a host movie::Decoder (movie_host_decoder.cpp). The protocol, the status codes, the
// buffer counts and the rules for Start/Flush/Stop and stale indices follow Witch-Time's runtime/nnmovie_mf.c
// (docs/WITCH-TIME-NOTICES.md): the game's player drives a decoder through input indices (AcquireInputBufferIndex,
// GetInputBuffer, SendInputBufferForDecode) and output indices (AcquireOutputBufferIndex, GetOutputBuffer,
// ReleaseOutputBufferIndex), and waits on the decoder's client events between them.
//
// Pure bookkeeping, no guest memory and no locking (the caller holds its decoder's lock), so
// tests/movie_host_queue_test.cpp drives it without the game.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace nx::movie_host {

// movie:: status codes [subsdk]: the game treats anything but kOk as "not now / not valid".
constexpr uint32_t kOk = 0;
constexpr uint32_t kFailed = 0x1000;
constexpr uint32_t kInvalid = 0x100b;
constexpr uint32_t kNoOutput = 0x100e;  // AcquireOutputBufferIndex: nothing decoded [subsdk 0x1e8d0]
constexpr uint32_t kNoInput = 0x1011;   // AcquireInputBufferIndex: nothing offered [subsdk 0x1fac8]
constexpr uint32_t kFlagEos = 4;        // MediaCodec's end of stream

// The client events, as bits of what one call wants signalled. DecoderEvents holds them in this order:
// +0 input available, +8 output available, +0x10 output format changed, +0x18 error.
constexpr int kEvInput = 1, kEvOutput = 2, kEvFormat = 4;

class Queue {
public:
    // 8 compressed-data buffers bound how far the feed runs ahead; 16 decoded frames is the player's own pool for a
    // video decoder (a frame acquired while that pool is empty is dropped and the picture stops), per Witch-Time.
    explicit Queue(int inputs = 8, int outputs = 16)
        : in_state_(size_t(inputs), InCodec), in_data_(size_t(inputs)), out_state_(size_t(outputs), OutFree),
          out_data_(size_t(outputs)) {}

    int inputs() const { return int(in_state_.size()); }
    int outputs() const { return int(out_state_.size()); }

    // Start: the first start, and the first after a Flush/Stop, offers every input index. Returns the events to
    // signal: the format event once per decoder (the player waits for every decoder's before it starts), and input.
    int start() {
        const bool offer = !started_ || flushed_;
        if (offer) {
            release_all();
            for (int i = 0; i < inputs(); i++) {
                in_state_[size_t(i)] = InOffered;
                in_list_.push_back(i);
            }
        }
        started_ = true;
        flushed_ = false;
        int ev = offer ? kEvInput : 0;
        if (!format_signalled_) {
            format_signalled_ = true;
            ev |= kEvFormat;
        }
        return ev;
    }

    // Flush (stop = false) and Stop/Reset (stop = true) void every index the game holds and every decoded frame;
    // nothing is offered again until the next Start, so no event.
    void halt(bool stop) {
        release_all();
        flushed_ = true;
        if (stop) started_ = false;
    }

    uint32_t acquire_input(int& index, int& more) {
        index = -1;
        more = 0;
        if (in_list_.empty()) return kNoInput;
        index = in_list_.front();
        in_list_.pop_front();
        in_state_[size_t(index)] = InClient;
        more = int(in_list_.size());
        return kOk;
    }

    // The game keeps its own index list across a loop (SeekTo(0), Flush, Start), so it asks for buffers it no longer
    // holds: GetInputBuffer refuses those and the game waits a tick instead of reading into them (Witch-Time #138).
    bool holds_input(int i) const { return valid_in(i) && in_state_[size_t(i)] == InClient; }

    // One compressed sample handed back. A sample of a voided index is refused (kInvalid, nothing changes); an empty
    // one that is not the end of stream offers its buffer again; anything else becomes a decoded frame as soon as an
    // output buffer is free. `events` gets what to signal.
    uint32_t send_input(int i, int32_t size, int64_t time_us, uint32_t flags, int& events) {
        events = 0;
        if (!holds_input(i)) return kInvalid;
        if (size <= 0 && !(flags & kFlagEos)) {
            in_state_[size_t(i)] = InOffered;
            in_list_.push_back(i);
            events = kEvInput;
            return kOk;
        }
        in_data_[size_t(i)] = {time_us, flags & kFlagEos};
        in_state_[size_t(i)] = InDecoding;
        dec_list_.push_back(i);
        events = pump();
        return kOk;
    }

    uint32_t acquire_output(int& index, int64_t& time_us, uint32_t& flags, int& more) {
        index = -1;
        time_us = 0;
        flags = 0;
        more = 0;
        if (out_list_.empty()) return kNoOutput;
        index = out_list_.front();
        out_list_.pop_front();
        out_state_[size_t(index)] = OutClient;
        time_us = out_data_[size_t(index)].time_us;
        flags = out_data_[size_t(index)].flags;
        more = int(out_list_.size());
        return kOk;
    }

    // The frame behind an output index the game holds (false: not acquired, or voided by a Flush).
    bool held_output(int i, int64_t& time_us, uint32_t& flags) const {
        if (!valid_out(i) || out_state_[size_t(i)] != OutClient) return false;
        time_us = out_data_[size_t(i)].time_us;
        flags = out_data_[size_t(i)].flags;
        return true;
    }

    // Returns the events to signal: a sample waiting for a free output buffer may now be decoded into this one.
    int release_output(int i) {
        if (!valid_out(i) || out_state_[size_t(i)] != OutClient) return 0;
        out_state_[size_t(i)] = OutFree;
        out_data_[size_t(i)] = {};
        return pump();
    }

    // What is pending, for RegisterClientEvents arriving after Start (nothing may be lost).
    int pending_events() const { return (in_list_.empty() ? 0 : kEvInput) | (out_list_.empty() ? 0 : kEvOutput); }

private:
    enum InState { InCodec, InOffered, InClient, InDecoding };
    enum OutState { OutFree, OutReady, OutClient };
    struct Sample {
        int64_t time_us = 0;
        uint32_t flags = 0;
    };

    bool valid_in(int i) const { return i >= 0 && i < inputs(); }
    bool valid_out(int i) const { return i >= 0 && i < outputs(); }

    // The codec's progress: every sample waiting to be decoded moves to a free output buffer and its input buffer is
    // offered again. The host decodes from the film itself at presentation time, so "decoding" is only this move.
    int pump() {
        int ev = 0;
        while (!dec_list_.empty()) {
            int o = -1;
            for (int k = 0; k < outputs(); k++)
                if (out_state_[size_t(k)] == OutFree) {
                    o = k;
                    break;
                }
            if (o < 0) break;
            const int in = dec_list_.front();
            dec_list_.pop_front();
            out_data_[size_t(o)] = in_data_[size_t(in)];
            out_state_[size_t(o)] = OutReady;
            out_list_.push_back(o);
            in_data_[size_t(in)] = {};
            in_state_[size_t(in)] = InOffered;
            in_list_.push_back(in);
            ev |= kEvInput | kEvOutput;
        }
        return ev;
    }

    void release_all() {
        for (size_t k = 0; k < out_state_.size(); k++) out_state_[k] = OutFree, out_data_[k] = {};
        for (size_t k = 0; k < in_state_.size(); k++) in_state_[k] = InCodec, in_data_[k] = {};
        in_list_.clear();
        dec_list_.clear();
        out_list_.clear();
    }

    std::vector<int> in_state_;
    std::vector<Sample> in_data_;
    std::vector<int> out_state_;
    std::vector<Sample> out_data_;
    std::deque<int> in_list_, dec_list_, out_list_;
    bool started_ = false, flushed_ = false, format_signalled_ = false;
};

}  // namespace nx::movie_host
