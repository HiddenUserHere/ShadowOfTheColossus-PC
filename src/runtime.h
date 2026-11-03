#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in bb_longjmp
 * (gpu/shim/bbport_toggles.h uses the same buffer type). */
typedef bb_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
void runtime_content_title(const char *id);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
/* sceKernelReadTsc/GetTscFrequency read QPC instead of the TSC (cores out of step). */
void runtime_tsc_use_qpc(void);
/* sotc: SOTC_DET_CLOCK, the virtual guest clock (runtime_detclock.c). Off: one cached flag test. */
int runtime_det_clock_on(void);
uint64_t runtime_det_now_ns(void);          /* guest monotonic time since process start; advances */
uint64_t runtime_det_realtime_ns(void);     /* guest wall clock (unix ns); advances */
uint64_t runtime_det_from_unix_ns(uint64_t unix_ns); /* guest wall clock to virtual time, no advance */
void runtime_det_advance_ns(uint64_t ns);   /* a sleep of ns: advances virtual time, clamped */
uint64_t runtime_det_tsc(void);             /* rdtsc replacement (Win64 callable), in ns */
uint64_t runtime_det_tsc_frequency(void);   /* 1000000000 */
void runtime_det_report(void);
/* A guest timed wait: the virtual deadline (det) or the host deadline (off). */
typedef struct RuntimeWait { clockid_t clock; struct timespec real_end; uint64_t vend_ns; int det; } RuntimeWait;
void runtime_wait_relative(RuntimeWait *w, clockid_t clock, uint64_t timeout_us);
void runtime_wait_absolute(RuntimeWait *w, clockid_t clock, const struct timespec *guest_abs);
/* Runs attempt(ctx, end) (a host timed call on the condvar/lock) until it succeeds, fails other than
 * ETIMEDOUT, or the guest timeout expires (then ETIMEDOUT). Off: a single attempt with real_end. */
int runtime_wait_timed(RuntimeWait *w, int (*attempt)(void *ctx, const struct timespec *end), void *ctx);
uint32_t runtime_wait_remaining_us(const RuntimeWait *w);
int runtime_wait_cond(RuntimeWait *w, pthread_cond_t *cv, pthread_mutex_t *m);
int runtime_wait_mutex(RuntimeWait *w, pthread_mutex_t *m);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
uintptr_t runtime_sotc_resolve(const char *name);
int runtime_thread_is_guest(void);
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
/* sotc: one "SOTC USLEEP:" line for the SOTC_FPS report; resets the usleep counters (runtime_kernel.c). */
void runtime_usleep_stats(char *out, size_t size, double seconds);
/* sotc: SOTC_MUTEX_SPIN_US stats (src/runtime_mutex.c), printed by the driver.cpp 5 s report. */
void runtime_mutex_spin_stats(char *out, size_t size, double seconds);
/* sotc: "SOTC YIELD:" line (scePthreadYield counters and SOTC_YIELD_BACKOFF) and "SOTC CPU:" line (process
 * CPU share), printed by the driver.cpp 5 s report (src/runtime_thread.c). */
void runtime_yield_stats(char *out, size_t size, double seconds);
void runtime_cpu_stats(char *out, size_t size, double seconds);
/* sotc: runs fn(arg) on the thread's host stack (probe.c): the fault handler's alternate stack when
 * the caller runs on a guest fiber stack, the current stack otherwise. Reentrant: see probe.c. */
#ifdef __cplusplus
extern "C"
#endif
void runtime_call_on_host_stack(void (*fn)(void *), void *arg);
#endif
