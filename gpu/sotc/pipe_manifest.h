// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PIPE_MANIFEST (default on; 0 = off). The pipelines the game needs, built at the game-start loading
// screen instead of at their first draw (where a missing graphics pipeline drops its draws for seconds on a cold
// cache, and a missing compute pipeline stalls the recorder).
//
// A manifest is a text file of records, each holding only hashes and numeric state (never shader code or other game
// data): data/pipeline_manifest/sotc-pipelines.tsv ships with the port (tools/gen_pipeline_manifest.py merges it from
// the user records), and every session appends what it built that no manifest held to the user's own file
// (<user>/shader_predict/sotc-pipelines.user.tsv). Lines (tab separated, '#' starts a comment):
//   H  format=1  game=<serial>  key=<sizeof GraphicsPipelineKey>  rt=<sizeof RuntimeInfo>  meta=<ShaderMetaVersion>
//      variant=<the shader cache variant key>     -- the header of the records that follow it
//   S  <stage id>  <program hash>  <stage variant>  <spec>  <hint>
//      a shader stage as a draw used it: the game's program hash, the stage variant (SOTC_HEAP / SOTC_SHADER_ADDR),
//      the StageSpecialization bytes the draw's permutation was keyed by (StageSpecialization::Serialize: runtime
//      info, start bindings, fetch attributes, the buffer/image/sampler format keys), hex; and the raw sharp fields the
//      translation reads (hint text, see HintText), so a translation without the draw can be made from the program
//      code the game loads (SotcPredict::NoteLoadedData hands it on) and checked: its specialization must equal the
//      recorded one, or the stage is skipped.
//   G  <pipeline id>  <key>  <slot>:<stage id>[,<slot>:<stage id>...]
//      a graphics pipeline: GraphicsPipelineKey bytes with the stage hashes zeroed (hex), then its stages by slot.
//   C  <pipeline id>  <stage id>
//      a compute pipeline.
// Records under a header whose key/rt/meta sizes differ from this build's are skipped (counted). A different variant
// key is accepted (the state is the game's; the specialization check guards the rest) and noted once.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pretranslate.h"

namespace SotcPipeManifest {

/// SOTC_PIPE_MANIFEST: on unless set to 0. Read once.
bool On() noexcept;

struct Header {
    std::string game;    ///< the game serial (informational; records are accepted for any SotC eboot)
    std::string variant; ///< the shader cache variant key (vk_pipeline_serialization.cpp WarmUp)
    uint32_t key_size{}, rt_size{}, meta_version{};
};

struct Stage {
    uint64_t id{};
    uint64_t hash{};   ///< the game's program hash (Info::pgm_hash)
    uint8_t variant{}; ///< Shader::StageVariant*
    std::vector<uint8_t> spec;
    SotcPretranslate::Hint hint;
};

struct Pipe {
    uint64_t id{};
    bool compute{};
    std::vector<uint8_t> key;                           ///< graphics: GraphicsPipelineKey bytes, stage hashes zeroed
    std::vector<std::pair<uint8_t, uint64_t>> stages;   ///< (stage slot, stage id); compute: slot 0
};

struct Data {
    std::vector<Stage> stages;
    std::vector<Pipe> pipes;
    size_t shipped_pipes{}, user_pipes{}; ///< pipelines read from each file (before the merge)
    size_t refused_records{};             ///< records under a header of another layout
    bool variant_differs{};               ///< a header with another variant key was accepted
};

/// The shipped manifest and the user's, merged (deduplicated by id; a pipeline whose stages are missing is dropped).
/// Remembers every id as known (Known). log receives one line per file.
Data Load(const Header& want, std::string& log);

/// Stable ids (FNV-1a over the record's fields).
uint64_t StageId(uint64_t hash, uint8_t variant, const std::vector<uint8_t>& spec, const SotcPretranslate::Hint& hint);
uint64_t PipeId(bool compute, const std::vector<uint8_t>& key, const std::vector<std::pair<uint8_t, uint64_t>>& stages);

/// Capture (command processor): ids already in a manifest or appended this session.
bool Known(uint64_t id);
/// Appends a record to the user's manifest (the session's header first) and marks it known.
void AppendStage(const Header& header, const Stage& stage);
void AppendPipe(const Header& header, const Pipe& pipe);
/// Writes the appended records out (called from the 5 s report and at the end of the boot build).
void Flush();
/// Records appended this session.
uint64_t Appended() noexcept;
/// The private bytes of the process (0 when unknown): the SOTC_GPL_NODROP boot library line.
uint64_t ProcessPrivateBytes() noexcept;

/// The per-user list of stages not to translate again under this variant key: Failed (their translation could not
/// reproduce the draw's specialization, or failed), Late (their program's code did not come with the boot load; the
/// next boot holds do not wait for them, they are still built when their code comes). <user>/shader_predict/
/// sotc-pipelines.skip.tsv.
enum class SkipKind : uint8_t { Failed = 0, Late = 1 };
void LoadSkips(const std::string& variant);
bool Skipped(uint64_t stage_id, SkipKind kind);
void NoteSkip(uint64_t stage_id, SkipKind kind, const char* reason);

/// Program code of the game's loaded shader files (loader thread; SotcPredict::NoteLoadedData). Until SetWanted the
/// code of every program is kept (up to a cap); after it only the wanted programs'.
void NoteProgram(uint64_t hash, const void* code, size_t bytes) noexcept;
void SetWanted(const std::vector<uint64_t>& hashes);
/// The code of a program, or null when it has not come (yet).
std::shared_ptr<const std::vector<uint32_t>> Code(uint64_t hash);
/// Wanted programs whose code came so far, and the time of the last program of any kind (steady clock ns).
uint64_t CodeArrivals() noexcept;
int64_t LastProgramNs() noexcept;
/// Frees the code store (the manifest work is over).
void DropCode();

/// The hint text of a stage record ("-" when empty) and its parser.
std::string HintText(const SotcPretranslate::Hint& hint);
bool ParseHint(const std::string& text, SotcPretranslate::Hint& out);

std::string ShippedPath();
std::string UserPath();

} // namespace SotcPipeManifest
