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
#include <string>
#include <vector>

namespace SotcPretranslate {

/// SOTC_PRETRANSLATE=1 or SOTC_PRECOMPILE=1, read once.
bool On() noexcept;

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
    uint8_t stage{}; ///< 0 pixel, 1 vertex
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
};

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
void NoteDropped(int stage, int reason) noexcept; ///< 0 known program, 1 no pair yet (given up)
void NoteBuilt(int stage, bool ok) noexcept;
void NoteSuperseded(int stage) noexcept;           ///< a draw needed it first: translated normally
void NoteFirstUse(int stage, bool hit, uint32_t diff) noexcept;

/// The 5 s report line (driver.cpp); nothing when the switch is off.
void Report(std::string& out, double seconds);

} // namespace SotcPretranslate
