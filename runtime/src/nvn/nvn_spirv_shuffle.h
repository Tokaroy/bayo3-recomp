// SPDX-License-Identifier: GPL-3.0-or-later
// Subgroup shuffle as shuffle-xor, for Qualcomm's Adreno 750 shader compiler (issue #5).
//
// The Adreno 750 driver of the 512.762 series crashes inside its shader compiler (libllvm-qgl.so, CreateQGLCShader,
// SIGSEGV at address 0x48) on modules that use OpGroupNonUniformShuffle. Other projects hit the same signature: a
// single subgroupShuffle in a compute shader crashes it, while subgroupShuffleXor, subgroupBroadcast and subgroupAdd
// compile (github.com/musetric/musetric issue 1139), and d2r-mobile issue 123 / S2Arm issue 358 crash the same way on
// shuffles inside helper functions. The game's Maxwell SHFL becomes such a shuffle in 1326 of its 5719 translated
// shaders, so skipping the shader that crashes would only move the crash to the next one.
//
// Shuffle(value, id) reads lane `id`; ShuffleXor(value, mask) reads lane `self ^ mask`. With mask = id ^ self the
// lane is the same, so every shuffle becomes
//     %self = OpLoad %uint %SubgroupLocalInvocationId
//     %mask = OpBitwiseXor %uint %id %self
//     %r    = OpGroupNonUniformShuffleXor %T %scope %value %mask
// Both instructions need the same capability (GroupNonUniformShuffle). The translator's SHFL helper already declares
// and reads SubgroupLocalInvocationId; a module that uses a shuffle without declaring it is left alone.
//
// Pure (no Vulkan, no logging): tests/spirv_shuffle_xor_test.cpp drives it.
#pragma once

#include <cstdint>
#include <vector>

namespace nx::nvn {

// Rewrites every OpGroupNonUniformShuffle of `in` into `out`. Returns the number rewritten; 0 leaves `out` untouched
// (no shuffle, a malformed module, or no SubgroupLocalInvocationId variable to read the lane from).
inline int patch_shuffle_xor(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    enum : uint32_t {
        OpTypeInt = 21, OpTypePointer = 32, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpBitwiseXor = 198,
        OpShuffle = 345, OpShuffleXor = 346, DecorationBuiltIn = 11, BuiltInSubgroupLocalInvocationId = 41,
        StorageInput = 1,
    };
    if (in.size() < 5 || in[0] != 0x07230203u) return 0;
    uint32_t lane_var = 0, shuffles = 0;
    std::vector<uint32_t> pointer_of;  // indexed by id: the pointee type of an OpTypePointer, 0 otherwise
    std::vector<uint32_t> var_type;    // indexed by id: the pointer type of an Input OpVariable, 0 otherwise
    std::vector<bool> is_uint32;       // indexed by id: OpTypeInt 32 0
    const uint32_t bound = in[3];
    if (bound == 0 || bound > (1u << 22)) return 0;
    pointer_of.assign(bound, 0);
    var_type.assign(bound, 0);
    is_uint32.assign(bound, false);
    for (size_t i = 5; i < in.size();) {
        const uint32_t n = in[i] >> 16, op = in[i] & 0xffff;
        if (n == 0 || n > in.size() - i) return 0;
        const uint32_t* w = &in[i];
        if (op == OpDecorate && n >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInSubgroupLocalInvocationId)
            lane_var = w[1];
        else if (op == OpTypeInt && n == 4 && w[1] < bound && w[2] == 32 && w[3] == 0)
            is_uint32[w[1]] = true;
        else if (op == OpTypePointer && n == 4 && w[1] < bound && w[2] == StorageInput)
            pointer_of[w[1]] = w[3];
        else if (op == OpVariable && n >= 4 && w[2] < bound && w[3] == StorageInput)
            var_type[w[2]] = w[1];
        else if (op == OpShuffle) {
            if (n != 6) return 0;
            shuffles++;
        }
        i += n;
    }
    if (!shuffles || !lane_var || lane_var >= bound) return 0;
    const uint32_t ptr = var_type[lane_var];
    const uint32_t uint_type = ptr < bound ? pointer_of[ptr] : 0;
    if (!uint_type || uint_type >= bound || !is_uint32[uint_type]) return 0;

    std::vector<uint32_t> o;
    o.reserve(in.size() + size_t(shuffles) * 9);  // each rewrite adds a 4-word load and a 5-word xor
    o.insert(o.end(), in.begin(), in.begin() + 5);
    uint32_t next = bound;
    int rewritten = 0;
    for (size_t i = 5; i < in.size();) {
        const uint32_t n = in[i] >> 16, op = in[i] & 0xffff;
        const uint32_t* w = &in[i];
        if (op == OpShuffle) {
            // OpGroupNonUniformShuffle %type %result %scope %value %id
            const uint32_t self = next++, mask = next++;
            o.insert(o.end(), {(4u << 16) | OpLoad, uint_type, self, lane_var});
            o.insert(o.end(), {(5u << 16) | OpBitwiseXor, uint_type, mask, w[5], self});
            o.insert(o.end(), {(6u << 16) | OpShuffleXor, w[1], w[2], w[3], w[4], mask});
            rewritten++;
        } else {
            o.insert(o.end(), w, w + n);
        }
        i += n;
    }
    o[3] = next;
    out.swap(o);
    return rewritten;
}

}  // namespace nx::nvn
