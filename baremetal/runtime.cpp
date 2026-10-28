// Minimal C/C++ runtime for the bare-metal build.
//
// Everything is compiled against the normal libstdc++/glibc *headers*, but
// nothing from libc or libstdc++.so is linked. This file supplies the handful
// of out-of-line symbols those headers end up referencing: memory and string
// functions, strtol, a heap and the C++ ABI hooks. libc.cpp has the rest
// (formatted output, stdio over the RAM disk, math, time).
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "hw.hpp"
// No libc headers here on purpose: their C++ overloads (memchr and friends)
// would clash with the plain C definitions below.
extern "C" size_t strlen(const char*);
extern "C" int printf(const char*, ...);

extern "C" {

// ---------------------------------------------------------------- memory / strings
void* memset(void* d, int c, size_t n) {
    void* r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void* memcpy(void* __restrict d, const void* __restrict s, size_t n) {
    void* r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
void* memmove(void* d, const void* s, size_t n) {
    uint8_t* dp = (uint8_t*)d; const uint8_t* sp = (const uint8_t*)s;
    if (dp < sp) while (n--) *dp++ = *sp++;
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (; n; n--, p++) if (*p == (uint8_t)c) return (void*)p;
    return nullptr;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b; }
    return 0;
}
char* strchr(const char* s, int c) {
    for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return nullptr; }
}
char* strrchr(const char* s, int c) {
    const char* r = nullptr;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; }
}
char* strstr(const char* h, const char* n) {
    size_t k = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, k)) return (char*)h;
    return k ? nullptr : (char*)h;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

// ---------------------------------------------------------------- errno / strtol
static int errno_value;
int* __errno_location(void) { return &errno_value; }

long strtol(const char* s, char** end, int base) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0;
    const char* start = p;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if ((*p | 32) >= 'a' && (*p | 32) <= 'z') d = (*p | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char*)(p == start ? s : p);
    return neg ? -(long)v : (long)v;
}
long __isoc23_strtol(const char* s, char** end, int base) { return strtol(s, end, base); }
int atoi(const char* s) { return (int)strtol(s, nullptr, 10); }

// ---------------------------------------------------------------- heap
// First-fit free list. It starts on a small static arena (enough for global
// constructors); kmain then hands it the machine's free RAM with heap_add(),
// so the kernel image itself stays small and boots in VMs with little memory.
struct Block { size_t size; Block* next; size_t free; size_t pad; };
static uint8_t boot_arena[64 << 10] __attribute__((aligned(16)));
static Block* heap_head;
static Block* heap_rover;
static size_t heap_total, heap_used, heap_peak;

static inline bool adjacent(Block* a, Block* b) { return (uint8_t*)(a + 1) + a->size == (uint8_t*)b; }

static void add_region(void* p, size_t n) {
    uintptr_t a = ((uintptr_t)p + 15) & ~(uintptr_t)15;
    uintptr_t e = ((uintptr_t)p + n) & ~(uintptr_t)15;
    if (e <= a || e - a < sizeof(Block) + 4096) return;
    Block* b = (Block*)a;
    b->size = e - a - sizeof(Block);
    b->free = 1;
    // Keep the list in address order so neighbouring blocks can merge.
    Block** pp = &heap_head;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    if (!heap_rover) heap_rover = b;
    heap_total += b->size;
}

void heap_add(void* p, size_t n) {
    if (!heap_head) add_region(boot_arena, sizeof(boot_arena));
    add_region(p, n);
}
size_t heap_free_bytes(void) { return heap_total - heap_used; }
size_t heap_peak_bytes(void) { return heap_peak; }

// The biggest block malloc could hand out now (free neighbours merge).
size_t heap_largest_free(void) {
    size_t best = 0;
    for (Block* b = heap_head; b; b = b->next) {
        if (!b->free) continue;
        size_t run = b->size;
        for (Block* c = b; c->next && c->next->free && adjacent(c, c->next); c = c->next)
            run += sizeof(Block) + c->next->size;
        if (run > best) best = run;
    }
    return best;
}

void* malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!n) n = 16;
    if (!heap_head) add_region(boot_arena, sizeof(boot_arena));
    for (int pass = 0; pass < 2; pass++) {
        for (Block* b = pass ? heap_head : heap_rover; b; b = b->next) {
            if (!b->free) continue;
            while (b->next && b->next->free && adjacent(b, b->next)) {   // coalesce lazily
                if (heap_rover == b->next) heap_rover = b;
                b->size += sizeof(Block) + b->next->size;
                heap_total += sizeof(Block);
                b->next = b->next->next;
            }
            if (b->size < n) continue;
            if (b->size >= n + sizeof(Block) + 64) {
                Block* rest = (Block*)((uint8_t*)(b + 1) + n);
                rest->size = b->size - n - sizeof(Block);
                rest->next = b->next;
                rest->free = 1;
                b->next = rest;
                b->size = n;
                heap_total -= sizeof(Block);
            }
            b->free = 0;
            heap_rover = b;
            heap_used += b->size;
            if (heap_used > heap_peak) heap_peak = heap_used;
            return b + 1;
        }
    }
    printf("malloc: out of memory (%lu bytes, %lu of %lu KB in use)\n",
           (unsigned long)n, (unsigned long)(heap_used >> 10), (unsigned long)(heap_total >> 10));
    return nullptr;
}
void free(void* p) {
    if (!p) return;
    Block* b = (Block*)p - 1;
    b->free = 1;
    heap_used -= b->size;
    while (b->next && b->next->free && adjacent(b, b->next)) {
        if (heap_rover == b->next) heap_rover = b;
        b->size += sizeof(Block) + b->next->size;
        heap_total += sizeof(Block);
        b->next = b->next->next;
    }
}
void* calloc(size_t a, size_t b) { void* p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    Block* b = (Block*)p - 1;
    if (b->size >= n) return p;
    void* q = malloc(n);
    if (q) { memcpy(q, p, b->size); free(p); }
    return q;
}

void abort(void) {
    serial_puts("abort()\n");
    for (;;) __asm__ volatile("cli; hlt");
}

// ---------------------------------------------------------------- C++ ABI
// Nothing throws (built with -fno-exceptions), so the exception entry points
// just stop the machine.
void* __dso_handle = nullptr;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void __cxa_pure_virtual() { serial_puts("pure virtual call\n"); abort(); }
void* __cxa_begin_catch(void*) { return nullptr; }
void __cxa_end_catch() {}
void __cxa_rethrow() { serial_puts("exception rethrown\n"); abort(); }
void _Unwind_Resume(void*) { serial_puts("_Unwind_Resume\n"); abort(); }
int __gxx_personality_v0() { return 0; }

} // extern "C"

// new can't return null (nothing checks), so running out there ends the
// program, with a message on screen. (Doom is C and doesn't use new.)
extern "C" void exit(int) noexcept;
static void* new_or_restart(size_t n) {
    void* p = malloc(n);
    if (!p) { printf("Out of memory\n"); exit(7); }
    return p;
}
void* operator new(size_t n) { return new_or_restart(n); }
void* operator new[](size_t n) { return new_or_restart(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

