// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>


#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "sdl_window.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <unordered_map>
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_event.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_ui_lang.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h"  // sotc: SOTC_TRACE dump toast
#include "sotc_lab_menu.h"          // sotc: SotC Lab (F2 menu)
#include "../sotc/autopilot.h"     // sotc: SotC Lab (F4 waypoint)

#include "../sotc/freecam.h"       // sotc: SotC Lab (F3 free-cam)
#include "../sotc/lab_commands.h"  // sotc: SotC Lab (SOTC_SET_TRIGGER commands)

extern Frontend::WindowSDL* g_window;
static const Vulkan::Instance* s_instance = nullptr;
// sotc: the volume of the runtime's audio output (src/runtime_audio.c); weak, so builds without the
// runtime (tests) still link.
extern "C" __attribute__((weak)) void runtime_audio_set_volume_percent(int pct);
extern "C" __attribute__((weak)) int runtime_audio_volume_percent(void);
extern "C" __attribute__((weak)) int runtime_rumble_enabled(void);
extern "C" __attribute__((weak)) void runtime_rumble_set(int on);

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save/load slot list (OpenSaveList), guarded by imgui_mutex.
bool list_active = false;
std::string list_title;
std::vector<std::string> list_items;
// sotc: each save's picture (icon0.png of its sce_sys), shown to the left of its text as in the
// PS4 and shadPS4 lists. Loaded once per file, kept for the session.
std::vector<std::string> list_icons, pending_icons;
std::unordered_map<std::string, ImTextureData*> icon_textures;

ImTextureData* IconTexture(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto it = icon_textures.find(path); it != icon_textures.end()) {
        return it->second;
    }
    ImTextureData* tex = nullptr;
    int w = 0, h = 0, n = 0;
    if (unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &n, 4)) {
        tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, w, h);
        std::memcpy(tex->GetPixels(), pixels, size_t(w) * h * 4);
        stbi_image_free(pixels);
        ImGui::RegisterUserTexture(tex);
    }
    icon_textures[path] = tex;
    return tex;
}
int list_selected = 0, list_result = -1;
int list_confirm = -1;  // item waiting for the delete confirmation, or -1
int list_first_deletable = 0;
bool list_scroll = false, list_stick_down = false, list_stick_up = false;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

// sotc: the settings menu is Bloodborne's; the SotC port shows only the game's dialogs
// (BB_OVERLAY_MENU=1 opens the menu anyway).
bool MenuAllowed() {
    if (Frontend::IsMonitor()) { // sotc: SOTC_HEADLESS=2 never opens the menu (no input)
        return false;
    }
    const char* v = std::getenv("BB_OVERLAY_MENU");
    if (v && v[0] == '0') return false;
    return true;
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    SotcEvent::Log("KEY", "%s", value ? SotcUi::T("Menu do jogo aberto", "In-game menu opened")
                                      : SotcUi::T("Menu do jogo fechado", "In-game menu closed"));
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// sotc: a short message at the top of the screen (F8: the frame generation mode): its text
// (under imgui_mutex) and until when, in steady clock milliseconds.
std::string toast_text;
std::atomic<long long> toast_until_ms{0};

// sotc: when the last game flip came (steady clock milliseconds), kept by LoadingShown.
std::atomic<long long> last_flip_ms{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ToastShown() {
    return NowMs() < toast_until_ms.load(std::memory_order_relaxed);
}

// sotc: the performance overlay (show_fps=1 in sotc.ini, or SOTC_FPS / SOTC_OVERLAY=1; F10
// toggles it). History of present intervals (one per game flip), present thread only.
constexpr int kHistory = 240;
float frame_hist[kHistory]{};
int hist_pos = 0, hist_count = 0;
uint64_t session_frames = 0, session_late = 0, session_stutters = 0;
std::atomic<bool> overlay_on{[] {
    if (const char* v = std::getenv("SOTC_OVERLAY")) {
        return v[0] != '0';
    }
    // sotc: a test run (SOTC_RUN_LABEL, the offline matrix runner) always shows the overlay and its run panel.
    return std::getenv("SOTC_FPS") != nullptr || std::getenv("SOTC_RUN_LABEL") != nullptr;
}()};

// Frame time the game aims for: 33.3 ms (30 FPS) or 16.7 ms (60 FPS), from the median.
float TargetMs() {
    if (hist_count < 8) {
        return 33.3f;
    }
    float sorted[kHistory];
    std::copy(frame_hist, frame_hist + hist_count, sorted);
    std::nth_element(sorted, sorted + hist_count / 2, sorted + hist_count);
    return sorted[hist_count / 2] < 25.0f ? 16.7f : 33.3f;
}

bool OverlayOn() {
    // sotc: SOTC_HEADLESS=2 (monitor): the overlay is the point of the window, always shown.
    return Frontend::IsMonitor() || BbSettings::Get().show_fps || overlay_on.load(std::memory_order_relaxed);
}

void RecordFrame(float ms) {
    frame_hist[hist_pos] = ms;
    hist_pos = (hist_pos + 1) % kHistory;
    hist_count = std::min(hist_count + 1, kHistory);
    ++session_frames;
    if (ms > TargetMs() * 1.1f) {
        ++session_late;
    }
    if (ms > 50.0f) {
        ++session_stutters;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// sotc: the launcher's settings file (SOTC_LAUNCHER_SETTINGS, user/launcher_settings.json), so a
// quick setting changed in the menu is also the launcher's choice next time. Only keys already in
// the file are replaced.
std::filesystem::path LauncherSettingsPath() {
#ifdef _WIN32
    const wchar_t* wpath = _wgetenv(L"SOTC_LAUNCHER_SETTINGS");
    return wpath ? std::filesystem::path{wpath} : std::filesystem::path{};
#else
    const char* cpath = std::getenv("SOTC_LAUNCHER_SETTINGS");
    return cpath ? std::filesystem::path{cpath} : std::filesystem::path{};
#endif
}

std::string ReadLauncherSettings() {
    const auto path = LauncherSettingsPath();
    if (path.empty()) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::regex LauncherSettingRegex(const char* key) {
    return std::regex("(\"" + std::string(key) + "\"\\s*:\\s*)(\"[^\"]*\"|true|false|-?[0-9.]+)");
}

// The value of a key (a string without its quotes), or "" when it is not in the file.
std::string LoadLauncherSetting(const char* key) {
    const std::string text = ReadLauncherSettings();
    std::smatch m;
    if (!std::regex_search(text, m, LauncherSettingRegex(key))) {
        return {};
    }
    std::string v = m[2].str();
    return v.size() >= 2 && v.front() == '"' ? v.substr(1, v.size() - 2) : v;
}

void SaveLauncherSetting(const char* key, const std::string& json_value) {
    std::string text = ReadLauncherSettings();
    std::smatch m;
    if (text.empty() || !std::regex_search(text, m, LauncherSettingRegex(key))) {
        return;
    }
    text = m.prefix().str() + m[1].str() + json_value + m.suffix().str();
    std::ofstream out(LauncherSettingsPath(), std::ios::binary | std::ios::trunc);
    out << text;
}

// sotc: SOTC_LOD_POOL_LOW at start (8 when unset): the page pool switch of texture level "High".
int EnvPoolLow() {
    static const int value = [] {
        const char* v = std::getenv("SOTC_LOD_POOL_LOW");
        return v ? std::atoi(v) : 8;
    }();
    return value;
}

// sotc: texture level (#235): 0 off; 1 high, with the page pool switch (coarser mips while the
// game's page pool is low, sotc_lod_stats.h); 2 maximum, without it. Applies at once
// (sotc_live.h) and is kept as the launcher's choice.
void SetTextureLevel(int level) {
    SotcLive::lod_linger.store(level > 0 ? 1 : 0, std::memory_order_relaxed);
    SotcLive::lod_pool_low.store(level == 2 ? 0 : (EnvPoolLow() > 0 ? EnvPoolLow() : 8),
                                 std::memory_order_relaxed);
    const char* keys[] = {"\"off\"", "\"high\"", "\"max\""};
    SaveLauncherSetting("textures", keys[level]);
    const char* names[] = {SotcUi::T("Desligada", "Off"), SotcUi::T("Alta", "High"),
                           SotcUi::T("Máxima", "Maximum")};
    SotcEvent::Log("MENU", SotcUi::T("Qualidade das texturas: %s", "Texture quality: %s"), names[level]);
}

// sotc: the FPS mode now: 0 = 30 (the game's cap), 1 = 60, 2 = unlimited.
int FpsMode() {
    static const int env_limit = [] {
        const char* v = std::getenv("SOTC_FPS_LIMIT");
        return v ? std::atoi(v) : 0;
    }();
    if (!SotcFpsCap::uncapped.load(std::memory_order_relaxed)) {
        return 0;
    }
    const int live_limit = SotcFpsCap::limit_fps.load(std::memory_order_relaxed);
    return (live_limit >= 0 ? live_limit : env_limit) > 0 ? 1 : 2;
}

const char* FpsModeName(int mode) {
    const char* names[] = {SotcUi::T("FPS travados em 30", "FPS capped at 30"),
                           SotcUi::T("FPS travados em 60", "FPS capped at 60"),
                           SotcUi::T("FPS ilimitados", "FPS unlimited")};
    return names[mode];
}

// sotc: FPS mode 0 = 30 (the game's cap), 1 = 60, 2 = unlimited, as the launcher's.
void SetFpsMode(int mode) {
    if (mode > 0) {
        SotcFpsCap::limit_fps.store(mode == 1 ? 60 : 0, std::memory_order_relaxed);
    }
    if ((mode > 0) != SotcFpsCap::uncapped.load(std::memory_order_relaxed)) {
        SotcFpsCap::Toggle();
    }
    const char* keys[] = {"\"30\"", "\"60\"", "\"unlimited\""};
    SaveLauncherSetting("fps_mode", keys[mode]);
    const char* names[] = {"30", "60", SotcUi::T("ilimitado", "unlimited")};
    SotcEvent::Log("MENU", "FPS: %s", names[mode]);
}

// sotc: upscaler 0 = FSR 1, 1 = bilinear; applied at the next frame (vk_presenter.cpp).
void SetUpscaler(int mode) {
    SotcLive::fsr1.store(mode == 0 ? 1 : 0, std::memory_order_relaxed);
    SaveLauncherSetting("upscaler", mode == 0 ? "\"fsr1\"" : "\"bilinear\"");
    SotcEvent::Log("MENU", "Upscaler: %s", mode == 0 ? "FSR 1" : "Bilinear");
}

// sotc: RCAS sharpening 0 high, 1 medium, 2 low, 3 off (the launcher's SHARPNESS values).
constexpr int RcasValues[] = {0, 250, 600, -2};
void SetRcas(int mode) {
    SotcLive::rcas.store(RcasValues[mode], std::memory_order_relaxed);
    const char* keys[] = {"\"0\"", "\"250\"", "\"600\"", "\"off\""};
    SaveLauncherSetting("sharpness", keys[mode]);
    const char* names[] = {SotcUi::T("Alta", "High"), SotcUi::T("Média", "Medium"),
                           SotcUi::T("Baixa", "Low"), SotcUi::T("Desligada", "Off")};
    SotcEvent::Log("MENU", SotcUi::T("Nitidez (RCAS): %s", "Sharpening (RCAS): %s"), names[mode]);
}

// sotc: one of the launcher's grass choice lists as a combo. With a live value (SotcLive::grass)
// a pick applies at once; without, it is only saved for the next start.
struct GrassChoice {
    const char* value;
    const char* pt;
    const char* en;
};

template <size_t N>
void GrassCombo(const char* label, const char* key, const GrassChoice (&choices)[N],
                std::atomic<float>* live, float env_value) {
    using SotcUi::T;
    static std::unordered_map<std::string, float> saved; // restart-only picks
    float now;
    if (live) {
        const float v = live->load(std::memory_order_relaxed);
        now = v >= 0.f ? v : env_value;
    } else if (const auto it = saved.find(key); it != saved.end()) {
        now = it->second;
    } else {
        // not saved yet (settings from before the grass options): the game's own value
        const std::string v = LoadLauncherSetting(key);
        now = saved[key] = v.empty() ? env_value : std::strtof(v.c_str(), nullptr);
    }
    char preview[16];
    std::snprintf(preview, sizeof(preview), "%g", now);
    const char* shown = preview;
    for (const auto& c : choices) {
        if (std::strtof(c.value, nullptr) == now) {
            shown = T(c.pt, c.en);
        }
    }
    if (ImGui::BeginCombo(label, shown)) {
        for (const auto& c : choices) {
            const float v = std::strtof(c.value, nullptr);
            if (ImGui::Selectable(T(c.pt, c.en), v == now)) {
                if (live) {
                    live->store(v, std::memory_order_relaxed);
                } else {
                    saved[key] = v;
                }
                SaveLauncherSetting(key, "\"" + std::string(c.value) + "\"");
            }
        }
        ImGui::EndCombo();
    }
}

// sotc: the launcher's grass options. Distance, height and detail are constants of the grass
// compute shaders, so they apply at once; shadows and fade are engine settings read at start.
void GrassSettings() {
    using SotcUi::T;
    static const std::array<float, 5> env = [] {
        std::array<float, 5> s{1.f, 1.f, 1.f, 1.f, 1.f};
        if (const char* v = std::getenv("SOTC_GRASS")) {
            std::sscanf(v, "%f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4]);
        }
        return s;
    }();
    static const GrassChoice detail[] = {{"1", "Como no PS4", "As on PS4"},
                                         {"0.25", "Alto", "High"},
                                         {"0.15", "Mais alto", "Higher"},
                                         {"0.1", "Muito alto", "Very high"}};
    static const GrassChoice distance[] = {{"1", "Como no PS4", "As on PS4"},
                                           {"2", "2x mais longe", "2x farther"},
                                           {"3", "3x mais longe", "3x farther"},
                                           {"5", "5x mais longe", "5x farther"}};
    static const GrassChoice weed_distance[] = {
        {"1", "Como no PS4", "As on PS4"},
        {"2", "2x mais longe", "2x farther"},
        {"3", "3x mais longe", "3x farther"},
        {"5", "5x mais longe", "5x farther"},
        {"10", "10x mais longe (campo denso)", "10x farther (dense field)"}};
    static const GrassChoice height[] = {{"0.75", "75%", "75%"},
                                         {"1", "Como no PS4", "As on PS4"},
                                         {"1.5", "150%", "150%"},
                                         {"2", "200% (arbustos)", "200% (bushes)"}};
    static const GrassChoice shadows[] = {
        {"1", "Perto (~3 m, mais leve)", "Near (~3 m, lighter)"},
        {"2", "Como no PS4 (~8 m)", "As on PS4 (~8 m)"},
        {"3", "Longe (~24 m, mais pesado)", "Far (~24 m, heavier)"}};
    static const GrassChoice fade[] = {{"0.5", "50%", "50%"},
                                       {"0.75", "75%", "75%"},
                                       {"1", "Como no PS4", "As on PS4"},
                                       {"2", "200%", "200%"},
                                       {"3", "300%", "300%"}};

    ImGui::SeparatorText(T("Grama", "Grass"));
    GrassCombo(T("Detalhe da grama", "Grass detail"), "grass_detail", detail, &SotcLive::grass[4],
               env[4]);
    Hint(T("Lâminas completas mais longe: a grama e a sombra perto de você não surgem tarde.",
           "Full blades farther out: grass and its shadow near you no longer pop in late."));
    GrassCombo(T("Distância da grama", "Grass distance"), "grass_distance", distance,
               &SotcLive::grass[0], env[0]);
    GrassCombo(T("Altura da grama", "Grass height"), "grass_height", height, &SotcLive::grass[1],
               env[1]);
    Hint(T("Recomendado: como no PS4, ou 150% se gosta de mais grama; 200% vira arbusto.",
           "Recommended: as on PS4, or 150% if you like more grass; 200% turns it into bushes."));
    GrassCombo(T("Distância do mato", "Weed distance"), "weed_distance", weed_distance,
               &SotcLive::grass[2], env[2]);
    GrassCombo(T("Altura do mato", "Weed height"), "weed_height", height, &SotcLive::grass[3],
               env[3]);
    GrassCombo(T("Sombras da grama (ao reiniciar)", "Grass shadows (on restart)"), "grass_shadows",
               shadows, nullptr, 2.f);
    GrassCombo(T("Esmaecimento da grama (ao reiniciar)", "Grass fade (on restart)"), "grass_fade",
               fade, nullptr, 1.f);
    Hint(T("Abaixo de 100% a sombra da grama também some (50%: quase sem sombra). Menos grama "
           "alivia um pouco a GPU.",
           "Below 100% the grass shadow fades too (50%: almost no shadow). Less grass is a little "
           "lighter on the GPU."));
}

// sotc: "Quick settings": the launcher's quick settings that can change while playing.
void QuickSettings() {
    using SotcUi::T;
    ImGui::SeparatorText(T("Ajuste rápido (vale na hora)", "Quick settings (apply at once)"));

    // sotc: FPS as the launcher's: 30 (the game's cap), 60 or unlimited. F7 switches the cap.
    const bool uncapped = SotcFpsCap::uncapped.load(std::memory_order_relaxed);
    const int fps_mode = FpsMode();
    const char* fps_names[] = {T("30 FPS (como no PS4)", "30 FPS (as on PS4)"), "60 FPS",
                               T("Ilimitado", "Unlimited")};
    if (ImGui::BeginCombo("FPS  [F7]", fps_names[fps_mode])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(fps_names[i], i == fps_mode) && i != fps_mode) {
                SetFpsMode(i);
            }
        }
        ImGui::EndCombo();
    }
    // sotc: flip pacing at 60 FPS and unlimited (videoout driver.cpp): the game's own rate or the vblank grid.
    const bool paced = SotcFpsCap::flip_pacer.load(std::memory_order_relaxed);
    const char* pace_names[] = {T("Grade do vblank (original)", "Vblank grid (original)"),
                                T("Suave (ritmo do jogo)", "Smooth (game's rate)")};
    if (ImGui::BeginCombo(T("Ritmo dos quadros  [F6]", "Frame pacing  [F6]"), pace_names[paced])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(pace_names[i], i == int(paced)) && i != int(paced)) {
                SotcFpsCap::TogglePacer();
            }
        }
        ImGui::EndCombo();
    }
    Hint(T("Em 60 FPS e ilimitado: solta cada quadro no ritmo do jogo em vez de na grade do "
           "vblank (sem o tranco 8/17 ms ou 17/25 ms).",
           "At 60 FPS and unlimited: releases each frame at the game's rate instead of on the "
           "vblank grid (no 8/17 ms or 17/25 ms double-beat)."));

    // Started at 30 FPS the game is not in its own 60 FPS mode (fps60game, chosen at start):
    // uncapped it runs faster, but some physics may be off (the 2nd colossus' wall).
    if (uncapped && !SotcFpsCap::target_slot.load(std::memory_order_relaxed)) {
        Hint(T("Aberto em 30 FPS: o modo 60 do próprio jogo (física certa) só entra abrindo o jogo "
               "com 60 FPS no lançador.",
               "Started at 30 FPS: the game's own 60 FPS mode (right physics) only applies when the "
               "game starts with 60 FPS in the launcher."));
    }

    // sotc: upscaler and sharpening, applied at the next frame (vk_presenter.cpp, sotc_live.h).
    static const int env_fsr1 = [] {
        const char* v = std::getenv("BB_FSR1");
        return v && v[0] != '0' ? 1 : 0;
    }();
    const int live_fsr1 = SotcLive::fsr1.load(std::memory_order_relaxed);
    const int up_mode = (live_fsr1 >= 0 ? live_fsr1 : env_fsr1) ? 0 : 1;
    const char* up_names[] = {T("FSR 1 (AMD, qualquer placa)", "FSR 1 (AMD, any GPU)"),
                              T("Bilinear (simples)", "Bilinear (plain)")};
    // sotc: DLSS and AMD FSR 4 (SOTC_UPSCALER=dlss / fsr4) are chosen at start, their render
    // size with them: shown, not switchable here (picking FSR 1 here saved "fsr1" over the
    // launcher's DLSS, 26 Oct).
    static const char* start_upscaler = [] {
        const char* v = std::getenv("SOTC_UPSCALER");
        return !v ? nullptr
               : std::strcmp(v, "dlss") == 0 ? "NVIDIA DLSS"
               : std::strcmp(v, "fsr4") == 0 ? "AMD FSR 4"
                                             : nullptr;
    }();
    if (start_upscaler) {
        ImGui::BeginDisabled();
        if (ImGui::BeginCombo("Upscaler", start_upscaler)) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Hint(T("Este upscaler troca no lançador (vale ao abrir o jogo de novo).",
               "This upscaler is switched in the launcher (applies when the game starts again)."));
    } else if (ImGui::BeginCombo("Upscaler", up_names[up_mode])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(up_names[i], i == up_mode) && i != up_mode) {
                SetUpscaler(i);
            }
        }
        ImGui::EndCombo();
    }
    static const int env_rcas = [] {
        const char* on = std::getenv("BB_RCAS");
        if (on && on[0] == '0') {
            return -2;
        }
        const char* v = std::getenv("BB_RCAS_ATTENUATION");
        return v ? std::atoi(v) : 250;
    }();
    const int live_rcas = SotcLive::rcas.load(std::memory_order_relaxed);
    const int rcas_now = live_rcas != -1 ? live_rcas : env_rcas;
    const char* rcas_names[] = {T("Alta", "High"), T("Média (padrão)", "Medium (default)"),
                                T("Baixa", "Low"), T("Desligada", "Off")};
    int rcas_mode = 1;
    for (int i = 0; i < 4; ++i) {
        if (RcasValues[i] == rcas_now) {
            rcas_mode = i;
        }
    }
    if (ImGui::BeginCombo(T("Nitidez (RCAS)", "Sharpening (RCAS)"), rcas_names[rcas_mode])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(rcas_names[i], i == rcas_mode) && i != rcas_mode) {
                SetRcas(i);
            }
        }
        ImGui::EndCombo();
    }
    if (!(live_fsr1 >= 0 ? live_fsr1 : env_fsr1)) {
        Hint(T("A nitidez vale com o FSR 1.", "Sharpening applies with FSR 1."));
    }

    using Vulkan::FrameGen::Mode;
    const Mode fg = Vulkan::FrameGen::Current();
    const char* fg_names[] = {T("Desligada", "Off"), "NVIDIA DLSS-G", "AMD FSR 3"};
    const char* fg_keys[] = {"\"off\"", "\"dlss\"", "\"fsr\""};
    if (ImGui::BeginCombo(T("Geração de quadros  [F8]", "Frame generation  [F8]"),
                          fg_names[static_cast<int>(fg)])) {
        for (int i = 0; i < 3; ++i) {
            const Mode mode = static_cast<Mode>(i);
            const bool available = Vulkan::FrameGen::Available(mode);
            if (!available) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(fg_names[i], mode == fg) && available &&
                Vulkan::FrameGen::Set(mode)) {
                SaveLauncherSetting("framegen", fg_keys[i]);
                SotcEvent::Log("MENU", SotcUi::T("Geração de quadros: %s", "Frame generation: %s"),
                               fg_names[i]);
            }
            if (!available) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndCombo();
    }

    static const int env_linger = [] {
        const char* v = std::getenv("SOTC_LOD_LINGER");
        return v ? std::atoi(v) : 0;
    }();
    const int live_linger = SotcLive::lod_linger.load(std::memory_order_relaxed);
    const int live_low = SotcLive::lod_pool_low.load(std::memory_order_relaxed);
    const bool hq = (live_linger >= 0 ? live_linger : env_linger) > 0;
    const int tex_level = !hq ? 0 : (live_low >= 0 ? live_low : EnvPoolLow()) > 0 ? 1 : 2;
    const char* tex_names[] = {T("Desligada", "Off"), T("Alta (estável)", "High (stable)"),
                               T("Máxima (nítida)", "Maximum (sharp)")};
    // sotc: "Maximum" in PS4 Pro mode exhausted the game's page pool within minutes at the Shrine
    // (0x1a371, 22 Oct) until tabnul's page allocator fix (PR #288); experimental there since.
    // SOTC_MAX_IN_PRO=0 blocks it again.
    static const bool neo = [] {
        const char* v = std::getenv("SOTC_NEO");
        const char* allow = std::getenv("SOTC_MAX_IN_PRO");
        return v && v[0] == '1' && allow && allow[0] == '0';
    }();
    if (ImGui::BeginCombo(T("Qualidade das texturas", "Texture quality"), tex_names[tex_level])) {
        for (int i = 0; i < 3; ++i) {
            const bool blocked = neo && i == 2;
            if (blocked) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(tex_names[i], i == tex_level) && i != tex_level && !blocked) {
                SetTextureLevel(i);
            }
            if (blocked) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("— não no modo PS4 Pro", "— not in PS4 Pro mode"));
            }
        }
        ImGui::EndCombo();
    }
    Hint(T("Alta: folhas e pedras nítidas, um pouco menos em vistas amplas. Máxima: sempre "
           "nítida, mas pode fechar em vistas muito amplas (fora do modo PS4 Pro, onde fechava "
           "em minutos). Alta e Máxima valem na hora (o jogo leva alguns segundos para carregar "
           "as texturas); Desligada só aparece ao abrir o jogo de novo (o jogo não descarta o "
           "que já carregou).",
           "High: sharp leaves and rocks, a little less in wide views. Maximum: always sharp, "
           "but it may close in very wide views (not in PS4 Pro mode, where it closed within "
           "minutes). High and Maximum apply at once (the game takes a few seconds to load the "
           "textures); Off only shows when the game starts again (the game keeps what it already "
           "loaded)."));

    static const int env_reuse = [] {
        const char* v = std::getenv("SOTC_SHADOW_REUSE");
        if (!v) {
            return 0;
        }
        if (v[0] == 'a') {
            return s_instance && s_instance->GetVendorID() == 0x10DE ? 1 : 0;
        }
        return v[0] == '0' ? 0 : 1;
    }();
    const int live_reuse = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
    bool reuse = (live_reuse >= 0 ? live_reuse : env_reuse) > 0;
    if (ImGui::Checkbox(T("Reaproveitar sombras distantes (podem piscar)",
                          "Reuse far shadows (they may blink)"),
                        &reuse)) {
        SotcLive::shadow_reuse.store(reuse ? 1 : 0, std::memory_order_relaxed);
        SaveLauncherSetting("shadow_reuse", reuse ? "\"1\"" : "\"0\"");
        SotcEvent::Log("MENU", SotcUi::T("Reaproveitar sombras distantes: %s", "Reuse far shadows: %s"),
                       reuse ? SotcUi::T("ligado", "on") : SotcUi::T("desligado", "off"));
    }

    if (runtime_audio_volume_percent && runtime_audio_set_volume_percent) {
        static const int volumes[] = {100, 150, 200, 300};
        const int now = runtime_audio_volume_percent();
        char label[16];
        std::snprintf(label, sizeof(label), "%d%%", now);
        if (ImGui::BeginCombo(T("Volume", "Volume"), label)) {
            for (const int v : volumes) {
                char item[16];
                std::snprintf(item, sizeof(item), "%d%%", v);
                if (ImGui::Selectable(item, v == now)) {
                    runtime_audio_set_volume_percent(v);
                    SaveLauncherSetting("volume", "\"" + std::to_string(v) + "\"");
                    SotcEvent::Log("MENU", "Volume: %d%%", v);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("Resolução, efeitos (névoa, vinheta, SSAO), AF 16x, modo PS4 Pro e "
                                "memória de vídeo: no lançador (valem ao abrir o jogo de novo).",
                                "Resolution, effects (fog, vignette, SSAO), 16x AF, PS4 Pro mode and "
                                "video memory use: in the launcher (they apply when the game starts "
                                "again)."));
    ImGui::PopStyleColor();
    GrassSettings();
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(660.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    const std::string title = std::string("Shadow of the Colossus — ") +
                              SotcUi::T("Configurações", "Settings") + "  (Insert / F1 / L3+R3)###menu";
    if (!ImGui::Begin(title.c_str(), &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // --- Header: FPS, GPU & VRAM info ---
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);
    if (s_instance) {
        const u64 budget_mb = s_instance->GetTotalMemoryBudget() / (1024 * 1024);
        const u64 used_mb = s_instance->GetDeviceMemoryUsage() / (1024 * 1024);
        ImGui::SameLine();
        ImGui::TextDisabled(" |  %s (%lu / %lu MB VRAM)",
                            std::string(s_instance->GetModelName()).c_str(),
                            static_cast<unsigned long>(used_mb),
                            static_cast<unsigned long>(budget_mb));
    }

    QuickSettings();

    // sotc: only what the SotC port reads. Bloodborne's output resolution, temporal upscalers
    // (FSR 3.1/4, DLSS, TAA, presets), RCAS strength and engine effects were shown here but never
    // applied: those are the launcher's (environment and engine cvars at start).
    ImGui::SeparatorText(SotcUi::T("Tela", "Display"));
    bool fs = s.fullscreen.load();
    if (ImGui::Checkbox(SotcUi::T("Tela cheia  [F11]", "Fullscreen  [F11]"), &fs)) {
        Store(s.fullscreen, fs, true);
        if (g_window) {
            g_window->SetFullscreen(fs);
        }
        SotcEvent::Log("MENU", SotcUi::T("Tela cheia: %s", "Fullscreen: %s"),
                       fs ? SotcUi::T("ligada", "on") : SotcUi::T("desligada", "off"));
    }
    ImGui::SameLine();
    Checkbox(SotcUi::T("Mostrar FPS  [F10]", "Show FPS  [F10]"), s.show_fps);

    ImGui::SeparatorText(SotcUi::T("Câmera e mouse", "Camera & mouse"));
    if (const bool was = s.mouse_enabled.load(); Checkbox(SotcUi::T("Câmera pelo mouse", "Mouse camera"), s.mouse_enabled), was != s.mouse_enabled.load()) {
        SotcEvent::Log("MENU", SotcUi::T("Câmera pelo mouse: %s", "Mouse camera: %s"),
                       s.mouse_enabled.load() ? SotcUi::T("ligada", "on") : SotcUi::T("desligada", "off"));
    }
    ImGui::BeginDisabled(!s.mouse_enabled);
    Slider(SotcUi::T("Sensibilidade do mouse", "Mouse sensitivity"), s.mouse_sens, 0.1f, 3.0f);
    if (const bool was = s.mouse_invert.load(); Checkbox(SotcUi::T("Inverter eixo vertical (Y)", "Invert vertical axis (Y)"), s.mouse_invert), was != s.mouse_invert.load()) {
        SotcEvent::Log("MENU", SotcUi::T("Inverter eixo Y do mouse: %s", "Invert mouse Y axis: %s"),
                       s.mouse_invert.load() ? SotcUi::T("ligado", "on") : SotcUi::T("desligado", "off"));
    }
    ImGui::EndDisabled();

    // sotc: the pad's vibration, at once and kept as the launcher's option (#292)
    if (runtime_rumble_enabled && runtime_rumble_set) {
        ImGui::SeparatorText(SotcUi::T("Controle", "Controller"));
        bool rumble = runtime_rumble_enabled() != 0;
        if (ImGui::Checkbox(SotcUi::T("Vibração do controle", "Controller vibration"), &rumble)) {
            runtime_rumble_set(rumble ? 1 : 0);
            SaveLauncherSetting("rumble", rumble ? "true" : "false");
            SotcEvent::Log("MENU", SotcUi::T("Vibração do controle: %s", "Controller vibration: %s"),
                           rumble ? SotcUi::T("ligada", "on") : SotcUi::T("desligada", "off"));
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(SotcUi::T("Fechar", "Close"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SotcUi::T("Esc / F1 fecha; tudo fica salvo",
                                        "Esc / F1 closes; everything is kept"));

    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// The game's save/load slot list: one entry per save, the selected one highlighted.
void SaveListBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.46f, viewport->WorkSize.y * 0.62f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##save_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::PushTextWrapPos(0.0f); // sotc: save dialog messages are whole sentences
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", list_title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##items", ImVec2(0.0f, -footer));
    // sotc: rows with the save's picture on the left (16:9 of 228x128) and its text on the right.
    const bool any_icon = std::any_of(list_icons.begin(), list_icons.end(),
                                      [](const std::string& p) { return !p.empty(); });
    const float row_h = any_icon ? ImGui::GetTextLineHeightWithSpacing() * 4.2f : 0.0f;
    for (int i = 0; i < static_cast<int>(list_items.size()); ++i) {
        ImGui::PushID(i);
        const bool selected = i == list_selected;
        ImTextureData* icon = i < static_cast<int>(list_icons.size()) ? IconTexture(list_icons[i]) : nullptr;
        const ImVec2 start = ImGui::GetCursorPos();
        const bool clicked =
            any_icon ? ImGui::Selectable("##row", selected,
                                         ImGuiSelectableFlags_AllowDoubleClick |
                                             ImGuiSelectableFlags_AllowOverlap,
                                         ImVec2(0.0f, row_h))
                     : ImGui::Selectable(list_items[i].c_str(), selected,
                                         ImGuiSelectableFlags_AllowDoubleClick);
        if (clicked) {
            list_selected = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                list_result = i;
            }
        }
        if (any_icon) {
            const ImVec2 end = ImGui::GetCursorPos();
            const float pad = row_h * 0.06f;
            const float img_h = row_h - pad * 2.0f;
            const float img_w = icon ? img_h * float(icon->Width) / float(std::max(icon->Height, 1))
                                     : img_h * 228.0f / 128.0f;
            ImGui::SetCursorPos(ImVec2(start.x + pad, start.y + pad));
            if (icon) {
                ImGui::Image(icon->GetTexRef(), ImVec2(img_w, img_h));
            } else {
                ImGui::Dummy(ImVec2(img_w, img_h));
            }
            const float text_x = start.x + pad * 3.0f + img_w;
            const float text_h = ImGui::CalcTextSize(list_items[i].c_str()).y;
            ImGui::SetCursorPos(ImVec2(text_x, start.y + std::max(pad, (row_h - text_h) * 0.5f)));
            ImGui::TextUnformatted(list_items[i].c_str());
            ImGui::SetCursorPos(end);
        }
        if (selected && list_scroll) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    list_scroll = false;
    ImGui::EndChild();
    ImGui::Separator();
    if (list_confirm >= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Apagar este save?   Enter / X (A): sim    Esc / O (B): nao");
    } else if (list_first_deletable >= static_cast<int>(list_items.size())) { // sotc: YES/NO
        ImGui::TextDisabled("Setas / direcional: escolher    Enter / X (A): confirmar    "
                            "Esc / O (B): nao");
    } else {
        ImGui::TextDisabled("Arrows / D-pad: Select    Enter / Cross (A): Confirm    "
                            "Esc / Circle (B): Back    Del / Triangle (Y): Delete");
    }
    ImGui::End();
}

// sotc: the port's loading screen, one look for every stretch where the game shows nothing: the boot shader
// preparation (SOTC_PRETRANSLATE hold, PipelineCache::MaybeHoldForPretranslate), the shader cache warm-up at start,
// and any second or more without a game frame. It fades in and out (LoadingAlpha), so it never cuts hard to the
// game's own loading screen or to the first game frame. A dark gradient backdrop, the title, a thin bar (filled with
// the percentage, count and time left when there is progress to show, a sweeping segment when there is none) and a
// status line; the preparation adds what it is and what closing the window does.
enum class LoadingMode { Stall, Cache, Prep };

ImU32 LoadingRgba(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

// Set by LoadingAlpha: the screen is still on (fading out included), so Visible() keeps the overlay drawn.
std::atomic<bool> loading_on{false};

void LoadingScreen(float alpha, float backdrop) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 size = viewport->WorkSize;
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 far(origin.x + size.x, origin.y + size.y);
    const float unit = size.y / 1080.0f;
    const float t = float(NowMs() % 1000000) / 1000.0f;

    LoadingMode mode = LoadingMode::Stall;
    u32 total = 0, done = 0;
    if (SotcStats::prep_active.load(std::memory_order_acquire)) {
        mode = LoadingMode::Prep;
        total = SotcStats::prep_total.load(std::memory_order_relaxed);
        done = SotcStats::prep_done.load(std::memory_order_relaxed);
    } else if (const u32 w = SotcStats::warmup_total.load(std::memory_order_relaxed);
               w > 0 && !SotcStats::warmup_finished.load(std::memory_order_relaxed)) {
        mode = LoadingMode::Cache;
        total = w;
        done = SotcStats::warmup_done.load(std::memory_order_relaxed);
    }
    done = std::min(done, total);

    // Backdrop: a vertical gradient, near black, a little warmer at the bottom.
    const float bg = std::clamp(backdrop, 0.0f, 1.0f) * alpha;
    draw->AddRectFilledMultiColor(origin, far, LoadingRgba(10, 10, 12, bg), LoadingRgba(10, 10, 12, bg),
                                  LoadingRgba(24, 20, 15, bg), LoadingRgba(24, 20, 15, bg));

    const auto centred = [&](const char* text, float px, float y, ImU32 colour) {
        const float w = ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
        draw->AddText(ImGui::GetFont(), px, ImVec2(origin.x + (size.x - w) * 0.5f, y), colour, text);
    };
    const float breathe = 0.82f + 0.18f * std::sin(t * 1.6f);
    float y = origin.y + size.y * 0.38f;
    centred("SHADOW OF THE COLOSSUS", 46.0f * unit, y, LoadingRgba(214, 184, 120, alpha * breathe));
    y += 46.0f * unit + 56.0f * unit;

    // The bar: filled with progress, a segment sweeping back and forth without.
    const float bar_w = size.x * 0.34f, bar_h = std::max(3.0f, 4.0f * unit);
    const ImVec2 bar0(origin.x + (size.x - bar_w) * 0.5f, y);
    const ImVec2 bar1(bar0.x + bar_w, bar0.y + bar_h);
    draw->AddRectFilled(bar0, bar1, LoadingRgba(255, 255, 255, alpha * 0.10f), bar_h * 0.5f);
    if (total > 0) {
        const float frac = float(done) / float(total);
        const float fill = bar_w * frac;
        draw->AddRectFilled(bar0, ImVec2(bar0.x + fill, bar1.y), LoadingRgba(214, 184, 120, alpha), bar_h * 0.5f);
        // A soft light running along the filled part, so a long stretch at one percentage still moves.
        if (fill > 40.0f * unit) {
            const float sx = bar0.x + fill * std::fmod(t * 0.5f, 1.0f);
            draw->AddRectFilled(ImVec2(std::max(bar0.x, sx - 36.0f * unit), bar0.y), ImVec2(sx, bar1.y),
                                LoadingRgba(255, 240, 205, alpha * 0.45f), bar_h * 0.5f);
        }
    } else {
        const float seg = bar_w * 0.22f;
        const float phase = 0.5f - 0.5f * std::cos(t * 2.2f);
        const float x0 = bar0.x + (bar_w - seg) * phase;
        draw->AddRectFilled(ImVec2(x0, bar0.y), ImVec2(x0 + seg, bar1.y), LoadingRgba(214, 184, 120, alpha * 0.9f),
                            bar_h * 0.5f);
    }
    y += bar_h + 24.0f * unit;

    // The status line, then the numbers.
    char status[160];
    switch (mode) {
    case LoadingMode::Prep:
        std::snprintf(status, sizeof(status), "%s", SotcUi::T("Preparando os shaders do jogo", "Preparing the game's shaders"));
        break;
    case LoadingMode::Cache:
        std::snprintf(status, sizeof(status), "%s", SotcUi::T("Carregando os shaders", "Loading the shaders"));
        break;
    case LoadingMode::Stall:
        std::snprintf(status, sizeof(status), "%s", SotcUi::T("Carregando", "Loading"));
        break;
    }
    centred(status, 24.0f * unit, y, LoadingRgba(225, 225, 225, alpha));
    y += 24.0f * unit + 12.0f * unit;
    if (total > 0) {
        char detail[192];
        const u32 writes = SotcStats::prep_writes.load(std::memory_order_relaxed);
        if (mode == LoadingMode::Prep && done >= total && writes > 0) {
            std::snprintf(detail, sizeof(detail),
                          SotcUi::T("Gravando no disco…  (%u arquivos)", "Writing to disk…  (%u files)"), writes);
        } else {
            // sotc: percent and count only; the time estimate went (the total still grows while the game loads
            // files, so it was unreliable).
            std::snprintf(detail, sizeof(detail), "%d%%  ·  %u / %u",
                          int(float(done) / float(total) * 100.0f + 0.5f), done, total);
        }
        centred(detail, 19.0f * unit, y, LoadingRgba(150, 150, 150, alpha));
        y += 19.0f * unit + 10.0f * unit;
    }
    if (mode == LoadingMode::Prep) {
        centred(SotcUi::T("Feito uma vez só: depois disso o jogo não compila mais shaders durante a partida.",
                          "Done once: after this the game no longer compiles shaders while you play."),
                18.0f * unit, y + 10.0f * unit, LoadingRgba(130, 130, 130, alpha));
        centred(SotcUi::T("Fechar a janela agora interrompe o preparo: ele recomeça na próxima abertura.",
                          "Closing the window now stops the preparation: it starts again next time."),
                16.0f * unit, far.y - 64.0f * unit, LoadingRgba(100, 100, 100, alpha));
    }
    ImGui::End();
}

// sotc: the loading screen's opacity, from the render thread (under imgui_mutex): it fades in over 350 ms while it is
// wanted and out over 300 ms once it is not (the game flips again: its frames show through the fade). backdrop: how
// much of the game's last frame the screen covers; a hitch in play only dims it, and after 2.5 s without a game frame
// (or with progress to show) it is opaque, so a frozen picture does not show through.
float LoadingAlpha(bool wanted, float* backdrop) {
    static long long last_ms = 0;
    static float alpha = 0.0f;
    const long long now = NowMs();
    const long long gap = now - last_ms;
    const float dt = float(gap > 100 || gap < 0 ? 16 : gap); // the first frame after a pause steps one frame
    last_ms = now;
    alpha = wanted ? std::min(1.0f, alpha + dt / 350.0f) : std::max(0.0f, alpha - dt / 300.0f);
    loading_on.store(alpha > 0.0f, std::memory_order_relaxed);
    const bool progress = SotcStats::prep_active.load(std::memory_order_relaxed) ||
                          SotcStats::flip_count.load(std::memory_order_relaxed) == 0;
    const long long stall = now - last_flip_ms.load(std::memory_order_relaxed);
    *backdrop = progress ? 1.0f : std::clamp(0.6f + float(stall - 2500) / 500.0f * 0.4f, 0.6f, 1.0f);
    return alpha;
}

std::atomic<const char*> upscaler_label{nullptr};

// sotc: the upscaler shown while comparing DLSS with FSR1 (F9, vk_dlssg.cpp), top right,
// below the FPS counter when it is shown (fps_bottom: its lower edge this frame, else 0).
void UpscalerLabel(const char* label, float fps_bottom) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const float top = fps_bottom > 0.0f ? fps_bottom + pad * 0.5f : viewport->WorkPos.y + pad;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, top),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##upscaler", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s  (F9)", label);
    ImGui::End();
}

float FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    // Latest frames in order, oldest first.
    float frames[kHistory];
    for (int i = 0; i < hist_count; ++i) {
        frames[i] = frame_hist[(hist_pos - hist_count + i + kHistory) % kHistory];
    }
    const float target = TargetMs();
    float sum = 0.0f, worst = 0.0f;
    int late = 0;
    for (int i = 0; i < hist_count; ++i) {
        sum += frames[i];
        worst = std::max(worst, frames[i]);
        late += frames[i] > target * 1.1f;
    }
    float sorted[kHistory];
    std::copy(frames, frames + hist_count, sorted);
    std::sort(sorted, sorted + hist_count);
    const float p99 = hist_count ? sorted[std::min(hist_count - 1, hist_count * 99 / 100)] : 0.0f;
    const float avg_fps = sum > 0.0f ? 1000.0f * hist_count / sum : 0.0f;
    const float seconds = sum / 1000.0f;

    const ImVec4 green(0.45f, 0.9f, 0.45f, 1.0f), yellow(1.0f, 0.82f, 0.3f, 1.0f),
        red(1.0f, 0.4f, 0.35f, 1.0f), grey(0.7f, 0.7f, 0.7f, 1.0f);
    const auto rate_color = [&](float ms) {
        return ms <= target * 1.1f ? green : ms <= target * 1.6f ? yellow : red;
    };
    const float now_ms = frame_ms_avg;
    ImGui::TextColored(rate_color(now_ms), "%.0f FPS", now_ms > 0.0f ? 1000.0f / now_ms : 0.0f);
    ImGui::SameLine();
    ImGui::TextColored(grey, SotcUi::T("%.1f ms   alvo %.0f FPS", "%.1f ms   target %.0f FPS"), now_ms, 1000.0f / target);

    // Frame time graph, 0 to 2.2x the target, with the target line.
    const float graph_max = target * 2.2f;
    const ImVec2 graph_size(260.0f * base_scale, 54.0f * base_scale);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, late ? yellow : green);
    ImGui::PlotLines("##frametimes", frames, hist_count, 0, nullptr, 0.0f, graph_max, graph_size);
    ImGui::PopStyleColor();
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const float y = hi.y - (hi.y - lo.y) * (target / graph_max);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y),
                                        IM_COL32(120, 200, 255, 160), 1.0f);

    ImGui::Text(SotcUi::T("%.0f s: média %.1f | 1%% low %.1f | pior %.0f ms", "%.0f s: avg %.1f | 1%% low %.1f | worst %.0f ms"), seconds, avg_fps,
                p99 > 0.0f ? 1000.0f / p99 : 0.0f, worst);
    ImGui::TextColored(late ? (late > 3 ? red : yellow) : green, SotcUi::T("Atrasados: %d", "Late: %d"), late);
    ImGui::SameLine();
    ImGui::TextColored(grey, SotcUi::T("| sessão %llu (%.1f%%), travadas >50 ms %llu", "| session %llu (%.1f%%), stutters >50 ms %llu"),
                       static_cast<unsigned long long>(session_late),
                       session_frames ? 100.0 * session_late / session_frames : 0.0,
                       static_cast<unsigned long long>(session_stutters));
    if (SotcStats::kFpsLog) {
        ImGui::TextColored(grey, "CPU %.0f cmds/image | relectures GPU %.0f/s (%.0f ms/s)",
                           SotcStats::ov_cmds_per_frame.load(), SotcStats::ov_readbacks_per_s.load(),
                           SotcStats::ov_readback_ms_per_s.load());
        const int cause = SotcStats::ov_worst_cause.load();
        ImGui::TextColored(grey, "Shaders compiles %u (5 s), %llu session",
                           SotcStats::ov_compiles_window.load(),
                           static_cast<unsigned long long>(SotcStats::ov_compiles_total.load()));
        if (cause >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(grey, SotcUi::T("| pior quadro: %s %.0f ms", "| worst frame: %s %.0f ms"),
                               SotcStats::kFrameCauseNames[cause],
                               SotcStats::ov_worst_cause_ms.load());
        }
    }
    // Frame generation (vk_framegen.h): DLSS-G (vk_dlssg.cpp) or FSR 3 (vk_fsrfg.cpp).
    switch (Vulkan::FrameGen::Current()) {
    case Vulkan::FrameGen::Mode::Off:
        ImGui::TextColored(grey, SotcUi::T("Geração de quadros: desligada", "Frame generation: off"));
        break;
    case Vulkan::FrameGen::Mode::Dlss:
        if (const int fg = SotcStats::ov_fg_state.load(std::memory_order_relaxed); fg == 1) {
            const uint32_t shown = SotcStats::ov_fg_frames.load(std::memory_order_relaxed);
            const uint32_t n = shown ? shown : 2;
            ImGui::TextColored(green, SotcUi::T("DLSS-G x%u: ~%.0f FPS na tela", "DLSS-G x%u: ~%.0f FPS shown"), n, avg_fps * n);
        } else if (fg == 2) {
            ImGui::TextColored(red, "DLSS-G : erreur (statut %#x)",
                               SotcStats::ov_fg_status.load(std::memory_order_relaxed));
        } else {
            ImGui::TextColored(grey, SotcUi::T("DLSS-G: aguardando (sem cena 3D)", "DLSS-G: waiting (no 3D scene)"));
        }
        break;
    case Vulkan::FrameGen::Mode::Fsr:
        if (const int fg = SotcStats::ov_fsr_state.load(std::memory_order_relaxed); fg == 1) {
            ImGui::TextColored(green, SotcUi::T("FSR 3 x2: ~%.0f FPS na tela", "FSR 3 x2: ~%.0f FPS shown"), avg_fps * 2);
        } else if (fg == 2) {
            ImGui::TextColored(red, "FSR 3 : erreur (voir le journal)");
        } else {
            ImGui::TextColored(grey, SotcUi::T("FSR 3: aguardando (sem cena 3D)", "FSR 3: waiting (no 3D scene)"));
        }
        break;
    }
    if (const uint64_t vram = SotcStats::vram_used.load(std::memory_order_relaxed)) {
        ImGui::TextColored(grey, "VRAM %.1f / %.1f Go", vram / 1073741824.0,
                           SotcStats::vram_budget.load(std::memory_order_relaxed) / 1073741824.0);
        ImGui::SameLine();
    }
    ImGui::TextDisabled(SotcUi::T("F7: %s   F8: geração de quadros   F10: esconder", "F7: %s   F8: frame generation   F10: hide"),
                        FpsModeName(FpsMode()));
    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
    return bottom;
}

// sotc: the test run panel (top left, under the overlay), shown when SOTC_RUN_LABEL is set: the offline
// matrix runner (tools/offline_matrix.py) names the configuration and its run, and the panel shows the
// guest frame, the frame time and the per-frame costs. Nothing here runs when SOTC_RUN_LABEL is unset.
struct RunPanelData {
    std::string label, index, flags;
    uint64_t exit_frames = 0; // SOTC_DET_EXIT_FRAMES: the run ends after this many game flips (0: no bar)
    // Per-frame costs, published every kRunWindow game flips. The counters are monotonic; each window
    // takes the difference of two snapshots.
    struct Snapshot {
        uint64_t flips = 0, draw_ns = 0, cp_ns = 0, draws = 0, dispatches = 0;
    };
    Snapshot base;
    bool base_set = false, have_costs = false;
    double draw_ms = 0.0, cp_ms = 0.0, draws_per_frame = 0.0, dispatches_per_frame = 0.0;
};
constexpr uint64_t kRunWindow = 30;

RunPanelData& RunPanelState() {
    static RunPanelData data;
    return data;
}

bool RunPanelOn() {
    static const bool on = [] {
        const char* label = std::getenv("SOTC_RUN_LABEL");
        if (!label || !label[0]) {
            return false;
        }
        RunPanelData& d = RunPanelState();
        d.label = label;
        if (const char* v = std::getenv("SOTC_RUN_INDEX")) {
            d.index = v;
        }
        if (const char* v = std::getenv("SOTC_RUN_FLAGS")) {
            d.flags = v;
        }
        if (const char* v = std::getenv("SOTC_DET_EXIT_FRAMES")) {
            d.exit_frames = std::strtoull(v, nullptr, 10);
        }
        return true;
    }();
    return on;
}

// The text cut to max_width (current font) with "..." at the end; whole UTF-8 characters only.
std::string FitText(const std::string& text, float max_width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    std::string out = text;
    while (!out.empty()) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back(); // a continuation byte: drop the rest of its character too
        }
        if (ImGui::CalcTextSize((out + "...").c_str()).x <= max_width) {
            break;
        }
    }
    return out + "...";
}

// Reads the costs of the window: the draw and command processor CPU time (SOTC_FPS accounting, through
// SotcStats::TakeAccount, a snapshot that resets nothing) and the draw thread's ops (SOTC_DRAW_WORK,
// draw_work_ops, monotonic). Called once per Render, under imgui_mutex.
void SampleRunCosts(RunPanelData& d) {
    RunPanelData::Snapshot now;
    now.flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (SotcStats::kFpsLog) {
        const SotcStats::FrameAccount account = SotcStats::TakeAccount();
        now.draw_ns = account.cpu_ns[SotcStats::RoleDraw];
        now.cp_ns = account.cpu_ns[SotcStats::RoleCp];
    }
    if (SotcStats::kDrawWork) {
        now.draws = SotcStats::draw_work_ops[0].load(std::memory_order_relaxed) +
                    SotcStats::draw_work_ops[1].load(std::memory_order_relaxed);
        now.dispatches = SotcStats::draw_work_ops[2].load(std::memory_order_relaxed);
    }
    if (!d.base_set || now.flips < d.base.flips) { // first call, or the flip count went back
        d.base = now;
        d.base_set = true;
        return;
    }
    const uint64_t frames = now.flips - d.base.flips;
    if (frames < kRunWindow) {
        return;
    }
    const double n = static_cast<double>(frames);
    d.draw_ms = double(now.draw_ns - d.base.draw_ns) / 1e6 / n;
    d.cp_ms = double(now.cp_ns - d.base.cp_ns) / 1e6 / n;
    d.draws_per_frame = double(now.draws - d.base.draws) / n;
    d.dispatches_per_frame = double(now.dispatches - d.base.dispatches) / n;
    d.have_costs = true;
    d.base = now;
}

void RunPanel() {
    RunPanelData& d = RunPanelState();
    SampleRunCosts(d);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const float width = 320.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::Begin("##testrun", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    using SotcUi::T;
    const ImVec4 green(0.45f, 0.9f, 0.45f, 1.0f), yellow(1.0f, 0.82f, 0.3f, 1.0f),
        red(1.0f, 0.4f, 0.35f, 1.0f), grey(0.7f, 0.7f, 0.7f, 1.0f);
    const float avail = width - 2.0f * ImGui::GetStyle().WindowPadding.x;

    // Row 1: the configuration label and the run index ("5/18").
    const std::string index_text = d.index.empty() ? std::string()
                                                   : std::string(T("Execução ", "Run ")) + d.index;
    const float index_w = index_text.empty() ? 0.0f : ImGui::CalcTextSize(index_text.c_str()).x + 12.0f * base_scale;
    ImGui::TextColored(yellow, "%s", FitText(d.label, avail - index_w).c_str());
    if (!index_text.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(grey, "%s", index_text.c_str());
    }

    // Row 2: the switches of the run that differ from the defaults, as the runner wrote them.
    ImGui::TextColored(grey, "%s", FitText(d.flags.empty() ? std::string(T("(padrões)", "(defaults)")) : d.flags,
                                           avail)
                                      .c_str());

    // Row 3: the guest frame, and the progress toward the end when the run has one.
    const uint64_t flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (d.exit_frames) {
        ImGui::Text(T("quadro %llu / %llu", "frame %llu / %llu"), static_cast<unsigned long long>(flips),
                    static_cast<unsigned long long>(d.exit_frames));
        const float fraction = std::min(1.0f, float(double(flips) / double(d.exit_frames)));
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 8.0f * base_scale), "");
    } else {
        ImGui::Text(T("quadro %llu", "frame %llu"), static_cast<unsigned long long>(flips));
    }

    // Row 4: the frame rate and frame time, as the FPS counter (the same smoothed frame time).
    const float target = TargetMs();
    const float now_ms = frame_ms_avg;
    const ImVec4 rate = now_ms <= target * 1.1f ? green : now_ms <= target * 1.6f ? yellow : red;
    ImGui::TextColored(rate, "%.0f FPS", now_ms > 0.0f ? 1000.0f / now_ms : 0.0f);
    ImGui::SameLine();
    ImGui::TextColored(grey, T("%.1f ms/quadro", "%.1f ms/frame"), now_ms);

    // Row 5: the draw thread and command processor CPU time per frame (SOTC_FPS=1), and the ops per frame
    // (SOTC_DRAW_WORK=1). Each says what is missing when its switch is off.
    if (!SotcStats::kFpsLog) {
        ImGui::TextColored(grey, "%s", T("draw / cp: precisa de SOTC_FPS=1", "draw / cp: needs SOTC_FPS=1"));
    } else if (d.have_costs) {
        ImGui::TextColored(grey, T("draw %.1f ms  cp %.1f ms  (por quadro)", "draw %.1f ms  cp %.1f ms  (per frame)"),
                           d.draw_ms, d.cp_ms);
    } else {
        ImGui::TextColored(grey, "%s", T("draw / cp: medindo...", "draw / cp: measuring..."));
    }
    // Row 6: the ops of the draw thread per frame (draws, dispatches) from SOTC_DRAW_WORK.
    if (!SotcStats::kDrawWork) {
        ImGui::TextColored(grey, "%s", T("ops: precisa de SOTC_DRAW_WORK=1", "ops: needs SOTC_DRAW_WORK=1"));
    } else if (d.have_costs) {
        ImGui::TextColored(grey, T("ops %.0f/quadro (draws %.0f, disp %.0f)", "ops %.0f/frame (draws %.0f, disp %.0f)"),
                           d.draws_per_frame + d.dispatches_per_frame, d.draws_per_frame, d.dispatches_per_frame);
    } else {
        ImGui::TextColored(grey, "%s", T("ops: medindo...", "ops: measuring..."));
    }
    ImGui::End();
}

void Toast() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(toast_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    s_instance = &instance;
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && (menu_open || SotcLabMenu::Visible()) && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // sotc: SOTC_HEADLESS=2 (monitor): the window takes no keys and no mouse. It is never focused,
    // but a click or a stray key still reaches it: swallow them, so no hotkey, menu, SotC Lab
    // window or save list responds to the user. Gamepads are left alone (the game's pad input).
    if (Frontend::IsMonitor()) {
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return true;
        default:
            break;
        }
    }
    // sotc: F7 switches 30 -> 60 -> unlimited FPS, as the menu's FPS choice (kept for the launcher).
    // The new state is shown at the next frame (Render).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F7 && !event.key.repeat) {
        SetFpsMode((FpsMode() + 1) % 3);
        return true;
    }
    // sotc: F6 switches uncapped flip pacing: the game's own rate or the vblank grid
    // (sotc_fps_cap.h, videoout driver.cpp).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F6 && !event.key.repeat) {
        const bool paced = SotcFpsCap::TogglePacer();
        SotcEvent::Log("KEY", "%s",
                       paced ? SotcUi::T("F6: ritmo dos quadros suave", "F6: smooth frame pacing")
                             : SotcUi::T("F6: quadros na grade do vblank",
                                         "F6: flips on the vblank grid"));
        return true;
    }
    // sotc: F8 switches the frame generation mode (off, DLSS-G, FSR 3; vk_framegen.h).
    // The new mode is shown at the next frame (Render).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F8 && !event.key.repeat) {
        const Vulkan::FrameGen::Mode mode = Vulkan::FrameGen::Cycle();
        const char* fg_event_names[] = {SotcUi::T("desligada", "off"), "NVIDIA DLSS-G", "AMD FSR 3"};
        SotcEvent::Log("KEY", SotcUi::T("F8: geração de quadros %s", "F8: frame generation %s"),
                       fg_event_names[static_cast<int>(mode)]);
        if (mode == Vulkan::FrameGen::Mode::Off &&
            !Vulkan::FrameGen::Available(Vulkan::FrameGen::Mode::Fsr) &&
            !Vulkan::FrameGen::Available(Vulkan::FrameGen::Mode::Dlss)) {
            toast_text = SotcUi::T("Geração de quadros: indisponível nesta placa", "Frame generation: not available on this GPU");
            toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
        }
        return true;
    }
    // sotc: F5 (diagnostic) saves the textures of the next frame (sotc_stats.h).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5 && !event.key.repeat) {
        SotcStats::tex_capture.store(true, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", SotcUi::T("F5: captura de texturas", "F5: texture capture"));
        toast_text = SotcUi::T("F5: texturas capturadas", "F5: textures captured");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
        return true;
    }
    // sotc: F9 (diagnostic) uploads every cached texture again from guest memory (sotc_stats.h).
    // With the launcher's DLSS comparison on (SOTC_DLSS_COMPARE=1), F9 switches DLSS / FSR 1
    // instead (window.cpp).
    static const bool dlss_compare = [] {
        const char* v = std::getenv("SOTC_DLSS_COMPARE");
        return v && v[0] == '1';
    }();
    if (!dlss_compare && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 &&
        !event.key.repeat) {
        SotcStats::reload_all.store(true, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", SotcUi::T("F9: recarregar texturas", "F9: reload textures"));
        toast_text = SotcUi::T("F9: texturas recarregadas", "F9: textures reloaded");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
        return true;
    }
    // sotc: SotC Lab (docs/sotc/LAB.md), only with SOTC_LAB=1: F2 menu, F3 free-cam, F4 route
    // waypoint while flying; mouse motion and wheel steer the free camera while it flies.
    if (SotcLabMenu::Available()) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F2) {
            SotcLabMenu::Toggle();
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F3) {
            // The free camera runs on the engine's debug camera and console commands (freecam.cpp).
            const bool on = !SotcFreecam::Active();
            if (!SotcFreecam::SetActive(on) && on) {
                toast_text = SotcUi::T("Câmera livre indisponível (comandos do motor não encontrados)",
                                       "Free camera unavailable (engine commands not found)");
            } else {
                toast_text = on ? SotcUi::T("F3: câmera livre ligada", "F3: free camera on")
                                : SotcUi::T("F3: câmera livre desligada", "F3: free camera off");
            }
            toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F4 &&
            SotcFreecam::Active()) {
            SotcAutopilot::AddWaypoint(SotcFreecam::Get().cam);
            toast_text = SotcUi::T("F4: ponto de rota adicionado", "F4: route waypoint added");
            toast_until_ms.store(NowMs() + 1500, std::memory_order_relaxed);
            return true;
        }
        // G while flying: free-cam ends with Wander left where the camera is (teleport).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_G &&
            SotcFreecam::Active() && !SotcLabMenu::Visible() && !menu_open) {
            SotcFreecam::LeaveHere();
            toast_text = SotcUi::T("G: Wander deixado aqui", "G: Wander left here");
            toast_until_ms.store(NowMs() + 1500, std::memory_order_relaxed);
            return true;
        }
        if (SotcFreecam::Active() && !SotcLabMenu::Visible() && !menu_open) {
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                SotcFreecam::FeedMouse(event.motion.xrel, event.motion.yrel, 0.0f);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                SotcFreecam::FeedMouse(0.0f, 0.0f, event.wheel.y);
            }
        }
    }
    // sotc: F10 shows or hides the performance overlay.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat) {
        const bool on = !OverlayOn();
        BbSettings::Get().show_fps = false; // the ini setting only gives the initial state
        overlay_on.store(on, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", on ? SotcUi::T("F10: painel de desempenho mostrado", "F10: performance overlay shown")
                                       : SotcUi::T("F10: painel de desempenho escondido", "F10: performance overlay hidden"));
        return true;
    }
    if (list_active && !menu_open && list_result == -1) {
        const int count = static_cast<int>(list_items.size());
        const auto move = [&](int delta) {
            list_confirm = -1;
            if (count) {
                list_selected = (list_selected + delta + count) % count;
                list_scroll = true;
            }
        };
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            switch (event.key.key) {
            case SDLK_UP:
            case SDLK_W:
                move(-1);
                break;
            case SDLK_DOWN:
            case SDLK_S:
                move(1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                if (!event.key.repeat && list_confirm >= 0) {
                    list_result = -100 - list_confirm; // delete it (PollSaveList)
                    list_confirm = -1;
                } else if (!event.key.repeat && count) {
                    list_result = list_selected;
                }
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                if (!event.key.repeat) {
                    if (list_confirm >= 0) {
                        list_confirm = -1;
                    } else {
                        list_result = -2;
                    }
                }
                break;
            case SDLK_DELETE:
                if (!event.key.repeat && list_selected >= list_first_deletable &&
                    list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                move(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                move(1);
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                if (list_confirm >= 0) {
                    list_result = -100 - list_confirm;
                    list_confirm = -1;
                } else if (count) {
                    list_result = list_selected;
                }
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                if (list_confirm >= 0) {
                    list_confirm = -1;
                } else {
                    list_result = -2;
                }
                break;
            case SDL_GAMEPAD_BUTTON_NORTH:
                if (list_selected >= list_first_deletable && list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                const bool down = event.gaxis.value > 20000, up = event.gaxis.value < -20000;
                if (down && !list_stick_down) {
                    move(1);
                }
                if (up && !list_stick_up) {
                    move(-1);
                }
                list_stick_down = down;
                list_stick_up = up;
            }
            return true;
        case SDL_EVENT_MOUSE_MOTION: {
            const float density = PixelDensity(event.motion.windowID);
            io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                io.AddMouseButtonEvent(0, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
            return true;
        default:
            break;
        }
    }
    const bool is_open = menu_open;
    // sotc: the SotC Lab window (F2) also takes keyboard and mouse input while it is shown.
    const bool feed = is_open || SotcLabMenu::Visible();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const bool toggle_key = (event.key.key == SDLK_INSERT || event.key.key == SDLK_F1);
        if (down && !event.key.repeat && MenuAllowed() &&
            (toggle_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(toggle_key ? !is_open : false);
            return true;
        }
        if (down && !event.key.repeat && !is_open && event.key.key == SDLK_ESCAPE &&
            SotcLabMenu::Visible()) {
            SotcLabMenu::Toggle(); // sotc: Esc closes the SotC Lab window
            return true;
        }
        if (!feed) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down && MenuAllowed()) {
            SetOpen(!is_open);
            return true;
        }
        if (!feed) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!feed) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!feed) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!feed) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!feed) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool LoadingShown() {
    // sotc: a second without a game flip, the boot before the game's first flip, or the boot shader preparation (the
    // game is held: no flip comes). Read from the presenter and the video out threads: the races only move the moment
    // by a frame.
    static std::atomic<u64> seen{~u64{0}};
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    const long long now = NowMs();
    const bool prep = SotcStats::prep_active.load(std::memory_order_acquire);
    if (flips == 0) {
        return true;
    }
    if (seen.exchange(flips, std::memory_order_relaxed) != flips) {
        last_flip_ms.store(now, std::memory_order_relaxed);
        return prep;
    }
    return prep || now - last_flip_ms.load(std::memory_order_relaxed) >= 1000;
}

// sotc: a SOTC_MENU_TRIGGER / SOTC_SET_TRIGGER file waits (scripted tests): Render handles it.
bool TriggerPending() {
    static const char* menu = std::getenv("SOTC_MENU_TRIGGER");
    static const char* set = std::getenv("SOTC_SET_TRIGGER");
    std::error_code ec;
    return (menu && std::filesystem::exists(menu, ec)) || (set && std::filesystem::exists(set, ec));
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || list_active || OverlayOn() ||
                           SotcLabMenu::Visible() ||
                           ToastShown() || LoadingShown() || loading_on.load(std::memory_order_relaxed) ||
                           TriggerPending() ||
                           upscaler_label.load(std::memory_order_relaxed));
}

void SetUpscalerLabel(const char* label) {
    upscaler_label.store(label, std::memory_order_relaxed);
}

bool CapturesInput() {
    return menu_open || text_entry_active || list_active || SotcLabMenu::Visible();
}

bool OpenSaveList(const std::string& title, const std::vector<std::string>& items, int focus,
                  int first_deletable) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false; // nothing could show it
    }
    list_title = title;
    list_items = items;
    list_icons = std::move(pending_icons); // sotc: the pictures sent just before (or none)
    pending_icons.clear();
    list_selected = items.empty() ? 0 : std::clamp(focus, 0, static_cast<int>(items.size()) - 1);
    list_result = -1;
    list_confirm = -1;
    list_first_deletable = first_deletable < 0 ? static_cast<int>(items.size()) : first_deletable;
    list_scroll = true;
    list_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
    return true;
}

void SetSaveListIcons(std::vector<std::string> paths) {
    std::scoped_lock lock{imgui_mutex};
    pending_icons = std::move(paths);
}

int PollSaveList() {
    std::scoped_lock lock{imgui_mutex};
    if (!list_active || list_result == -1) {
        return -1;
    }
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = menu_open.load();
    }
    return list_result;
}

void CloseSaveList() {
    std::scoped_lock lock{imgui_mutex};
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = menu_open.load();
    }
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent,
            bool game_frame) {
    // Present interval for the FPS readout (measured also while nothing is drawn), game frames
    // only: a generated frame (FSR 3) is drawn half a frame later.
    const auto now = std::chrono::steady_clock::now();
    static auto last_draw = now;
    const float draw_ms = std::chrono::duration<float, std::milli>(now - last_draw).count();
    last_draw = now;
    if (game_frame) {
        const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
        last_present = now;
        if (ms > 0.0f && ms < 1000.0f) {
            frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        }
        if (ms > 0.0f && ms < 5000.0f) {
            RecordFrame(ms);
        }
    }
    const float ms = draw_ms;
    // The frame generation mode changed (F8): shown for 2.5 s.
    static Vulkan::FrameGen::Mode shown_mode = Vulkan::FrameGen::Current();
    if (const Vulkan::FrameGen::Mode mode = Vulkan::FrameGen::Current(); mode != shown_mode) {
        shown_mode = mode;
        std::scoped_lock lock{imgui_mutex};
        toast_text = std::string(SotcUi::T("Geração de quadros: ", "Frame generation: ")) + Vulkan::FrameGen::Name(mode);
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    // The flip pacing was switched (F6).
    static bool shown_paced = SotcFpsCap::flip_pacer;
    if (const bool paced = SotcFpsCap::flip_pacer; paced != shown_paced) {
        shown_paced = paced;
        std::scoped_lock lock{imgui_mutex};
        toast_text = paced ? SotcUi::T("Ritmo dos quadros: suave (ritmo do jogo)",
                                       "Frame pacing: smooth (game's rate)")
                           : SotcUi::T("Ritmo dos quadros: grade do vblank (original)",
                                       "Frame pacing: vblank grid (original)");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    // The FPS mode was switched (F7 or the menu).
    static int shown_fps_mode = FpsMode();
    if (const int fps_mode = FpsMode(); fps_mode != shown_fps_mode) {
        shown_fps_mode = fps_mode;
        std::scoped_lock lock{imgui_mutex};
        toast_text = FpsModeName(fps_mode);
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    // sotc: SOTC_TRACE: a finished dump shows a toast for 4 s. LastDump is polled at most every
    // 250 ms; the toast shows when the dump serial changed (ToastShown keeps Visible() true).
    if (SotcTrace::kOn) {
        static long long next_poll_ms = 0;
        static uint64_t shown_serial = 0;
        if (const long long now_ms = NowMs(); now_ms >= next_poll_ms) {
            next_poll_ms = now_ms + 250;
            const SotcTrace::DumpInfo dump = SotcTrace::LastDump();
            if (dump.serial != shown_serial) {
                shown_serial = dump.serial;
                if (dump.serial != 0) {
                    char num[96];
                    std::snprintf(num, sizeof num, "%llu, %.0f ms",
                                  static_cast<unsigned long long>(dump.frame), dump.worst_ms);
                    const std::string file = std::filesystem::path(dump.path).filename().string();
                    std::scoped_lock lock{imgui_mutex};
                    toast_text = std::string(SotcUi::T("Trace: quadro ", "Trace: frame ")) + num +
                                 " \xE2\x86\x92 " + file;
                    toast_until_ms.store(now_ms + 4000, std::memory_order_relaxed);
                }
            }
        }
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    const bool loading = LoadingShown() && !list_active && !text_entry_active;
    float loading_backdrop = 1.0f;
    const float loading_alpha = LoadingAlpha(loading, &loading_backdrop);
    static bool was_loading = false;
    static long long loading_since = 0;
    if (loading != was_loading) { // sotc: one log line when it appears and when it goes away
        was_loading = loading;
        if (loading) {
            loading_since = NowMs();
            std::fprintf(stderr, "SOTC loading screen: shown\n");
            SotcEvent::Log("GAME", "%s", SotcUi::T("Tela de carregamento (o jogo parou de mostrar quadros)",
                                                   "Loading screen (the game stopped showing frames)"));
        } else {
            std::fprintf(stderr, "SOTC loading screen: hidden after %.1f s\n",
                         double(NowMs() - loading_since) / 1000.0);
            SotcEvent::Log("GAME", SotcUi::T("Fim da tela de carregamento (%.1f s)", "Loading screen over (%.1f s)"),
                           double(NowMs() - loading_since) / 1000.0);
        }
    }
    if (loading_alpha > 0.0f) {
        LoadingScreen(loading_alpha, loading_backdrop);
    }
    // sotc: SOTC_MENU_TRIGGER=<file>: when the file appears it is removed and the menu opens or
    // closes (scripted pictures of the menu: the "menu" event of tools/sotc_play.py).
    static const char* menu_trigger = std::getenv("SOTC_MENU_TRIGGER");
    if (menu_trigger && MenuAllowed() && std::remove(menu_trigger) == 0) {
        SetOpen(!menu_open);
    }
    // sotc: SOTC_SET_TRIGGER=<file>: "tex=off|high|max", "fps=30|60|unlimited",
    // "up=fsr1|bilinear" or "rcas=high|medium|low|off" in the file sets that quick setting as the
    // menu does, then the file is removed (scripted tests: tools/sotc_play.py).
    static const char* set_trigger = std::getenv("SOTC_SET_TRIGGER");
    if (set_trigger) {
        if (std::FILE* f = std::fopen(set_trigger, "rb")) {
            char line[256] = {}; // sotc: room for SotC Lab command lines
            std::fread(line, 1, sizeof(line) - 1, f);
            std::fclose(f);
            std::remove(set_trigger);
            const std::string text{line};
            const std::string key = text.substr(0, text.find('='));
            const char v = text.size() > key.size() + 1 ? text[key.size() + 1] : 0;
            if (key == "tex") {
                SetTextureLevel(v == 'o' ? 0 : v == 'm' ? 2 : 1);
            } else if (key == "fps") {
                SetFpsMode(v == '3' ? 0 : v == '6' ? 1 : 2);
            } else if (key == "up") {
                SetUpscaler(v == 'f' ? 0 : 1);
            } else if (key == "rcas") {
                SetRcas(v == 'h' ? 0 : v == 'm' ? 1 : v == 'l' ? 2 : 3);
            } else if (SotcLab::Enabled()) {
                // sotc: any other line is a SotC Lab command (lab_commands.h), e.g. "boss 3".
                std::string cmd = text.substr(0, text.find_first_of("\r\n"));
                std::fprintf(stderr, "SOTC SET: %s -> %s\n", cmd.c_str(),
                             SotcLab::Execute(cmd).c_str());
            }
            std::fprintf(stderr, "SOTC SET: %s\n", line);
        }
    }
    if (menu_open) {
        Menu();
    }
    SotcLabMenu::Draw(); // sotc: SotC Lab (F2); draws nothing when hidden
    float fps_bottom = 0.0f;
    if (OverlayOn() && !menu_open) {
        fps_bottom = FpsCounter();
        if (RunPanelOn()) { // sotc: SOTC_RUN_LABEL, the test run panel (top left)
            RunPanel();
        }
    }
    if (ToastShown()) {
        Toast();
    }
    if (const char* label = upscaler_label.load(std::memory_order_relaxed)) {
        UpscalerLabel(label, fps_bottom);
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (list_active && !menu_open) {
        SaveListBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
