// tests for runtime/src/nvn/nvn_spirv_shuffle.h: OpGroupNonUniformShuffle -> load lane, xor, OpGroupNonUniformShuffleXor.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "nvn/nvn_spirv_shuffle.h"

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

// Ids: 1 uint, 2 ptr Input uint, 3 lane var, 4 float, 5 scope const, 6 value, 7 lane id, 8 shuffle result,
// 9 void, 10 fn type, 11 fn, 12 label. Bound 13. Not a valid program, only the shapes the patch reads.
std::vector<uint32_t> module(bool lane_builtin, int shuffles) {
    std::vector<uint32_t> m = {0x07230203u, 0x00010300u, 0, 13, 0};
    auto op = [&](uint32_t code, std::initializer_list<uint32_t> operands) {
        m.push_back((uint32_t(operands.size() + 1) << 16) | code);
        m.insert(m.end(), operands);
    };
    op(17, {1});   // OpCapability Shader
    op(17, {65});  // OpCapability GroupNonUniformShuffle
    if (lane_builtin) op(71, {3, 11, 41});  // OpDecorate %3 BuiltIn SubgroupLocalInvocationId
    op(21, {1, 32, 0});    // %1 = OpTypeInt 32 0
    op(32, {2, 1, 1});     // %2 = OpTypePointer Input %1
    op(59, {2, 3, 1});     // %3 = OpVariable %2 Input
    op(22, {4, 32});       // %4 = OpTypeFloat 32
    op(19, {9});           // %9 = OpTypeVoid
    op(33, {10, 9});       // %10 = OpTypeFunction %9
    op(54, {9, 11, 0, 10});  // %11 = OpFunction
    op(248, {12});           // %12 = OpLabel
    for (int k = 0; k < shuffles; k++) op(345, {4, 8, 5, 6, 7});  // %8 = OpGroupNonUniformShuffle %4 %5 %6 %7
    op(253, {});  // OpReturn
    op(56, {});   // OpFunctionEnd
    return m;
}

}  // namespace

int main() {
    using nx::nvn::patch_shuffle_xor;
    {
        const auto in = module(true, 1);
        std::vector<uint32_t> out;
        check(patch_shuffle_xor(in, out) == 1, "one shuffle rewritten");
        check(out.size() == in.size() + 9, "a 4-word load and a 5-word xor added");
        check(out[3] == 15, "the id bound grows by two");
        // Find the rewritten sequence.
        size_t at = 0;
        for (size_t i = 5; i < out.size(); i += out[i] >> 16)
            if ((out[i] & 0xffff) == 61) at = i;
        check(at && out[at] == ((4u << 16) | 61) && out[at + 1] == 1 && out[at + 2] == 13 && out[at + 3] == 3,
              "OpLoad %uint %13 %lane");
        const size_t x = at + 4;
        check(out[x] == ((5u << 16) | 198) && out[x + 1] == 1 && out[x + 2] == 14 && out[x + 3] == 7 &&
                  out[x + 4] == 13,
              "OpBitwiseXor %uint %14 %id %13");
        const size_t s = x + 5;
        check(out[s] == ((6u << 16) | 346) && out[s + 1] == 4 && out[s + 2] == 8 && out[s + 3] == 5 &&
                  out[s + 4] == 6 && out[s + 5] == 14,
              "OpGroupNonUniformShuffleXor %float %8 %scope %value %14");
        bool left = false;
        for (size_t i = 5; i < out.size(); i += out[i] >> 16) left |= (out[i] & 0xffff) == 345;
        check(!left, "no OpGroupNonUniformShuffle left");
    }
    {
        std::vector<uint32_t> out;
        check(patch_shuffle_xor(module(true, 3), out) == 3 && out[3] == 19, "three shuffles, six new ids");
    }
    {
        std::vector<uint32_t> out = {42};
        check(patch_shuffle_xor(module(true, 0), out) == 0 && out.size() == 1, "no shuffle: untouched");
        check(patch_shuffle_xor(module(false, 1), out) == 0 && out.size() == 1, "no lane builtin: untouched");
        std::vector<uint32_t> bad = module(true, 1);
        bad.back() = 0;  // a zero-length instruction
        check(patch_shuffle_xor(bad, out) == 0 && out.size() == 1, "malformed module: untouched");
        check(patch_shuffle_xor({1, 2, 3}, out) == 0, "not SPIR-V: untouched");
    }
    if (failures) return 1;
    std::printf("spirv_shuffle_xor: ok\n");
    return 0;
}
