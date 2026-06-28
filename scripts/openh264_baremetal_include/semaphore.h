// Minimal <semaphore.h> for the bare-metal aarch64 openh264 decoder build. newlib
// ships no semaphore.h and no sem_t, so we define an opaque sem_t here. Only the
// openh264 multi-threaded decode path uses semaphores (WELS_EVENT = sem_t*); the
// single-threaded DecodeFrameNoDelay path never does, so the no-op shim bodies
// (openh264_baremetal_shim.c) are never exercised at runtime — they exist only so
// the threading TUs link. See scripts/build_openh264_baremetal.sh.
#ifndef OPENH264_BAREMETAL_SEMAPHORE_H
#define OPENH264_BAREMETAL_SEMAPHORE_H

#include <time.h>  // struct timespec

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __value; } sem_t;

int sem_init(sem_t*, int, unsigned int);
int sem_destroy(sem_t*);
int sem_wait(sem_t*);
int sem_trywait(sem_t*);
int sem_timedwait(sem_t*, const struct timespec*);
int sem_post(sem_t*);
int sem_getvalue(sem_t*, int*);

#ifdef __cplusplus
}
#endif

#endif  // OPENH264_BAREMETAL_SEMAPHORE_H
