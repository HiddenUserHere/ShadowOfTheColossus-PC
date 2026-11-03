// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    void SetFullscreen(bool fs);
    bool IsFullscreen() const;
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    std::string title_suffix; ///< sotc: " · v01.01 · 60 FPS" (game version and frame rate)
    u64 title_flips{};
    u64 title_ticks{};
    void UpdateTitleStats();
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

/// sotc: SOTC_HEADLESS=1 (read once): the game window is created hidden and never takes the focus
/// or the mouse; rendering and presentation go on as usual (see window.cpp). SOTC_HEADLESS=2
/// (monitor) is headless too, but the window is shown in a corner, still without the focus.
bool IsHeadless();
/// sotc: SOTC_HEADLESS=2 (read once): the game window is a small visible monitor window, and the
/// performance overlay is always on. It takes no keys and no mouse (bbport_overlay.cpp).
bool IsMonitor();
/// sotc: the HWND of the headless window under SOTC_HEADLESS (Windows), nullptr otherwise.
void* HeadlessWindowHwnd();

} // namespace Frontend
