// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PRETRANSLATE=1 (off by default). Step 2c of translating the game's shaders at area load: every pixel
// and vertex program of a shader file the engine's loader hands on (RES_LOADED, probe_csdr.cpp, needs
// SOTC_CSDR_PROBE=1 for the hook) is translated on a pipeline worker before its first draw, with the register state
// its file declares (SotcPredict::NoteLoadedData) and synthetic sharps (Info::sotc_synthetic). The program is
// installed in the program cache; a draw whose key equals the pre-translated one finds it (a hit), any other key
// translates a permutation as before (a miss). The report line counts both, with the key parts of the misses.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Shader {
struct Info;
struct StageSpecialization;
} // namespace Shader

namespace SotcPretranslate {

/// SOTC_PRETRANSLATE=1 or SOTC_PRECOMPILE=1, read once. True also with SOTC_PRETRANSLATE_COMPUTE=1 alone (compute
/// programs only; the pixel and vertex requests stay off without SOTC_PRETRANSLATE).
bool On() noexcept;
/// SOTC_PRETRANSLATE=1 or SOTC_PRECOMPILE=1: the pixel and vertex requests (and their backlog, hold and report).
bool PixelVertexOn() noexcept;
/// SOTC_PRETRANSLATE_COMPUTE=1 (default off, also set by SOTC_PRECOMPILE=1 with SOTC_PRETRANSLATE_COMPUTE=1):
/// compute programs of the loaded shader files are prebuilt (translated, and their ComputePipeline built on a worker)
/// under the key their first dispatch computes. Needs SOTC_RES_LOADED=1 (sotc.py sets it for the switches it knows).
bool ComputeOn() noexcept;

/// SOTC_PRECOMPILE=1 (sotc.py --precompile): the shader preparation run. Every program the game loads at boot is
/// translated (all its variants) and written to the shader cache, then the port leaves by itself (PipelineCache,
/// DrainPretranslate). A later session finds the programs in the cache index and creates their modules at their
/// first draw (PipelineCache::LoadLazyProgram).
bool Precompile() noexcept;

/// Jobs queued and finished so far (the precompile run waits for them), and the time of the last request.
uint64_t JobsQueued() noexcept;
uint64_t JobsDone() noexcept;
int64_t LastRequestNs() noexcept;

struct Request {
    uint64_t hash{};
    uint8_t stage{}; ///< 0 pixel, 1 vertex, 2 compute
    std::vector<uint32_t> code;
    uint32_t rsrc1{}, rsrc2{};
    // Pixel: SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, the interpolant count, the interpolated attributes and the VS
    // param each input slot reads (SotcPredict step 2a).
    uint32_t ena{}, addr{}, num_inputs{};
    uint64_t interp_mask{};
    std::array<uint8_t, 32> param{};
    /// The program descriptor's dwords +16..+31: the stage registers in the Orbis order (predict_probe.cpp).
    std::array<uint32_t, 16> regs{};
    /// Vertex: the pixel program of its shader file (0 when the file has none or several). Its bindings come first
    /// in the pipeline, so the vertex stage's start bindings follow from that program.
    uint64_t pair{};
    /// Command processor: when the request was first deferred (a vertex request waiting for its pixel program).
    int64_t deferred_ns{};
    /// Compute: the register layout that passed the validity test (ComputeLayoutOf); 0 = none (not requested).
    uint8_t cs_layout{};
};

/// Compute: the stage registers of a compute program's descriptor (predict_probe.cpp). The descriptor dwords +16..+23
/// hold the registers in one of two orders (ComputeLayoutOf): layout 1 is Lo, Hi, Rsrc1, Rsrc2, NumThreadX/Y/Z;
/// layout 2 is Lo, Rsrc1, Rsrc2, NumThreadX/Y/Z. The one that passes the validity test is used. Thread counts are the
/// low 16 bits of each register (NUM_THREAD_FULL).
struct ComputeFields {
    uint32_t rsrc1{}, rsrc2{}, nx{}, ny{}, nz{};
};
/// 1 or 2 for the layout whose fields are valid (threads 1..1024 each, product <= 1024, user SGPRs <= 16), 0 for none.
/// ambiguous (when set) is true when both layouts are valid.
inline uint8_t ComputeLayoutOf(const std::array<uint32_t, 16>& regs, bool* ambiguous = nullptr) {
    const auto valid = [](uint32_t rsrc2, uint32_t x, uint32_t y, uint32_t z) {
        const uint32_t tx = x & 0xFFFFu, ty = y & 0xFFFFu, tz = z & 0xFFFFu;
        if (tx < 1 || ty < 1 || tz < 1 || tx > 1024 || ty > 1024 || tz > 1024 || tx * ty * tz > 1024) {
            return false;
        }
        return ((rsrc2 >> 1) & 0x1Fu) <= 16;
    };
    const bool one = valid(regs[3], regs[4], regs[5], regs[6]);
    const bool two = valid(regs[2], regs[3], regs[4], regs[5]);
    if (ambiguous) {
        *ambiguous = one && two;
    }
    return one ? 1 : two ? 2 : 0;
}
inline ComputeFields ComputeFieldsOf(const std::array<uint32_t, 16>& regs, uint8_t layout) {
    if (layout == 1) {
        return {regs[2], regs[3], regs[4] & 0xFFFFu, regs[5] & 0xFFFFu, regs[6] & 0xFFFFu};
    }
    return {regs[1], regs[2], regs[3] & 0xFFFFu, regs[4] & 0xFFFFu, regs[5] & 0xFFFFu};
}

/// Loader thread: queues a request (a hash already queued once is ignored).
void Push(Request&& request) noexcept;
/// Command processor: true when requests wait.
bool Pending() noexcept;
/// Command processor: the waiting requests.
std::vector<Request> Take();

/// Key parts of a miss (NoteFirstUse diff bits).
enum DiffBit : uint32_t {
    DiffProps = 1u << 0,      ///< RuntimeInfo props (user data count, VGPRs, float modes)
    DiffLayout = 1u << 1,     ///< pixel color targets, MRTZ mask, samples
    DiffPsInputs = 1u << 2,   ///< pixel input enables, count, slots
    DiffPsOther = 1u << 3,    ///< other pixel fields (Z export, early Z, dual source, clip emulation, front face)
    DiffVsHw = 1u << 4,       ///< vertex outputs, clip settings
    DiffVsSw = 1u << 5,       ///< vertex step rates, vertex/instance SGPR offsets
    DiffFetch = 1u << 6,      ///< fetch shader and vertex attributes
    DiffBuffers = 1u << 7,
    DiffImages = 1u << 8,
    DiffSamplers = 1u << 9,
    DiffStart = 1u << 10,     ///< start bindings
    DiffFmasks = 1u << 11,
    // The pixel fields of DiffPsOther, one by one.
    DiffPsZExport = 1u << 12,
    DiffPsFrontFace = 1u << 13,
    DiffPsDualSource = 1u << 14,
    DiffPsClip = 1u << 15,
    DiffPsEarlyZ = 1u << 16,
    // The image fields of DiffImages, one by one.
    DiffImgType = 1u << 17,
    DiffImgInteger = 1u << 18,
    DiffImgSrgb = 1u << 19,
    DiffImgSwizzle = 1u << 20,
    DiffImgConversion = 1u << 21,
    DiffImgBindings = 1u << 22,
    DiffImgStorage = 1u << 23,
    // The vertex fields of DiffVsHw and DiffVsSw, one by one.
    DiffVsOutputs = 1u << 24,
    DiffVsClipDisable = 1u << 25,
    DiffVsDepthEmulation = 1u << 26,
    DiffVsUserClip = 1u << 27,
    DiffVsStepRate = 1u << 28,
    DiffVsVertexSgpr = 1u << 29,
    DiffVsInstanceSgpr = 1u << 30,
    DiffVsTessEmulated = 1u << 31,
    DiffBits = 32,
};

/// Counters (stage 0 pixel, 1 vertex).
void NoteQueued(int stage) noexcept;
void NoteDropped(int stage, int reason) noexcept; ///< 0 known program, 1 no pair yet (given up), 2 SOTC_SHADER_ADDR on,
                                                  ///< 3 a profile the prebuild does not cover (heap)
/// Compute (stage 2): the register layout of a predicted program (0 none, 1, 2) and whether both layouts passed.
void NoteComputeLayout(uint8_t layout, bool ambiguous) noexcept;
/// Compute: the predicted registers against the first dispatch's (true = equal).
void NoteVerify(bool match) noexcept;
void NoteBuilt(int stage, bool ok) noexcept;
void NoteSuperseded(int stage) noexcept;           ///< a draw needed it first: translated normally
void NoteFirstUse(int stage, bool hit, uint32_t diff) noexcept;

// sotc: SOTC_PRETRANSLATE_COMPUTE_HINTS (default on; 0 = off; needs SOTC_PRETRANSLATE_COMPUTE). A compute program's
// synthetic sharps (resource_patching_pass.cpp) take the raw T#/V# fields its first real dispatch had, learned at a
// miss of that program and kept in user/shader_predict/compute_hints.tsv (written by the port; read at the first use).
// The file's lines: "hash<TAB>P" starts a program's hint (it replaces the earlier lines of that hash), then one line
// per binding: "hash<TAB>I<TAB>key<TAB>type<TAB>num_format<TAB>data_format<TAB>dst x y z w" for an image and
// "hash<TAB>B<TAB>key<TAB>stride<TAB>swizzle<TAB>index_stride<TAB>element_size<TAB>num_format<TAB>data_format<TAB>dst
// x y z w" for a buffer. The key is the flat-buffer dword of the binding's first sharp dword (HintKeyOf).
template <typename Fetch>
inline uint64_t HintKeyOf(const Fetch& fetch) {
    for (uint32_t i = 0; i < fetch.offsets.size(); ++i) {
        if ((fetch.load_mask >> i) & 1) {
            return uint64_t{static_cast<uint32_t>(fetch.offsets[i])} - i;
        }
    }
    return ~uint64_t{0};
}
struct HintImage {
    uint64_t key{};
    uint8_t type{}, num_format{}, data_format{};
    std::array<uint8_t, 4> dst_sel{};
    bool operator==(const HintImage&) const = default;
};
struct HintBuffer {
    uint64_t key{};
    uint16_t stride{};
    uint8_t swizzle{}, index_stride{}, element_size{}, num_format{}, data_format{};
    std::array<uint8_t, 4> dst_sel{};
    bool operator==(const HintBuffer&) const = default;
};
struct Hint {
    std::vector<HintImage> images;
    std::vector<HintBuffer> buffers;
    bool operator==(const Hint&) const = default;
};
using HintPtr = std::shared_ptr<const Hint>;

/// SOTC_PRETRANSLATE_COMPUTE_HINTS (see above): the switch.
bool HintsOn() noexcept;
/// Compute prebuild (InstallComputePrebuild's worker job): the hint of a program, or null. Marks the program as
/// prebuilt with a hint for the report.
HintPtr AcquireHint(uint64_t hash) noexcept;
/// The hint a worker's translation uses (set by HintScope), or null.
const Hint* ActiveHint() noexcept;
/// Sets ActiveHint for the scope of one translation.
class HintScope {
public:
    explicit HintScope(HintPtr hint);
    ~HintScope();
    HintScope(const HintScope&) = delete;
    HintScope& operator=(const HintScope&) = delete;

private:
    HintPtr hint_;
    const Hint* prev_;
};
/// Command processor, a compute program's first draw (its dispatch): the real key (spec, from info's flat buffer)
/// against the prebuilt one (predicted). Writes the real fields to the hint file on a miss, logs the first 32 misses.
void NoteComputeFirstUse(uint64_t hash, bool hit, const Shader::Info& info, const Shader::StageSpecialization& real,
                         const Shader::StageSpecialization& predicted) noexcept;

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcPretranslate
