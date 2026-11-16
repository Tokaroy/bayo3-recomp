// The host movie::Decoder's index protocol (runtime/src/movie_host_queue.h), without the game.
#include <cstdio>
#include <cstdlib>

#include "movie_host_queue.h"

using namespace nx::movie_host;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

int main() {
    {  // nothing is offered before Start; the first Start offers every input and asks for the format event once
        Queue q(2, 2);
        int i, more;
        check(q.acquire_input(i, more) == kNoInput && i == -1, "no input before Start");
        check(q.start() == (kEvInput | kEvFormat), "first Start: input + format");
        check(q.start() == 0, "a second Start offers nothing and no second format event");
        check(q.acquire_input(i, more) == kOk && i == 0 && more == 1, "first input");
        int j;
        check(q.acquire_input(j, more) == kOk && j == 1 && more == 0, "second input");
        check(q.acquire_input(j, more) == kNoInput, "inputs exhausted");
    }
    {  // a sample becomes a frame with its time; its input is offered again; release frees the output
        Queue q(2, 1);
        q.start();
        int a, b, more, ev;
        q.acquire_input(a, more);
        q.acquire_input(b, more);
        check(q.send_input(a, 100, 33333, 0, ev) == kOk && ev == (kEvInput | kEvOutput), "decoded: input + output");
        check(q.send_input(b, 100, 66666, 0, ev) == kOk && ev == 0, "no free output: waits");
        int o;
        int64_t t;
        uint32_t f;
        check(q.acquire_output(o, t, f, more) == kOk && t == 33333 && more == 0, "first frame");
        int none;
        check(q.acquire_output(none, t, f, more) == kNoOutput && none == -1, "second still waiting");
        int64_t ht;
        uint32_t hf;
        check(q.held_output(o, ht, hf) && ht == 33333, "held output");
        check(q.release_output(o) == (kEvInput | kEvOutput), "release lets the waiting sample through");
        check(!q.held_output(o, ht, hf), "released output is not held");
        check(q.acquire_output(o, t, f, more) == kOk && t == 66666, "second frame");
    }
    {  // an empty sample is offered back; the end of stream is a frame with the flag
        Queue q(1, 2);
        q.start();
        int i, more, ev, o;
        int64_t t;
        uint32_t f;
        q.acquire_input(i, more);
        check(q.send_input(i, 0, 0, 0, ev) == kOk && ev == kEvInput, "empty sample: buffer offered again");
        check(q.acquire_output(o, t, f, more) == kNoOutput, "no frame from an empty sample");
        q.acquire_input(i, more);
        check(q.send_input(i, 0, 99, kFlagEos, ev) == kOk && (ev & kEvOutput), "EOS queued");
        check(q.acquire_output(o, t, f, more) == kOk && (f & kFlagEos), "EOS frame");
    }
    {  // Flush voids held indices; a stale index is refused; Start after Flush offers everything again
        Queue q(2, 2);
        q.start();
        int i, more, ev, o;
        int64_t t;
        uint32_t f;
        q.acquire_input(i, more);
        q.send_input(i, 10, 1, 0, ev);
        q.acquire_output(o, t, f, more);
        q.halt(false);
        check(!q.holds_input(i) && !q.held_output(o, t, f), "Flush voids everything");
        check(q.send_input(i, 10, 2, 0, ev) == kInvalid, "stale input refused");
        check(q.release_output(o) == 0, "stale output release ignored");
        check(q.acquire_input(i, more) == kNoInput, "nothing offered until Start");
        check(q.start() == kEvInput, "Start after Flush offers again, no second format event");
        check(q.acquire_input(i, more) == kOk && more == 1, "both inputs back");
        check(q.pending_events() == kEvInput, "pending: input");
    }
    {  // Stop then Start
        Queue q(1, 1);
        q.start();
        q.halt(true);
        check(q.start() == kEvInput, "Start after Stop offers again");
    }
    std::puts("movie_host_queue: ok");
    return 0;
}
