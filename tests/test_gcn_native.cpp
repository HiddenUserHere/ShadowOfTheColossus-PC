// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: ARCH-GCN-NATIVE offline test (no GPU, no game). Checks the GPU-visible layouts of
// shader_recompiler/gcn_layout.h and the hash that the host and the SPIR-V code must agree on:
//   ninja -C out/gpu gcn-native-test && out/gpu/gcn-native-test.exe
// The reference murmur3_32 below is written from the published algorithm, byte by byte, so it does not share code
// with the header.
// Part (b): a GCN compute program, hand-encoded with tests/gcn_encode.h, goes through the real recompiler entry
// (TranslateProgram, then EmitSPIRV) for three profiles, and a heap sample for two gcn_tex settings. The SPIR-V is
// written to argv[1] (default ./spv); validate it with `spirv-val --target-env vulkan1.3 <dir>/*.spv`. The miss path
// is checked here by counting its atomics.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include "common/elf_info.h"
// The recompiler headers carry warnings of the shadPS4 code (-Wunused-variable, -Wreorder-ctor, -Wunused-parameter);
// this test is -Werror, so they are muted for these includes only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wreorder-ctor"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/gcn_layout.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#pragma clang diagnostic pop
#include "gcn_encode.h"

// The recompiler asks for the game serial (SotC-only paths), as tests/test_barycentric_gs.cpp does.
namespace Core {
class Emulator {
public:
    static void SetSerial() {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = "CUSA08809";
    }
};
} // namespace Core

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace Shader::Gcn;

// Reference murmur3_32 (Austin Appleby's algorithm) over a byte array, little-endian 4-byte blocks.
static uint32_t Murmur3Ref(const std::vector<uint8_t>& key, uint32_t seed) {
    const uint32_t c1 = 0xCC9E2D51u;
    const uint32_t c2 = 0x1B873593u;
    uint32_t h = seed;
    const size_t nblocks = key.size() / 4;
    for (size_t b = 0; b < nblocks; ++b) {
        const uint8_t* p = key.data() + 4 * b;
        uint32_t k = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                     (uint32_t(p[3]) << 24);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5u + 0xE6546B64u;
    }
    // The key length is a multiple of 4 here: no tail.
    h ^= uint32_t(key.size());
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// The header hash over n words equals murmur3_32 over their bytes with seed ^ 0x9E3779B9.
static void CheckHash(const std::vector<uint32_t>& words, uint32_t seed) {
    std::vector<uint8_t> bytes;
    for (uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(uint8_t(w >> (8 * i)));
        }
    }
    const uint32_t expected = Murmur3Ref(bytes, seed ^ 0x9E3779B9u);
    const uint32_t actual = Hash(words.data(), uint32_t(words.size()), seed);
    if (actual != expected) {
        std::fprintf(stderr, "hash mismatch: n=%zu seed=0x%08x actual=0x%08x expected=0x%08x\n",
                     words.size(), seed, actual, expected);
        ++failures;
    }
}

static void HashTests() {
    // Empty key, single words, the T# and S# sizes, and the page-miss key.
    CheckHash({}, 0);
    CheckHash({0}, 0);
    CheckHash({0xFFFFFFFFu}, 0);
    CheckHash({1, 2}, kMissPage);
    CheckHash({0x12345678u, 0x9ABCDEF0u, 0, 0xFFFFFFFFu}, kMissSampler);
    std::vector<uint32_t> tsharp = {0x00000001u, 0x80000000u, 0x0000FFFFu, 0x40000000u,
                                    0xDEADBEEFu, 0x00C0FFEEu, 0x13579BDFu, 0x2468ACE0u};
    CheckHash(tsharp, ViewClass(false, false, false, 0));
    CheckHash(tsharp, ViewClass(true, true, true, 0xFFFFu));
    CheckHash(tsharp, SamplerClass(true, 0x3Fu, true));
    // Long input: many blocks, every bit pattern in the mix.
    std::vector<uint32_t> longkey;
    uint32_t x = 0x811C9DC5u;
    for (int i = 0; i < 257; ++i) {
        x = x * 0x01000193u ^ uint32_t(i);
        longkey.push_back(x);
    }
    CheckHash(longkey, 0xA5A5A5A5u);
    CheckHash(longkey, 0);
    // Seed 0x9E3779B9 cancels the constant: the reference must still match.
    CheckHash({7, 8, 9}, 0x9E3779B9u);
    // The seen bitmap index is a mask of the hash, so it is always below kSeenBits.
    const uint32_t h = Hash(tsharp.data(), 8, ViewClass(false, false, false, 0));
    CHECK((h & (kSeenBits - 1)) < kSeenBits);
}

static void MathTests() {
    CHECK(Rotl32(0x80000001u, 1) == 0x00000003u);
    // Rotl32 with r = 0 shifts by 32 (undefined): the shader and host only use 13 and 15, never 0.
    CHECK(Rotl32(0x12345678u, 8) == 0x34567812u);
}

static void ClassTests() {
    // Images: the top byte is never 0xFF (bits 8.. carry the bindless key below 2^16).
    CHECK(ViewClass(false, false, false, 0) == 0u);
    CHECK(ViewClass(true, false, false, 0) == 1u);
    CHECK(ViewClass(false, true, false, 0) == 2u);
    CHECK(ViewClass(false, false, true, 0) == 4u);
    CHECK(ViewClass(false, false, false, 0xFFFFFFFFu) == 0x00FFFF00u);
    CHECK(ViewClass(true, true, true, 0x1234u) == 0x00123407u);
    CHECK(!IsSamplerClass(ViewClass(true, true, true, 0xFFFFu)));
    // Samplers: fixed top byte, bits 0 (depth), 1..6 (post op), 7 (single lod).
    CHECK(SamplerClass(false, 0, false) == 0xFF000000u);
    CHECK(SamplerClass(true, 0, false) == 0xFF000001u);
    CHECK(SamplerClass(false, 0x3Fu, false) == 0xFF00007Eu);
    CHECK(SamplerClass(false, 0, true) == 0xFF000080u);
    CHECK(SamplerClass(true, 0x3Fu, true) == 0xFF0000FFu);
    // The post op is masked to six bits.
    CHECK(SamplerClass(false, 0xFFFFFFFFu, false) == 0xFF00007Eu);
    CHECK(IsSamplerClass(SamplerClass(true, 5, true)));
    CHECK(!IsSamplerClass(0x00FFFFFFu));
}

static void LayoutTests() {
    // Sizes fixed by the shader side (the emitted SPIR-V reads these offsets).
    static_assert(sizeof(GlobalsHeader) == 64);
    static_assert(sizeof(MissRingHeader) == 16);
    static_assert(sizeof(MissRecord) == 48);
    static_assert(sizeof(TexEntry) == 48);
    CHECK(sizeof(GlobalsHeader) == 64);
    CHECK(sizeof(MissRingHeader) == 16);
    CHECK(sizeof(MissRecord) == 48);
    CHECK(sizeof(TexEntry) == 48);

    // GlobalsHeader member offsets.
    CHECK(offsetof(GlobalsHeader, page_table_address) == 0);
    CHECK(offsetof(GlobalsHeader, page_table_guest_base) == 8);
    CHECK(offsetof(GlobalsHeader, page_table_size) == 16);
    CHECK(offsetof(GlobalsHeader, miss_ring_offset) == 24);
    CHECK(offsetof(GlobalsHeader, seen_offset) == 28);
    CHECK(offsetof(GlobalsHeader, tex_table_offset) == 32);
    CHECK(offsetof(GlobalsHeader, tex_table_mask) == 36);
    CHECK(offsetof(GlobalsHeader, null_image_slot) == 40);
    CHECK(offsetof(GlobalsHeader, flags) == 44);
    CHECK(offsetof(GlobalsHeader, reserved) == 48);

    // MissRecord member offsets (words start at byte 16).
    CHECK(offsetof(MissRecord, stamp) == 0);
    CHECK(offsetof(MissRecord, kind) == 4);
    CHECK(offsetof(MissRecord, view_class) == 8);
    CHECK(offsetof(MissRecord, reserved) == 12);
    CHECK(offsetof(MissRecord, words) == 16);

    // TexEntry member offsets.
    CHECK(offsetof(TexEntry, words) == 0);
    CHECK(offsetof(TexEntry, view_class) == 32);
    CHECK(offsetof(TexEntry, slot) == 36);
    CHECK(offsetof(TexEntry, state) == 40);
    CHECK(offsetof(TexEntry, reserved) == 44);

    // Byte layout of the globals buffer, with the literal numbers.
    CHECK(kMissRingCapacity == 4096);
    CHECK(kSeenBits == 65536);
    CHECK(kTexTableCapacity == 65536);
    CHECK(kTexMaxProbes == 16);
    CHECK(kMissRingOffset == 64);
    CHECK(kSeenOffset == 196688);     // 64 + 16 + 4096 * 48
    CHECK(kTexTableOffset == 204880); // + 65536 / 8
    CHECK(kGlobalsSize == 3350608);   // + 65536 * 48
    CHECK(kTexTableOffset % 16 == 0);
    CHECK(kGlobalsSize == kTexTableOffset + kTexTableCapacity * sizeof(TexEntry));

    // Enum values are part of the shader protocol.
    CHECK(kMissNone == 0 && kMissPage == 1 && kMissImage == 2 && kMissSampler == 3);
    CHECK(kTexEmpty == 0 && kTexLive == 1 && kTexTombstone == 2);
}

// ---------------------------------------------------------------------------------------------------------------
// Part (b): SPIR-V of a hand-encoded GCN compute program.

// Opcodes of the SPIR-V the emitter writes (SPIR-V headers, unified1 spirv.hpp11).
constexpr uint32_t kSpvOpAtomicIAdd = 234;
constexpr uint32_t kSpvOpAtomicOr = 241;
constexpr uint32_t kSpvMagic = 0x07230203u;

// Counts the instructions of one opcode in a SPIR-V module (the header is 5 words).
static uint32_t CountOpcode(const std::vector<uint32_t>& spv, uint32_t opcode) {
    uint32_t count = 0;
    size_t i = 5;
    while (i < spv.size()) {
        const uint32_t wordcount = spv[i] >> 16;
        if (wordcount == 0) {
            break; // malformed: stop instead of looping
        }
        if ((spv[i] & 0xFFFFu) == opcode) {
            ++count;
        }
        i += wordcount;
    }
    return count;
}

// A V# (raw buffer, 4 dwords): 40-bit base, stride 4 bytes, num_records, dst_sel RGBA, 32-bit UINT.
static std::array<uint32_t, 4> RawBufferSharp(uint64_t base, uint32_t num_records) {
    const uint32_t dw1 = (uint32_t(base >> 32) & 0xFFu) | (4u << 16); // stride in bits 16..29
    const uint32_t dw3 = 4u | (5u << 3) | (6u << 6) | (7u << 9) |     // dst_sel X,Y,Z,W = R,G,B,A
                         (4u << 12) | (4u << 15);                     // num_format UINT, data_format 32
    return {uint32_t(base), dw1, num_records, dw3};
}

// The compute program: the V#s of the source and destination buffers are loaded from a table at the pointer in
// s[0:1] (a V# in the user SGPRs alone is not a flat buffer sharp, and the shader-address path needs one), then one
// dword is copied from the source buffer to the destination buffer at byte offset v0.
static std::vector<uint32_t> ComputeCopyCode() {
    GcnEnc::Code c;
    GcnEnc::SMovB32Literal(c, GcnEnc::kVccHi, 0);  // the prologue the translator expects
    GcnEnc::SLoadDwordx4(c, 8, 0, 0);              // s_load_dwordx4 s[8:11], s[0:1], 0 (source V#)
    GcnEnc::SLoadDwordx4(c, 12, 0, 4);             // s_load_dwordx4 s[12:15], s[0:1], 4 (destination V#)
    // A plain dword from the table, used as data: its read is a ReadConst that stays in the IR, so the shader keeps
    // the flat buffer (has_flatbuf), which the shader-address path needs. The sharps alone do not keep it.
    GcnEnc::SLoad(c, GcnEnc::kSmrdLoadDword, 16, 0, 8); // s_load_dword s16, s[0:1], 8
    GcnEnc::SWaitcnt(c, 0, 7, 0);                  // s_waitcnt vmcnt(0) lgkmcnt(0)
    GcnEnc::BufferLoadDword(c, 1, 0, 2);           // buffer_load_dword v1, v0, s[8:11], offen
    GcnEnc::VMovB32(c, 2, 16);                     // v_mov_b32 v2, s16
    GcnEnc::SWaitcnt(c, 0, 7, 0);                  // s_waitcnt vmcnt(0)
    GcnEnc::BufferStoreDword(c, 1, 0, 3, 0);       // buffer_store_dword v1, v0, s[12:15], offen
    GcnEnc::BufferStoreDword(c, 2, 0, 3, 4);       // buffer_store_dword v2, v0, s[12:15], offen offset:4
    GcnEnc::SEndpgm(c);
    return c;
}

static Shader::Profile ComputeProfile(uint8_t shader_addr, uint8_t gcn_buf) {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600; // SPIR-V 1.6, as the pipeline cache requests
    profile.max_shared_memory_size = 65536;
    profile.subgroup_size = 32;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    profile.support_float16 = true;
    profile.support_float64 = true;
    profile.shader_addr = shader_addr;
    profile.gcn_buf = gcn_buf;
    return profile;
}

// The recompiler sequence of PipelineCache::CompileModule for a compute stage: Info from the params, RuntimeInfo,
// TranslateProgram, EmitSPIRV. The user data is the 16 user SGPRs; the flat buffer is filled by the translation.
// `variant` is the stage variant (Shader::StageVariantHeap for the heap stage); `heap_stage` (optional) receives the
// answer of Info::bindless.heap_stage, the heap decision of the finished Info.
static std::vector<uint32_t> CompileCompute(const std::vector<uint32_t>& code,
                                            const std::array<uint32_t, 16>& user_data,
                                            const Shader::Profile& profile, uint8_t variant = 0,
                                            bool* heap_stage = nullptr) {
    Shader::ShaderParams params{
        .user_data = std::span<const uint32_t, 16>{user_data},
        .code = code,
        .hash = 0x5107C0DEull,
        .variant = variant,
    };
    Shader::Info info{Shader::HwStage::Compute, Shader::SwStage::Compute, params};
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    runtime.props.num_user_data = 16;
    runtime.props.num_allocated_vgprs = 8;
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Shader::Pools pools;
    const Shader::IR::Program program = Shader::TranslateProgram(code, pools, info, runtime, profile);
    if (heap_stage) {
        *heap_stage = program.info.bindless.heap_stage;
    }
    Shader::Backend::Bindings binding{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
}

static void ComputeTests(const std::filesystem::path& dir) {
    const std::vector<uint32_t> code = ComputeCopyCode();
    // The table: V# of the source buffer at dword 0, V# of the destination at dword 4. The table lives in host
    // memory: the flattening walker reads it through the pointer in s[0:1] (48-bit host address here).
    alignas(16) static std::array<uint32_t, 16> table{};
    const auto src = RawBufferSharp(0x0700000000ull, 1024);
    const auto dst = RawBufferSharp(0x0700100000ull, 1024);
    for (size_t i = 0; i < 4; ++i) {
        table[i] = src[i];
        table[4 + i] = dst[i];
    }
    table[8] = 0x1234ABCDu; // the dword read as data
    // User data: s[0:1] = the table pointer, the rest unused.
    std::array<uint32_t, 16> user_data{};
    const uint64_t table_ptr = reinterpret_cast<uintptr_t>(table.data());
    user_data[0] = uint32_t(table_ptr);
    user_data[1] = uint32_t(table_ptr >> 32);

    struct Case {
        const char* name;
        uint8_t shader_addr;
        uint8_t gcn_buf;
        bool miss_path; // table miss code (GcnPageMiss) expected
    };
    constexpr Case cases[] = {
        {"compute_addr3_buf1", 3, 1, true},
        {"compute_addr3_buf0", 3, 0, false},
        {"compute_addr0", 0, 0, false},
    };
    for (const Case& test : cases) {
        const Shader::Profile profile = ComputeProfile(test.shader_addr, test.gcn_buf);
        const std::vector<uint32_t> spv = CompileCompute(code, user_data, profile);
        const uint32_t or_count = CountOpcode(spv, kSpvOpAtomicOr);
        const uint32_t add_count = CountOpcode(spv, kSpvOpAtomicIAdd);
        std::printf("%s: %zu words, OpAtomicOr %u, OpAtomicIAdd %u\n", test.name, spv.size(), or_count,
                    add_count);
        CHECK(spv.size() > 5 && spv[0] == kSpvMagic);
        if (test.miss_path) {
            // Each table access (load and store) appends a page miss: an atomic OR on the seen bitmap and an
            // atomic IAdd on the ring head.
            CHECK(or_count >= 1);
            CHECK(add_count >= 1);
        } else {
            CHECK(or_count == 0);
            CHECK(add_count == 0);
        }
        const auto path = dir / (std::string(test.name) + ".spv");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * sizeof(uint32_t)));
        CHECK(bool(out));
    }
}

// The heap stage: one image_sample of a 2D T# and an S#, both read from the table, the result stored to a buffer. A
// compute stage with the heap variant is the heap stage of its pipeline (RefreshComputeKey), so this stands in for a
// fragment heap stage (the fragment one needs interpolants and an export).
static std::vector<uint32_t> HeapSampleCode() {
    GcnEnc::Code c;
    GcnEnc::SMovB32Literal(c, GcnEnc::kVccHi, 0);
    GcnEnc::SLoad(c, 3, 16, 0, 0);       // s_load_dwordx8 s[16:23], s[0:1], 0 (T#, dwords 0..7)
    GcnEnc::SLoadDwordx4(c, 24, 0, 8);   // s_load_dwordx4 s[24:27], s[0:1], 8 (S#, dwords 8..11)
    GcnEnc::SLoadDwordx4(c, 28, 0, 12);  // s_load_dwordx4 s[28:31], s[0:1], 12 (destination V#)
    // A dword read as data (table dword 16), so the flat buffer exists: with only sharps from the table, the gcn_tex
    // probe (HoistGcnTex) reads them from a flat buffer that the shader does not have (see the report).
    GcnEnc::SLoad(c, GcnEnc::kSmrdLoadDword, 32, 0, 16);
    GcnEnc::SWaitcnt(c, 0, 7, 0);
    // image_sample_lz v[4:7], v[8:9], s[16:23], s[24:27] dmask:0xf (LOD 0: valid in a compute stage)
    GcnEnc::Mimg(c, GcnEnc::kImageSampleLz, 0xF, 8, 4, 4, 6);
    GcnEnc::VMovB32(c, 3, 32);                   // v_mov_b32 v3, s32
    GcnEnc::SWaitcnt(c, 0, 7, 0);
    GcnEnc::BufferStoreDword(c, 4, 0, 7, 0);     // buffer_store_dword v4, v0, s[28:31], offen
    GcnEnc::BufferStoreDword(c, 3, 0, 7, 4);     // buffer_store_dword v3, v0, s[28:31], offen offset:4
    GcnEnc::SEndpgm(c);
    return c;
}

static void HeapTests(const std::filesystem::path& dir) {
    // The heap switches of the instance (Instance::CreateDevice sets them before any shader).
    Shader::SetBindlessImagesEnabled(true);
    Shader::SetBindlessSamplersEnabled(true);
    const std::vector<uint32_t> code = HeapSampleCode();

    alignas(16) static std::array<uint32_t, 32> table{};
    table[16] = 0x55AAu; // the dword read as data
    // T# (8 dwords): base >> 8 (38-bit field), data_format 8_8_8_8 and num_format UNORM in dword 1 (format at bit 20,
    // number at bit 26), width-1 and height-1, dst_sel RGBA and type 2D (9) in dword 3.
    table[0] = uint32_t(0x0700000000ull >> 8);
    table[1] = (10u << 20);
    table[2] = 1023u | (1023u << 14);
    table[3] = 4u | (5u << 3) | (6u << 6) | (7u << 9) | (9u << 28);
    // dwords 4..7 of the T# and the S# (dwords 8..11) stay zero: default filtering and wrap.
    const auto dst = RawBufferSharp(0x0700100000ull, 1024);
    for (size_t i = 0; i < 4; ++i) {
        table[12 + i] = dst[i];
    }
    std::array<uint32_t, 16> user_data{};
    const uint64_t table_ptr = reinterpret_cast<uintptr_t>(table.data());
    user_data[0] = uint32_t(table_ptr);
    user_data[1] = uint32_t(table_ptr >> 32);

    struct Case {
        const char* name;
        uint8_t gcn_tex;
    };
    constexpr Case cases[] = {
        {"heap_gcntex0", 0},
        {"heap_gcntex1", 1},
    };
    for (const Case& test : cases) {
        Shader::Profile profile = ComputeProfile(0, 0);
        profile.heap_images = 1;
        profile.bindless_images = true;
        profile.gcn_tex = test.gcn_tex;
        bool heap_stage = false;
        const std::vector<uint32_t> spv =
            CompileCompute(code, user_data, profile, Shader::StageVariantHeap, &heap_stage);
        std::printf("%s: %zu words, heap stage %d\n", test.name, spv.size(), int(heap_stage));
        CHECK(spv.size() > 5 && spv[0] == kSpvMagic);
        CHECK(heap_stage);
        const auto path = dir / (std::string(test.name) + ".spv");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * sizeof(uint32_t)));
        CHECK(bool(out));
    }
}

int main(int argc, char** argv) {
    LayoutTests();
    MathTests();
    ClassTests();
    HashTests();
    const std::filesystem::path dir = argc > 1 ? argv[1] : "spv";
    std::filesystem::create_directories(dir);
    Core::Emulator::SetSerial();
    ComputeTests(dir);
    HeapTests(dir);
    if (failures != 0) {
        std::fprintf(stderr, "gcn-native-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("gcn-native-test: all checks passed\n");
    return 0;
}
