// HeapProbe — replaces the global operator new/delete for the test binary so
// AllocScope (HeapProbe.h) can record allocation sizes. Outside a scope these are
// plain malloc/free. The plain, nothrow and array forms are all replaced so every
// allocation and its deallocation go through the same malloc/free pair — under
// ASan the sanitizer supplies its own nothrow/array operator new (used e.g. by
// std::stable_sort's temporary buffer), which would otherwise be freed by our
// operator delete and trip alloc-dealloc-mismatch. The aligned forms keep their
// own (unrecorded) allocate/free pair.
//
// NOTE: a program may define these only once — if another test ever needs its
// own allocation hook, extend this one instead of adding a second definition.
#include "HeapProbe.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<int> g_active{0};
std::atomic<std::size_t> g_count{0}, g_max{0}, g_total{0};

void record(std::size_t n) {
    if (g_active.load(std::memory_order_relaxed) == 0) return;
    g_count.fetch_add(1, std::memory_order_relaxed);
    g_total.fetch_add(n, std::memory_order_relaxed);
    std::size_t cur = g_max.load(std::memory_order_relaxed);
    while (n > cur &&
           !g_max.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
    }
}
}  // namespace

void* operator new(std::size_t n) {
    record(n);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    record(n);
    return std::malloc(n ? n : 1);
}

void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace carsh::testprobe {

AllocScope::AllocScope() {
    g_count.store(0);
    g_max.store(0);
    g_total.store(0);
    g_active.store(1);
}

AllocScope::~AllocScope() { g_active.store(0); }

AllocStats AllocScope::stats() const {
    AllocStats s;
    s.count = g_count.load();
    s.maxBytes = g_max.load();
    s.totalBytes = g_total.load();
    return s;
}

}  // namespace carsh::testprobe
