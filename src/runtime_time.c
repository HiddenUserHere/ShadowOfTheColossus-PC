/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
    struct timeval t;
#ifdef _WIN32
    if (gettimeofday(&t,NULL)) { perror("STOP: host gettimeofday failed"); exit(21); }
    if (zone) { zone->minuteswest=(int32_t)(-compat_utc_offset((time_t)t.tv_sec)/60); zone->dsttime=0; }
#else
    struct timezone z;
    if (gettimeofday(&t,&z)) {
        /* Guest errno/TLS failure path is not implemented yet. */
        perror("STOP: host gettimeofday failed"); exit(21);
    }
    if (zone) { zone->minuteswest=z.tz_minuteswest; zone->dsttime=z.tz_dsttime; }
#endif
    if (runtime_det_clock_on()) { /* sotc: SOTC_DET_CLOCK: the wall clock is virtual (the zone stays the host's) */
        const uint64_t ns=runtime_det_realtime_ns();
        t.tv_sec=(time_t)(ns/1000000000u); t.tv_usec=(long)(ns%1000000000u/1000u);
    }
    if (value) { value->seconds=t.tv_sec; value->microseconds=t.tv_usec; }
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (!strcmp(name,"n88vx3C5nW8")) return (uintptr_t)guest_gettimeofday;
    return 0;
}
