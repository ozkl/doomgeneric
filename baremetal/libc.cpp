// The C library Doom expects, for bare metal: formatted output (with real
// floating-point formatting), sscanf, strtod, math on the x87, ctype,
// qsort/rand, time from the CMOS clock, and stdio/dirent/stat over the
// FAT32 RAM disk (storage.cpp). runtime.cpp has memory, strings and the heap.
//
// Compiled against the glibc headers for the types (FILE, DIR, struct stat,
// struct tm, jmp_buf), but none of glibc is linked: FILE and DIR here are
// our own structures, handed out as opaque pointers.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include "fat32.hpp"
#include "storage.hpp"
#include "hw.hpp"

extern "C" {
size_t strlen(const char*);
void* memcpy(void*, const void*, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strncpy(char*, const char*, size_t);
}

// Text written to stdout/stderr goes to the serial port and the boot log.
void console_write(const char* s, size_t n, bool is_stderr);   // kernel.cpp
int console_getchar();                            // kernel.cpp: there's no keyboard stdin (EOF)
void platform_sleep_ms(uint32_t ms);              // kernel.cpp
uint32_t platform_ms();                           // kernel.cpp: ms since boot
uint64_t platform_us();                           // kernel.cpp: microseconds since boot

extern "C" {

// ================================================================ ctype
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

int strcasecmp(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char* strcasestr(const char* h, const char* n) {
    size_t k = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, k)) return (char*)h;
    return k ? nullptr : (char*)h;
}
char* strncat(char* d, const char* s, size_t n) {
    char* r = d;
    while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = 0;
    return r;
}
char* strcat(char* d, const char* s) { char* r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
size_t strspn(const char* s, const char* a) {
    size_t n = 0;
    for (; s[n]; n++) { const char* p = a; while (*p && *p != s[n]) p++; if (!*p) break; }
    return n;
}
size_t strcspn(const char* s, const char* r) {
    size_t n = 0;
    for (; s[n]; n++) for (const char* p = r; *p; p++) if (*p == s[n]) return n;
    return n;
}
char* strpbrk(const char* s, const char* a) { s += strcspn(s, a); return *s ? (char*)s : nullptr; }
char* strdup(const char* s) { size_t n = strlen(s) + 1; char* d = (char*)malloc(n); if (d) memcpy(d, s, n); return d; }
size_t strnlen(const char* s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
static char* tok_save;
char* strtok(char* s, const char* d) {
    if (!s) s = tok_save;
    if (!s) return nullptr;
    s += strspn(s, d);
    if (!*s) { tok_save = nullptr; return nullptr; }
    char* e = s + strcspn(s, d);
    if (*e) { *e = 0; tok_save = e + 1; } else tok_save = nullptr;
    return s;
}
const char* strerror(int e) {
    switch (e) {
    case ENOENT: return "File not found";
    case EACCES: return "Permission denied";
    case ENOSPC: return "Disk full";
    case EEXIST: return "File already exists";
    case ENOTEMPTY: return "Directory not empty";
    case EIO: return "Disk error";
    case ENOMEM: return "Out of memory";
    case ENOTDIR: return "Path not found";
    default: return "Error";
    }
}
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

// ================================================================ x87 math
// Everything goes through the 387 (a 386 build needs one; the kernel checks).
static inline long double x87_fyl2x(long double y, long double x) { long double r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static inline long double x87_2pow(long double x) {           // 2^x
    long double i, f, r;
    __asm__("frndint" : "=t"(i) : "0"(x));
    f = x - i;                                                  // |f| <= 0.5
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1.0L;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}
double fabs(double x) { return x < 0 ? -x : x; }
void sincos(double x, double* s, double* c) {
    long double si, co;
    __asm__("fsincos" : "=t"(co), "=u"(si) : "0"((long double)x));
    *s = (double)si; *c = (double)co;
}
double sqrt(double x) { long double r; __asm__("fsqrt" : "=t"(r) : "0"((long double)x)); return (double)r; }
double sin(double x) { long double r; __asm__("fsin" : "=t"(r) : "0"((long double)x)); return (double)r; }
double cos(double x) { long double r; __asm__("fcos" : "=t"(r) : "0"((long double)x)); return (double)r; }
double tan(double x) {
    long double r, one;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"((long double)x));
    (void)one;
    return (double)r;
}
double atan2(double y, double x) {
    long double r;
    __asm__("fpatan" : "=t"(r) : "0"((long double)x), "u"((long double)y) : "st(1)");
    return (double)r;
}
double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { return atan2(sqrt((1.0 - x) * (1.0 + x)), x); }
double log(double x) {
    if (x < 0) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    return (double)x87_fyl2x(0.6931471805599453094L, x);       // ln 2 * log2 x
}
double log10(double x) {
    if (x <= 0) return x == 0 ? -__builtin_inf() : __builtin_nan("");
    return (double)x87_fyl2x(0.3010299956639811952L, x);
}
double log2(double x) { return x <= 0 ? log(x) : (double)x87_fyl2x(1.0L, x); }
double exp(double x) {
    if (x > 11356) return __builtin_inf();
    if (x < -11400) return 0;
    return (double)x87_2pow(x * 1.4426950408889634074L);
}
double floor(double x) {
    if (!(x == x) || fabs(x) >= 4503599627370496.0) return x;  // NaN or already integral
    double t = (double)(long long)x;
    return t > x ? t - 1 : t;
}
double ceil(double x) {
    if (!(x == x) || fabs(x) >= 4503599627370496.0) return x;
    double t = (double)(long long)x;
    return t < x ? t + 1 : t;
}
double trunc(double x) { return x < 0 ? ceil(x) : floor(x); }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
double fmod(double x, double y) {
    long double r = x;
    unsigned short sw;
    do {
        __asm__("fprem; fnstsw %%ax" : "=t"(r), "=a"(sw) : "0"(r), "u"((long double)y));
    } while (sw & 0x400);                                       // C2: reduction incomplete
    return (double)r;
}
double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : __builtin_inf();
    if (x < 0) {
        if (y != floor(y)) return __builtin_nan("");
        double r = (double)x87_2pow(x87_fyl2x(y, -(long double)x));
        return fmod(fabs(y), 2.0) == 1.0 ? -r : r;
    }
    return (double)x87_2pow(x87_fyl2x(y, x));
}
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }
double ldexp(double x, int e) { return (double)x87_2pow(e) * x; }
double modf(double x, double* ip) { double i = trunc(x); *ip = i; return x - i; }
float sqrtf(float x) { return (float)sqrt(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float fabsf(float x) { return x < 0 ? -x : x; }
float floorf(float x) { return (float)floor(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float atan2f(float y, float x) { return (float)atan2(y, x); }

// Exact-enough powers of ten for formatting and parsing (80-bit intermediates).
static long double pow10l_int(int e) {
    long double r = 1, b = 10;
    unsigned n = e < 0 ? -e : e;
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return e < 0 ? 1 / r : r;
}

// ================================================================ strtod
double strtod(const char* s, char** end) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (!strncasecmp(p, "inf", 3)) { if (end) *end = (char*)p + 3; return neg ? -__builtin_inf() : __builtin_inf(); }
    if (!strncasecmp(p, "nan", 3)) { if (end) *end = (char*)p + 3; return __builtin_nan(""); }
    long double m = 0;
    int digits = 0, scale = 0;
    bool any = false;
    for (; isdigit((unsigned char)*p); p++) {
        any = true;
        if (digits < 19) { m = m * 10 + (*p - '0'); if (m) digits++; } else scale++;
    }
    if (*p == '.') {
        p++;
        for (; isdigit((unsigned char)*p); p++) {
            any = true;
            if (digits < 19) { m = m * 10 + (*p - '0'); if (m) digits++; scale--; }
        }
    }
    if (!any) { if (end) *end = (char*)s; return 0; }
    if ((*p | 32) == 'e' || (*p | 32) == 'd') {               // 1D+10 too (Fortran/BASIC style)
        const char* q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            for (; isdigit((unsigned char)*q); q++) if (e < 10000) e = e * 10 + (*q - '0');
            scale += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char*)p;
    long double v = m;
    if (scale > 4950) v = __builtin_infl();
    else if (scale < -4950) v = 0;
    else if (scale) v = scale < 0 ? m / pow10l_int(-scale) : m * pow10l_int(scale);
    return (double)(neg ? -v : v);
}
double atof(const char* s) { return strtod(s, nullptr); }
float strtof(const char* s, char** e) { return (float)strtod(s, e); }
long double strtold(const char* s, char** e) { return strtod(s, e); }
long long strtoll(const char* s, char** e, int b) { return strtol(s, e, b); }
unsigned long strtoul(const char* s, char** e, int b) { return (unsigned long)strtol(s, e, b); }
unsigned long long strtoull(const char* s, char** e, int b) { return (unsigned long long)strtol(s, e, b); }
long atol(const char* s) { return strtol(s, nullptr, 10); }

// ================================================================ formatted output
struct Out {
    char* buf; size_t cap, len;
    void (*sink)(const char*, size_t, void*); void* ctx;      // streaming (files, console)
    char chunk[128]; size_t clen;
};
static void out_flush(Out& o) { if (o.sink && o.clen) { o.sink(o.chunk, o.clen, o.ctx); o.clen = 0; } }
static void out_c(Out& o, char c) {
    if (o.sink) { o.chunk[o.clen++] = c; if (o.clen == sizeof o.chunk) out_flush(o); }
    else if (o.len + 1 < o.cap) o.buf[o.len] = c;
    o.len++;
}
static void out_n(Out& o, char c, int n) { while (n-- > 0) out_c(o, c); }
static void out_s(Out& o, const char* s, size_t n) { while (n--) out_c(o, *s++); }

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16, F_UPPER = 32 };

// Pad and emit a converted field: sign/prefix, zero fill, body.
static void out_field(Out& o, const char* pre, const char* body, size_t blen, int width, int flags, int zeros) {
    size_t plen = strlen(pre);
    int total = (int)(plen + blen) + zeros;
    if (!(flags & F_LEFT) && !(flags & F_ZERO)) out_n(o, ' ', width - total);
    out_s(o, pre, plen);
    if (!(flags & F_LEFT) && (flags & F_ZERO)) out_n(o, '0', width - total);
    out_n(o, '0', zeros);
    out_s(o, body, blen);
    if (flags & F_LEFT) out_n(o, ' ', width - total);
}

static void out_int(Out& o, unsigned long long v, bool neg, int base, int width, int prec, int flags) {
    char tmp[32];
    int n = 0;
    const char* dg = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { tmp[n++] = dg[v % base]; v /= base; }
    char body[32];
    for (int i = 0; i < n; i++) body[i] = tmp[n - 1 - i];
    const char* pre = neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
    char pbuf[4];
    if ((flags & F_ALT) && base == 16 && n) { pbuf[0] = '0'; pbuf[1] = (flags & F_UPPER) ? 'X' : 'x'; pbuf[2] = 0; pre = pbuf; }
    int zeros = 0;
    if (prec >= 0) { flags &= ~F_ZERO; zeros = prec > n ? prec - n : 0; }
    else if (n == 0) zeros = 1;                                 // "0"
    if ((flags & F_ALT) && base == 8 && zeros == 0 && (n == 0 || body[0] != '0')) zeros = 1;
    out_field(o, pre, body, n, width, flags, zeros);
}

// Decimal digits of v > 0: `nd` significant digits, rounded; *e10 gets the
// exponent of the first one. Returns the count written (nd, at most 40).
// The decimal exponent of v > 0, and v scaled to [1, 10).
static int dec_exp(long double v, long double* mant) {
    int e = (int)floor((double)x87_fyl2x(0.3010299956639811952L, v));
    long double m = v / pow10l_int(e);
    if (m >= 10) { m /= 10; e++; }
    if (m < 1) { m *= 10; e--; }
    if (mant) *mant = m;
    return e;
}
static int float_digits(long double v, int nd, char* dig, int* e10) {
    if (nd < 1) nd = 1;
    if (nd > 40) nd = 40;
    long double m;
    int e = dec_exp(v, &m);
    int real = nd < 19 ? nd : 19;                               // beyond 19 digits: zeros
    for (int i = 0; i < real; i++) { int d = (int)m; if (d > 9) d = 9; dig[i] = (char)('0' + d); m = (m - d) * 10; }
    for (int i = real; i < nd; i++) dig[i] = '0';
    if (m >= 5 && real == nd) {                                 // round half up
        int i = nd - 1;
        while (i >= 0 && dig[i] == '9') dig[i--] = '0';
        if (i >= 0) dig[i]++;
        else { dig[0] = '1'; for (int k = 1; k < nd; k++) dig[k] = '0'; e++; }
    }
    *e10 = e;
    return nd;
}

static void out_float(Out& o, long double v, char conv, int width, int prec, int flags) {
    bool neg = __builtin_signbit((double)v);
    if (neg) v = -v;
    const char* pre = neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
    bool up = conv == 'F' || conv == 'E' || conv == 'G';
    if (v != v || v > 1.7976931348623157e308L) {
        const char* s = v != v ? (up ? "NAN" : "nan") : (up ? "INF" : "inf");
        out_field(o, pre, s, 3, width, flags & ~F_ZERO, 0);
        return;
    }
    if (prec < 0) prec = 6;
    char body[400];
    size_t bl = 0;
    char dig[48];
    char lc = (char)(conv | 32);
    bool strip = false;
    if (lc == 'g') {
        int P = prec == 0 ? 1 : prec;
        int X = 0;
        if (v != 0) float_digits(v, P, dig, &X);
        if (P > X && X >= -4) { lc = 'f'; prec = P - 1 - X; }
        else { lc = 'e'; prec = P - 1; }
        strip = !(flags & F_ALT);
    }
    if (lc == 'e') {
        int X = 0;
        if (v != 0) float_digits(v, prec + 1, dig, &X);
        else for (int i = 0; i <= prec && i < 48; i++) dig[i] = '0';
        int nd = prec + 1 > 40 ? 40 : prec + 1;
        body[bl++] = dig[0];
        if (prec > 0 || (flags & F_ALT)) body[bl++] = '.';
        for (int i = 1; i < nd; i++) body[bl++] = dig[i];
        for (int i = nd; i <= prec && bl < 300; i++) body[bl++] = '0';
        if (strip && prec > 0) {
            while (body[bl - 1] == '0') bl--;
            if (body[bl - 1] == '.') bl--;
        }
        body[bl++] = up ? 'E' : 'e';
        body[bl++] = X < 0 ? '-' : '+';
        int ax = X < 0 ? -X : X;
        if (ax >= 100) body[bl++] = (char)('0' + ax / 100);
        body[bl++] = (char)('0' + ax / 10 % 10);
        body[bl++] = (char)('0' + ax % 10);
    } else {                                                    // 'f'
        if (prec > 300) prec = 300;
        int X = 0, nd = 0;
        if (v != 0) {
            // digits down to 10^-prec: the leading exponent decides how many
            int e0 = dec_exp(v, nullptr);
            nd = e0 + 1 + prec;
            if (nd > 0) float_digits(v, nd, dig, &X);          // a rounding carry raises X; the rest are zeros
            else if (nd == 0 && v * pow10l_int(prec) >= 0.5L) { dig[0] = '1'; X = -prec; nd = 1; }
            else nd = 0;
        }
        // integer part
        if (nd == 0 || X < 0) body[bl++] = '0';
        else for (int i = 0; i <= X; i++) body[bl++] = i < nd && i < 40 ? dig[i] : '0';
        if (prec > 0 || (flags & F_ALT)) body[bl++] = '.';
        for (int k = 1; k <= prec; k++) {                       // digit for 10^-k
            int i = X + k;                                      // its index in dig
            body[bl++] = (nd && i >= 0 && i < nd && i < 40) ? dig[i] : '0';
        }
        if (strip && prec > 0) {
            while (body[bl - 1] == '0') bl--;
            if (body[bl - 1] == '.') bl--;
        }
    }
    out_field(o, pre, body, bl, width, flags, 0);
}

static void format(Out& o, const char* f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { out_c(o, *f); continue; }
        f++;
        int flags = 0;
        for (;; f++) {
            if (*f == '-') flags |= F_LEFT;
            else if (*f == '+') flags |= F_PLUS;
            else if (*f == ' ') flags |= F_SPACE;
            else if (*f == '#') flags |= F_ALT;
            else if (*f == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { flags |= F_LEFT; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        if (flags & F_LEFT) flags &= ~F_ZERO;
        int lng = 0;                                            // 1 l, 2 ll, 3 z/j/t, -1 h, -2 hh, 4 L
        for (;; f++) {
            if (*f == 'l') lng = lng == 1 ? 2 : 1;
            else if (*f == 'h') lng = lng == -1 ? -2 : -1;
            else if (*f == 'z' || *f == 'j' || *f == 't') lng = 3;
            else if (*f == 'L' || *f == 'q') lng = 4;
            else break;
        }
        char c = *f;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long) : lng == 1 || lng == 3 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            out_int(o, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v, v < 0, 10, width, prec, flags);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng == 2 ? va_arg(ap, unsigned long long)
                                 : lng == 1 || lng == 3 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
            if (c == 'X') flags |= F_UPPER;
            out_int(o, v, false, c == 'u' ? 10 : c == 'o' ? 8 : 16, width, prec, flags & ~(F_PLUS | F_SPACE));
            break;
        }
        case 'p': out_int(o, (uintptr_t)va_arg(ap, void*), false, 16, width, -1, F_ALT); break;
        case 'c': { char ch = (char)va_arg(ap, int); out_field(o, "", &ch, 1, width, flags & ~F_ZERO, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t n = prec >= 0 ? strnlen(s, prec) : strlen(s);
            out_field(o, "", s, n, width, flags & ~F_ZERO, 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            long double v = lng == 4 ? va_arg(ap, long double) : va_arg(ap, double);
            out_float(o, v, c, width, prec, flags);
            break;
        }
        case 'n': { int* p = va_arg(ap, int*); if (p) *p = (int)o.len; break; }
        case '%': out_c(o, '%'); break;
        default: out_c(o, '%'); out_c(o, c); break;
        }
    }
    out_flush(o);
}

int vsnprintf(char* buf, size_t cap, const char* f, va_list ap) {
    Out o{};
    o.buf = buf; o.cap = cap;
    format(o, f, ap);
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}
int snprintf(char* buf, size_t cap, const char* f, ...) {
    va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n;
}
int vsprintf(char* buf, const char* f, va_list ap) { return vsnprintf(buf, (size_t)-1 >> 1, f, ap); }
int sprintf(char* buf, const char* f, ...) {
    va_list ap; va_start(ap, f); int n = vsprintf(buf, f, ap); va_end(ap); return n;
}
extern "C" void klog_add(const char* s, size_t n);   // kernel.cpp: kept for DMESG
static void serial_sink(const char* s, size_t n, void*) { klog_add(s, n); while (n--) serial_putc(*s++); }
// printf is the kernel's log: the serial port and the boot log.
int vprintf(const char* f, va_list ap) { Out o{}; o.sink = serial_sink; format(o, f, ap); return (int)o.len; }
int printf(const char* f, ...) { va_list ap; va_start(ap, f); int n = vprintf(f, ap); va_end(ap); return n; }
int puts(const char* s) { klog_add(s, strlen(s)); klog_add("\n", 1); serial_puts(s); serial_putc('\n'); return 0; }

// ================================================================ sscanf
// Enough for Doom's config parsing: whitespace, literals, %d %i %u %x %o %s
// %c %f/%lf (with widths and %*), %n and %%.
int vsscanf(const char* str, const char* f, va_list ap) {
    const char* s = str;
    int assigned = 0;
    for (; *f; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; continue; }
        f++;
        if (*f == '%') { while (isspace((unsigned char)*s)) s++; if (*s != '%') break; s++; continue; }
        bool skip = false;
        if (*f == '*') { skip = true; f++; }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int lng = 0;                                            // 1 l, 2 ll, -1 h, -2 hh
        for (;; f++) {
            if (*f == 'l') lng = lng == 1 ? 2 : 1;
            else if (*f == 'h') lng = lng == -1 ? -2 : -1;
            else if (*f == 'z' || *f == 'j' || *f == 't') lng = 1;
            else break;
        }
        char c = *f;
        if (!c) break;
        if (c == 'n') { if (!skip) *va_arg(ap, int*) = (int)(s - str); continue; }
        if (c != 'c') while (isspace((unsigned char)*s)) s++;
        if (!*s) return assigned ? assigned : EOF;
        char tmp[128];
        int lim = width > 0 && width < (int)sizeof tmp ? width : (int)sizeof tmp - 1;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') {
            int n = 0;
            while (s[n] && n < lim && !isspace((unsigned char)s[n])) { tmp[n] = s[n]; n++; }
            tmp[n] = 0;
            int base = c == 'i' ? 0 : (c == 'x' || c == 'X') ? 16 : c == 'o' ? 8 : 10;
            char* end;
            long v = strtol(tmp, &end, base);
            if (end == tmp) break;
            s += end - tmp;
            if (!skip) {
                if (lng == 2) *va_arg(ap, long long*) = v;
                else if (lng == 1) *va_arg(ap, long*) = v;
                else if (lng == -1) *va_arg(ap, short*) = (short)v;
                else if (lng == -2) *va_arg(ap, char*) = (char)v;
                else *va_arg(ap, int*) = (int)v;
                assigned++;
            }
        } else if (c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'a') {
            int n = 0;
            while (s[n] && n < lim && !isspace((unsigned char)s[n])) { tmp[n] = s[n]; n++; }
            tmp[n] = 0;
            char* end;
            double v = strtod(tmp, &end);
            if (end == tmp) break;
            s += end - tmp;
            if (!skip) {
                if (lng >= 1) *va_arg(ap, double*) = v;
                else *va_arg(ap, float*) = (float)v;
                assigned++;
            }
        } else if (c == 's') {
            char* out = skip ? nullptr : va_arg(ap, char*);
            int n = 0, max = width > 0 ? width : 0x7FFFFFFF;
            while (*s && !isspace((unsigned char)*s) && n < max) { if (out) out[n] = *s; s++; n++; }
            if (out) { out[n] = 0; assigned++; }
        } else if (c == 'c') {
            char* out = skip ? nullptr : va_arg(ap, char*);
            int n = width > 0 ? width : 1;
            for (int i = 0; i < n; i++) { if (!*s) return assigned ? assigned : EOF; if (out) out[i] = *s; s++; }
            if (out) assigned++;
        } else {
            break;
        }
    }
    return assigned;
}
// glibc's headers rename sscanf (__isoc99_sscanf, __isoc23_sscanf) depending
// on its version and the language standard; define every spelling, under
// names of our own so none clashes with what the header declared.
#define SSCANF_AS(fn, sym) \
    int fn(const char* s, const char* f, ...) __asm__(sym); \
    int fn(const char* s, const char* f, ...) { va_list ap; va_start(ap, f); int n = vsscanf(s, f, ap); va_end(ap); return n; }
SSCANF_AS(bm_sscanf, "sscanf")
SSCANF_AS(bm_sscanf99, "__isoc99_sscanf")
SSCANF_AS(bm_sscanf23, "__isoc23_sscanf")
int system(const char*) { return -1; }

// ================================================================ stdio
// A FILE is read whole into memory when opened for input, and written whole
// when closed (or flushed) for output: that's what fat32.cpp does safely,
// and on a RAM disk it's quick.
struct BFile {
    int kind;                 // 0 file, 1 console (stdout/stderr), 2 keyboard (stdin)
    char* data; size_t len, cap, pos;
    bool writing, dirty, eof, err;
    bool mapped;              // data points into the RAM disk (read-only): not ours to free
    int unget;
    char path[256];
};
static BFile con_in{2}, con_out{1}, con_err{1};
FILE* stdin = (FILE*)&con_in;
FILE* stdout = (FILE*)&con_out;
FILE* stderr = (FILE*)&con_err;
static inline BFile* B(FILE* f) { return (BFile*)f; }

static void set_errno(int e) { errno = e; }

FILE* fopen(const char* path, const char* mode) {
    char full[256];
    if (!storage_resolve(path, full, sizeof full)) { set_errno(ENOENT); return nullptr; }
    if (!storage_ready()) { set_errno(EIO); return nullptr; }
    bool rd = mode[0] == 'r', ap = mode[0] == 'a', plus = false;
    for (const char* m = mode; *m; m++) if (*m == '+') plus = true;
    bool is_dir = false;
    uint32_t size = 0;
    bool exists = fat_exists(full, &is_dir, &size);
    if (is_dir) { set_errno(EISDIR); return nullptr; }
    if (rd && !exists) { set_errno(ENOENT); return nullptr; }
    BFile* b = (BFile*)calloc(1, sizeof(BFile));
    if (!b) { set_errno(ENOMEM); return nullptr; }
    strncpy(b->path, full, sizeof b->path - 1);
    b->unget = -1;
    size_t msize = 0;
    const void* m = rd && !plus && exists ? storage_map(full, &msize) : nullptr;
    if (m) {                                                    // a WAD: read where it lies, no copy
        b->data = (char*)m;
        b->len = b->cap = msize;
        b->mapped = true;
    } else if ((rd || ap) && exists) {
        b->cap = size + 1;
        b->data = (char*)malloc(b->cap);
        size_t got = 0;
        if (!b->data || (size && (!fat_read(full, b->data, size, &got) || got != size))) {
            free(b->data); free(b); set_errno(EIO); return nullptr;
        }
        b->len = size;
    }
    b->writing = !rd || plus;
    if (ap) b->pos = b->len;
    if (mode[0] == 'w') { b->dirty = true; }                    // creates/truncates at close
    return (FILE*)b;
}

static bool file_reserve(BFile* b, size_t need) {
    if (need <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) nc *= 2;
    char* d = (char*)realloc(b->data, nc);
    if (!d) return false;
    b->data = d; b->cap = nc;
    return true;
}
static void file_write(BFile* b, const char* s, size_t n) {
    if (b->kind == 1) { console_write(s, n, b == &con_err); return; }
    if (b->kind == 2 || !b->writing) { b->err = true; return; }
    if (!file_reserve(b, b->pos + n)) { b->err = true; return; }
    memcpy(b->data + b->pos, s, n);
    b->pos += n;
    if (b->pos > b->len) b->len = b->pos;
    b->dirty = true;
}
static void sink_file(const char* s, size_t n, void* ctx) { file_write((BFile*)ctx, s, n); }

int fflush(FILE* f) {
    if (!f) return 0;
    BFile* b = B(f);
    if (b->kind || !b->dirty) return 0;
    if (!storage_ready() || !fat_write(b->path, b->data, b->len)) {
        b->err = true;
        set_errno(ENOSPC);
        return EOF;
    }
    b->dirty = false;
    return 0;
}
int fclose(FILE* f) {
    if (!f) return EOF;
    BFile* b = B(f);
    if (b->kind) return 0;
    int r = fflush(f);
    if (r) {                                                    // nobody checks fclose: say it here
        const char* e = strerror(errno);
        fputs(b->path, stderr); fputs(": ", stderr); fputs(e, stderr); fputs("\n", stderr);
    }
    if (!b->mapped) free(b->data);
    free(b);
    return r;
}
int fgetc(FILE* f) {
    BFile* b = B(f);
    if (b->unget >= 0) { int c = b->unget; b->unget = -1; return c; }
    if (b->kind == 2) return console_getchar();
    if (b->kind == 1 || b->pos >= b->len) { b->eof = true; return EOF; }
    return (unsigned char)b->data[b->pos++];
}
int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE* f) { if (c == EOF) return EOF; B(f)->unget = c; B(f)->eof = false; return c; }
char* fgets(char* s, int n, FILE* f) {
    BFile* b = B(f);
    if (n <= 0) return nullptr;
    if (b->kind == 2) {                                         // a line from the keyboard
        int i = 0;
        while (i < n - 1) {
            int c = console_getchar();
            if (c == '\r') c = '\n';
            s[i++] = (char)c;
            if (c == '\n') break;
        }
        s[i] = 0;
        return s;
    }
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return nullptr;
    s[i] = 0;
    return s;
}
int fputc(int c, FILE* f) { char ch = (char)c; file_write(B(f), &ch, 1); return B(f)->err ? EOF : (unsigned char)c; }
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char* s, FILE* f) { file_write(B(f), s, strlen(s)); return B(f)->err ? EOF : 0; }
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f) { file_write(B(f), (const char*)p, sz * n); return B(f)->err ? 0 : n; }
size_t fread(void* p, size_t sz, size_t n, FILE* f) {
    BFile* b = B(f);
    if (!sz) return 0;
    if (b->kind) return 0;
    size_t want = sz * n, have = b->len - b->pos;
    if (want > have) { want = have - have % sz; b->eof = true; }
    memcpy(p, b->data + b->pos, want);
    b->pos += want;
    return want / sz;
}
int fseek(FILE* f, long off, int whence) {
    BFile* b = B(f);
    if (b->kind) return -1;
    long base = whence == SEEK_CUR ? (long)b->pos : whence == SEEK_END ? (long)b->len : 0;
    long p = base + off;
    if (p < 0) return -1;
    if ((size_t)p > b->len && b->writing) { if (!file_reserve(b, p)) return -1; memset(b->data + b->len, 0, p - b->len); b->len = p; }
    b->pos = (size_t)p > b->len ? b->len : (size_t)p;
    b->eof = false; b->unget = -1;
    return 0;
}
long ftell(FILE* f) { return B(f)->kind ? -1 : (long)B(f)->pos; }
void rewind(FILE* f) { fseek(f, 0, SEEK_SET); B(f)->err = false; }
int feof(FILE* f) { BFile* b = B(f); return b->kind == 0 && (b->eof || (b->pos >= b->len && b->unget < 0)); }
int ferror(FILE* f) { return B(f)->err; }
void clearerr(FILE* f) { B(f)->err = B(f)->eof = false; }
int fileno(FILE* f) { return B(f)->kind == 2 ? 0 : B(f)->kind == 1 ? (f == stderr ? 2 : 1) : 3; }
int setvbuf(FILE*, char*, int, size_t) { return 0; }
void setbuf(FILE*, char*) {}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    Out o{};
    o.sink = sink_file; o.ctx = B(f);
    format(o, fmt, ap);
    return (int)o.len;
}
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }
void perror(const char* s) {
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputs("\n", stderr);
}

int remove(const char* path) {
    char full[256];
    if (!storage_resolve(path, full, sizeof full) || !storage_ready()) { set_errno(ENOENT); return -1; }
    bool dir;
    if (!fat_exists(full, &dir)) { set_errno(ENOENT); return -1; }
    if (!fat_remove(full)) { set_errno(dir ? ENOTEMPTY : EIO); return -1; }
    return 0;
}
int unlink(const char* path) { return remove(path); }
int rmdir(const char* path) { return remove(path); }
int rename(const char* from, const char* to) {
    char a[256], b[256];
    if (!storage_resolve(from, a, sizeof a) || !storage_resolve(to, b, sizeof b) || !storage_ready()) { set_errno(ENOENT); return -1; }
    if (!fat_exists(a)) { set_errno(ENOENT); return -1; }
    if (!fat_rename(a, b)) { set_errno(EEXIST); return -1; }
    return 0;
}
int mkdir(const char* path, mode_t) {
    char full[256];
    if (!storage_resolve(path, full, sizeof full) || !storage_ready()) { set_errno(ENOENT); return -1; }
    if (fat_exists(full)) { set_errno(EEXIST); return -1; }
    if (!fat_mkdir(full)) { set_errno(ENOSPC); return -1; }
    return 0;
}
int stat(const char* path, struct stat* st) {
    char full[256];
    bool dir;
    uint32_t size;
    if (!storage_resolve(path, full, sizeof full) || !storage_ready() || !fat_exists(full, &dir, &size)) { set_errno(ENOENT); return -1; }
    memset(st, 0, sizeof *st);
    st->st_mode = dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = size;
    return 0;
}
int access(const char* path, int) { struct stat st; return stat(path, &st); }

char* getcwd(char* buf, size_t n) {
    const char* c = storage_cwd();
    if (strlen(c) + 1 > n) { set_errno(ERANGE); return nullptr; }
    memcpy(buf, c, strlen(c) + 1);
    return buf;
}
int chdir(const char* path) {
    char full[256];
    bool dir = false;
    if (!storage_resolve(path, full, sizeof full) || !storage_ready()) { set_errno(ENOENT); return -1; }
    if (!(full[0] == '/' && full[1] == 0) && (!fat_exists(full, &dir) || !dir)) { set_errno(ENOENT); return -1; }
    storage_set_cwd(full);
    return 0;
}

// ---- dirent
struct BDir { struct dirent* ents; int n, cap, pos; };
static bool collect(const char* name, uint32_t, bool is_dir, void* ctx) {
    BDir* d = (BDir*)ctx;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 32;
        struct dirent* e = (struct dirent*)realloc(d->ents, nc * sizeof(struct dirent));
        if (!e) return false;
        d->ents = e; d->cap = nc;
    }
    struct dirent* e = &d->ents[d->n++];
    memset(e, 0, sizeof *e);
    strncpy(e->d_name, name, sizeof e->d_name - 1);
    e->d_type = is_dir ? DT_DIR : DT_REG;
    return true;
}
DIR* opendir(const char* path) {
    char full[256];
    if (!storage_resolve(path, full, sizeof full) || !storage_ready()) { set_errno(ENOENT); return nullptr; }
    BDir* d = (BDir*)calloc(1, sizeof(BDir));
    if (!d) return nullptr;
    if (!fat_list(full, collect, d)) { free(d->ents); free(d); set_errno(ENOENT); return nullptr; }
    return (DIR*)d;
}
struct dirent* readdir(DIR* dp) {
    BDir* d = (BDir*)dp;
    return d->pos < d->n ? &d->ents[d->pos++] : nullptr;
}
int closedir(DIR* dp) { BDir* d = (BDir*)dp; free(d->ents); free(d); return 0; }

// ================================================================ misc
static uint64_t rng = 1;
void srand(unsigned s) { rng = s ? s : 1; }
int rand(void) {                                                // xorshift64*, 31-bit results
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (int)((rng * 0x2545F4914F6CDD1Dull) >> 33);
}

// A stable merge sort, as glibc's qsort is in practice: load() sorts the
// program by line number and relies on statements that share one keeping
// their order.
static void merge_sort(char* a, char* tmp, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    if (n < 2) return;
    if (n <= 8) {                                               // insertion sort (stable)
        for (size_t i = 1; i < n; i++) {
            memcpy(tmp, a + i * sz, sz);
            size_t j = i;
            while (j > 0 && cmp(a + (j - 1) * sz, tmp) > 0) { memcpy(a + j * sz, a + (j - 1) * sz, sz); j--; }
            memcpy(a + j * sz, tmp, sz);
        }
        return;
    }
    size_t h = n / 2;
    merge_sort(a, tmp, h, sz, cmp);
    merge_sort(a + h * sz, tmp, n - h, sz, cmp);
    if (cmp(a + (h - 1) * sz, a + h * sz) <= 0) return;         // already in order
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(a + j * sz, a + i * sz) < 0) memcpy(tmp + k++ * sz, a + j++ * sz, sz);
        else memcpy(tmp + k++ * sz, a + i++ * sz, sz);
    }
    while (i < h) memcpy(tmp + k++ * sz, a + i++ * sz, sz);
    while (j < n) memcpy(tmp + k++ * sz, a + j++ * sz, sz);
    memcpy(a, tmp, n * sz);
}
void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    if (n < 2 || !sz) return;
    char* tmp = (char*)malloc(n * sz);
    if (!tmp) {                                                 // no memory: insertion sort in place
        char t[512];
        if (sz > sizeof t) return;
        char* a = (char*)base;
        for (size_t i = 1; i < n; i++) {
            memcpy(t, a + i * sz, sz);
            size_t j = i;
            while (j > 0 && cmp(a + (j - 1) * sz, t) > 0) { memcpy(a + j * sz, a + (j - 1) * sz, sz); j--; }
            memcpy(a + j * sz, t, sz);
        }
        return;
    }
    merge_sort((char*)base, tmp, n, sz, cmp);
    free(tmp);
}
void* bsearch(const void* key, const void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp(key, (const char*)base + mid * sz);
        if (!c) return (char*)base + mid * sz;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

char* getenv(const char*) { return nullptr; }
int sigaction(int, const struct sigaction*, struct sigaction*) { return 0; }
int sigemptyset(sigset_t* s) { memset(s, 0, sizeof *s); return 0; }
__sighandler_t signal(int, __sighandler_t) { return nullptr; }
int raise(int) { return 0; }

// ================================================================ time
// The CMOS clock keeps local time (whatever zone the machine was set to);
// time() reads it once and counts on from the PIT, so it's cheap to call.
static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }
static long days_from_civil(long y, long m, long d) {           // H. Hinnant
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static long cmos_seconds() {
    uint8_t s, m, h, d, mo, y, b, c = 0;
    do {
        while (cmos(0x0A) & 0x80) {}
        s = cmos(0); m = cmos(2); h = cmos(4); d = cmos(7); mo = cmos(8); y = cmos(9); c = cmos(0x32);
    } while (s != cmos(0));
    b = cmos(0x0B);
    auto bin = [&](uint8_t v) { return (b & 4) ? v : (uint8_t)((v & 15) + (v >> 4) * 10); };
    bool pm = !(b & 2) && (h & 0x80);
    h = bin(h & 0x7F);
    if (!(b & 2)) h = (uint8_t)(h % 12 + (pm ? 12 : 0));
    int century = bin(c);
    long Y = (century >= 19 && century <= 21 ? century * 100 : 2000) + bin(y);
    if (century < 19 && bin(y) >= 80) Y -= 100;                 // no century register: 1980..2079
    return days_from_civil(Y, bin(mo), bin(d)) * 86400 + h * 3600L + bin(m) * 60L + bin(s);
}
static long boot_seconds = -1;
static uint32_t boot_ms;
static uint64_t boot_us;
static void time_sync() { if (boot_seconds < 0) { boot_seconds = cmos_seconds(); boot_ms = platform_ms(); boot_us = platform_us(); } }
time_t time(time_t* t) {
    time_sync();
    time_t now = boot_seconds + (platform_ms() - boot_ms) / 1000;
    if (t) *t = now;
    return now;
}
int clock_gettime(clockid_t clk, struct timespec* ts) {
    time_sync();
    uint64_t us = platform_us() - (clk == CLOCK_REALTIME ? boot_us : 0);
    ts->tv_sec = (clk == CLOCK_REALTIME ? boot_seconds : 0) + (time_t)(us / 1000000u);
    ts->tv_nsec = (long)(us % 1000000u) * 1000L;
    return 0;
}
int gettimeofday(struct timeval* tv, void*) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}
clock_t clock(void) { return (clock_t)platform_ms() * (CLOCKS_PER_SEC / 1000); }
int nanosleep(const struct timespec* req, struct timespec* rem) {
    uint64_t ms = (uint64_t)req->tv_sec * 1000 + req->tv_nsec / 1000000;
    platform_sleep_ms(ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}
unsigned sleep(unsigned s) { platform_sleep_ms(s * 1000); return 0; }
int usleep(unsigned us) { platform_sleep_ms(us / 1000); return 0; }

static struct tm tm_buf;
struct tm* gmtime_r(const time_t* t, struct tm* r) {
    long long s = *t, days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem / 60 % 60); r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)((days + 4) % 7); if (r->tm_wday < 0) r->tm_wday += 7;
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    r->tm_year = (int)(y - 1900); r->tm_mon = (int)(m - 1); r->tm_mday = (int)d;
    r->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    r->tm_isdst = 0;
    return r;
}
struct tm* gmtime(const time_t* t) { return gmtime_r(t, &tm_buf); }
struct tm* localtime_r(const time_t* t, struct tm* r) { return gmtime_r(t, r); }   // CMOS time is local
struct tm* localtime(const time_t* t) { return gmtime_r(t, &tm_buf); }
time_t mktime(struct tm* t) {
    return days_from_civil(t->tm_year + 1900L, t->tm_mon + 1L, t->tm_mday) * 86400 + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec;
}
size_t strftime(char* s, size_t max, const char* f, const struct tm* t) {
    static const char* const wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* const mo[] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};
    size_t n = 0;
    auto put = [&](const char* p) { while (*p && n + 1 < max) s[n++] = *p++; };
    char b[32];
    for (; *f && n + 1 < max; f++) {
        if (*f != '%') { s[n++] = *f; continue; }
        switch (*++f) {
        case 'H': snprintf(b, sizeof b, "%02d", t->tm_hour); put(b); break;
        case 'I': snprintf(b, sizeof b, "%02d", t->tm_hour % 12 ? t->tm_hour % 12 : 12); put(b); break;
        case 'M': snprintf(b, sizeof b, "%02d", t->tm_min); put(b); break;
        case 'S': snprintf(b, sizeof b, "%02d", t->tm_sec); put(b); break;
        case 'p': put(t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'd': snprintf(b, sizeof b, "%02d", t->tm_mday); put(b); break;
        case 'e': snprintf(b, sizeof b, "%2d", t->tm_mday); put(b); break;
        case 'm': snprintf(b, sizeof b, "%02d", t->tm_mon + 1); put(b); break;
        case 'Y': snprintf(b, sizeof b, "%d", t->tm_year + 1900); put(b); break;
        case 'y': snprintf(b, sizeof b, "%02d", (t->tm_year + 1900) % 100); put(b); break;
        case 'j': snprintf(b, sizeof b, "%03d", t->tm_yday + 1); put(b); break;
        case 'A': put(wd[t->tm_wday % 7]); break;
        case 'a': snprintf(b, 4, "%s", wd[t->tm_wday % 7]); put(b); break;
        case 'B': put(mo[t->tm_mon % 12]); break;
        case 'b': case 'h': snprintf(b, 4, "%s", mo[t->tm_mon % 12]); put(b); break;
        case 'D': snprintf(b, sizeof b, "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday, (t->tm_year + 1900) % 100); put(b); break;
        case 'T': snprintf(b, sizeof b, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec); put(b); break;
        case 'F': snprintf(b, sizeof b, "%d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday); put(b); break;
        case '%': put("%"); break;
        case 0: f--; break;
        default: break;
        }
    }
    s[n] = 0;
    return n;
}

} // extern "C"
