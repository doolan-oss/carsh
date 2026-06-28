// Minimal <pthread.h> for the bare-metal aarch64 openh264 decoder build. newlib
// (aarch64-none-elf) ships the pthread TYPES in <sys/_pthreadtypes.h> but no
// <pthread.h> and no function bodies. openh264's single-threaded DecodeFrameNoDelay
// path never spawns a thread; it only REFERENCES these symbols, which the no-op
// shim (openh264_baremetal_shim.c) satisfies. Pulled in only for this lib (see
// scripts/build_openh264_baremetal.sh), never by Circle code.
#ifndef OPENH264_BAREMETAL_PTHREAD_H
#define OPENH264_BAREMETAL_PTHREAD_H

// <sys/types.h> first: it sets the feature macros (__POSIX_VISIBLE) THEN pulls in
// <sys/_pthreadtypes.h>. Including _pthreadtypes.h cold leaves the pthread types
// guarded-out (the guard needs __POSIX_VISIBLE >= 199506, set via features.h).
#include <sys/types.h>  // pthread_t / _mutex_t / _cond_t / _attr_t / _condattr_t
#include <sched.h>              // SCHED_FIFO
#include <time.h>               // struct timespec
#include <stddef.h>             // size_t

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PTHREAD_SCOPE_SYSTEM
#define PTHREAD_SCOPE_SYSTEM 0
#endif

// GNU CPU-affinity bits openh264 references in its processor-count probe. We stub
// sched_getaffinity to fail so it falls through to sysconf(); CPU_COUNT is left
// undefined so that code branch is skipped.
typedef struct { unsigned long __bits[16]; } cpu_set_t;
int sched_getaffinity(int __pid, size_t __setsize, cpu_set_t* __set);

// cpu_set_t accessor macros (GNU). Only need to COMPILE — sched_getaffinity above
// always fails, so the code guarded by it never runs.
#define __CPU_BITS (8 * sizeof(unsigned long))
#define CPU_SETSIZE ((int)(sizeof(cpu_set_t) * 8))
#define CPU_ZERO(set) do { for (unsigned __i = 0; __i < sizeof(cpu_set_t)/sizeof(unsigned long); ++__i) (set)->__bits[__i] = 0UL; } while (0)
#define CPU_ISSET(i, set) (((set)->__bits[(i) / __CPU_BITS] >> ((i) % __CPU_BITS)) & 1UL)
#define CPU_SET(i, set) ((set)->__bits[(i) / __CPU_BITS] |= (1UL << ((i) % __CPU_BITS)))

int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
pthread_t pthread_self(void);
int pthread_setname_np(pthread_t, const char*);

int pthread_attr_init(pthread_attr_t*);
int pthread_attr_destroy(pthread_attr_t*);
int pthread_attr_setscope(pthread_attr_t*, int);
int pthread_attr_setschedpolicy(pthread_attr_t*, int);

int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int pthread_mutex_destroy(pthread_mutex_t*);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);

int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int pthread_cond_destroy(pthread_cond_t*);
int pthread_cond_signal(pthread_cond_t*);
int pthread_cond_broadcast(pthread_cond_t*);
int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);

#ifdef __cplusplus
}
#endif

#endif  // OPENH264_BAREMETAL_PTHREAD_H
