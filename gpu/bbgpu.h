/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* sotc: the GPU command thread or the draw recording thread (faults handled inline). */
int bbgpu_is_gpu_side_thread(void);
/* sotc: game flips so far (the loader's hang watchdog). */
uint64_t bbgpu_flip_count(void);
/* sotc: the game began a load; the texture cache drops unused images at the next submit. */
void bbgpu_request_texture_flush(void);
/* sotc: 1 once the boot's shader cache preload is over. */
int bbgpu_warmup_finished(void);
/* sotc: guest fiber stacks; the page tracker keeps them accessible while registered. */
void bbgpu_register_guest_stack(uintptr_t address, uint64_t size);
void bbgpu_register_guest_stack_end(uintptr_t address, uint64_t size);
/* sotc: readbacks mode (0 disabled, 1 relaxed, 2 precise). */
int bbgpu_readbacks_mode(void);
int bbgpu_page_read_watched(uintptr_t address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
/* sotc: save/load slot list; poll: -1 open, -2 cancelled, -100 - i: delete item i, else the
 * chosen item. Items from first_deletable on can be deleted (-1: none). */
int bbgpu_save_list_open(const char *title, const char *const *items, int count, int focus,
                         int first_deletable);
int bbgpu_save_list_poll(void);
void bbgpu_save_list_close(void);
/* sotc: pictures for the next save list (PNG paths, NULL: none), item by item. */
void bbgpu_save_list_icons(const char *const *paths, int count);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* sotc: mouse motion since the last call and held buttons (bit = SDL button); 0 when not captured. */
int bbgpu_mouse_take(float *dx, float *dy, unsigned *buttons);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* sotc: SotC Lab free-cam (gpu/sotc/freecam.h). Non-zero while it flies: the game gets a neutral pad
 * and the host pad is handed to the free camera (sticks/triggers -1..1 / 0..1, PS4 button bits). */
int bbgpu_freecam_captures_input(void);
void bbgpu_freecam_feed_pad(float lx, float ly, float rx, float ry, float l2, float r2,
                            uint32_t buttons);
/* sotc: crash report. hook runs once when the GPU library stops the port (an assertion: a Vulkan device lost ends
 * here), after its own stack is printed: the loader passes its every-thread stack dump. NULL removes it. */
void bbgpu_set_fatal_hook(void (*hook)(void));
/* sotc: crash report. With SOTC_TRACE=1 or 2: the last window_ms of the recorder go to a file (reason in its META),
 * waiting up to wait_ms for the writer. Any thread; for the fatal paths (no SOTC_TRACE_MAX limit). Returns 1 when
 * the file was written. */
int bbgpu_trace_fatal_dump(const char *reason, unsigned window_ms, unsigned wait_ms);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
/* sotc: where the engine keeps the pointer to its TargetFramesPerSeconds value (fps60game mode):
 * F7 then switches the game's own frame rate target instead of only its cap. */
void bbgpu_set_fps_target_slot(void *slot);
/* sotc: SOTC_STALL_DETAIL: the calling guest thread's CPU time is counted (src/runtime_thread.c host_start). */
void bbgpu_register_guest_thread(void);
/* sotc: SOTC_WRITE_DECL. The CPU is about to write [address, address+size) of guest memory
 * (a host memcpy/memset/memmove or file read on behalf of the guest; return_address is the
 * guest caller, for attribution). =1: the GPU-tracked pages of the span are taken as written
 * now, as one write fault per page would, but in one pass (page_manager.cpp DeclareCpuWrite).
 * =2: only counted (the would-be declared pages per caller, SOTC WRITE DECL report line).
 * Unset/0: returns at once. Returns the number of tracked pages found in the span. */
uint64_t bbgpu_declare_cpu_write(void *address, uint64_t size, uint64_t return_address);
/* sotc: PC-native write announces for the runtime's copies (SOTC_NATIVE, gpu/sotc/native/native_hooks.cpp).
 * A code is 0 for nothing to announce, else 1 + SotcNative::Kind (1 Persistent, 2 PerFrame).
 * bbgpu_native_guest_write_kind(return_address): the code for a guest memcpy/memset/memmove whose caller is
 * return_address (PerFrame for every caller while SOTC_NATIVE is on); lock-free. bbgpu_native_announce_write(dst, size, code)
 * announces [dst, dst+size) once the bytes landed; it takes a mutex (call it on the host stack). */
uint32_t bbgpu_native_guest_write_kind(uint64_t return_address);
void bbgpu_native_announce_write(void *dst, uint64_t size, uint32_t code);
/* sotc: a file read landed [dst, dst+size) in guest memory: announced PerFrame (marked, never uploaded by the
 * announce). Takes a mutex: call it on the host stack. */
void bbgpu_native_announce_file_read(void *dst, uint64_t size);
#ifdef __cplusplus
}
#endif
#endif
