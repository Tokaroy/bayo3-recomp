// SPDX-License-Identifier: GPL-3.0-or-later
// A host movie::Decoder for the game's video tracks, in the manner of Witch-Time's runtime/nnmovie_mf.c
// (docs/WITCH-TIME-NOTICES.md): the game's player keeps driving its Decoder objects, but a video decoder's slots are
// answered here, and its frames come from the host decoder of the film (movie.cpp: ffmpeg, Media Foundation or
// MediaCodec). The game's own decoder below that layer (NvMMLite on the faked nvdec/vic channels) never runs for
// video. Audio decoders keep the game's own implementation. NX_MOVIE_DECODER=game turns this off.
#pragma once

#include <cstdint>

namespace nx {
struct CPU;
}

namespace nx::movie_host {

bool enabled();

// Called by the movie::Decoder thunk before the game's implementation. True: the call was answered here and x0 holds
// its result; false: the game's implementation runs as usual (an audio decoder, or a destructor).
bool decoder_call(CPU& c, int slot);

// A movie controller seen at SetDataSource (the reader is controller+8): its decoder is at +0x160 and the film's
// file handle at +0x18, which is how a decoder finds the film it decodes.
void note_controller(uint64_t controller);

}  // namespace nx::movie_host
