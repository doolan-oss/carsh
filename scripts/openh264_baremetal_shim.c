// No-op POSIX-threading shim for the bare-metal aarch64 openh264 decoder. The
// single-threaded DecodeFrameNoDelay path (the only one carsh uses) NEVER spawns a
// thread; WelsThreadLib.cpp still references these symbols, which newlib bare metal
// does not provide. Mutex/cond/sem become no-ops (safe: no concurrency without
// threads); pthread_create fails so any accidental thread-pool init bails out
// instead of silently running serial work as if parallel. Built into
// libopenh264.a by scripts/build_openh264_baremetal.sh.
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stddef.h>  // size_t
#include <unistd.h>  // useconds_t / usleep decl

// --- mutexes: no-ops (single-threaded => never contended) --------------------
int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) { (void)m; (void)a; return 0; }
int pthread_mutex_destroy(pthread_mutex_t* m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t* m) { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t* m) { (void)m; return 0; }

// --- condition variables: no-ops --------------------------------------------
int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) { (void)c; (void)a; return 0; }
int pthread_cond_destroy(pthread_cond_t* c) { (void)c; return 0; }
int pthread_cond_signal(pthread_cond_t* c) { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t* c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { (void)c; (void)m; return 0; }
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) {
    (void)c; (void)m; (void)t; return 0;
}

// --- threads: creation fails (we never run multi-threaded decode) ------------
int pthread_create(pthread_t* th, const pthread_attr_t* a, void* (*fn)(void*), void* arg) {
    (void)th; (void)a; (void)fn; (void)arg; return EAGAIN;
}
int pthread_join(pthread_t th, void** ret) { (void)th; (void)ret; return 0; }
pthread_t pthread_self(void) { return (pthread_t)0; }
int pthread_setname_np(pthread_t th, const char* name) { (void)th; (void)name; return 0; }

int pthread_attr_init(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_setscope(pthread_attr_t* a, int s) { (void)a; (void)s; return 0; }
int pthread_attr_setschedpolicy(pthread_attr_t* a, int p) { (void)a; (void)p; return 0; }

// --- semaphores: no-ops (only the unused threaded path touches them) ---------
int sem_init(sem_t* s, int p, unsigned int v) { (void)s; (void)p; (void)v; return 0; }
int sem_destroy(sem_t* s) { (void)s; return 0; }
int sem_wait(sem_t* s) { (void)s; return 0; }
int sem_trywait(sem_t* s) { (void)s; return 0; }
int sem_timedwait(sem_t* s, const struct timespec* t) { (void)s; (void)t; return 0; }
int sem_post(sem_t* s) { (void)s; return 0; }
int sem_getvalue(sem_t* s, int* v) { (void)s; if (v) *v = 0; return 0; }

// --- processor-count probe: report "unknown" so openh264 falls back to 1 -----
int sched_getaffinity(int pid, size_t setsize, cpu_set_t* set) {
    (void)pid; (void)setsize; (void)set; return -1;  // fail -> sysconf path -> serial
}

// usleep: newlib declares it but ships no body, and Circle provides no syscall.
// Only openh264's threaded event-wait spins call it; the single-threaded decode
// path never does, so a no-op is safe (never sleeps on the hot path).
int usleep(useconds_t usec) { (void)usec; return 0; }
