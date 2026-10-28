// FAT32 filesystem driver (converted from the FAT12 one). See fat32.hpp.
//
// Safety on power loss or a pulled disk: file data always goes to newly
// allocated clusters first, then the FAT, then the directory entry, and only
// then are the old clusters freed. An interrupted write leaves either the old
// file or the new one (at worst plus some lost clusters that fsck reclaims),
// never a cross-linked or half-written file.
#include "fat32.hpp"
#include <string.h>
#include <stdlib.h>

namespace {

constexpr uint32_t SEC = 512;
constexpr uint32_t NONE = 0xFFFFFFFFu;
constexpr size_t NAME_MAX_LEN = 255;
enum : uint8_t { A_RO = 0x01, A_VOL = 0x08, A_DIR = 0x10, A_ARC = 0x20, A_LFN = 0x0F };
enum : uint8_t { NT_LOWER_BASE = 0x08, NT_LOWER_EXT = 0x10 };

struct __attribute__((packed)) Dirent {
    uint8_t name[11];
    uint8_t attr, ntres, ctime_tenth;
    uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus;
    uint32_t size;
};
struct __attribute__((packed)) LfnEnt {
    uint8_t ord;
    uint16_t n1[5];
    uint8_t attr, type, sum;
    uint16_t n2[6];
    uint16_t clus;
    uint16_t n3[2];
};
static_assert(sizeof(Dirent) == 32 && sizeof(LfnEnt) == 32, "directory entries are 32 bytes");

// A malloc'd buffer that frees itself.
struct Mem {
    uint8_t* p = nullptr;
    explicit Mem(size_t n) : p((uint8_t*)malloc(n ? n : 1)) {}
    ~Mem() { free(p); }
    Mem(const Mem&) = delete;
    Mem& operator=(const Mem&) = delete;
};
// A cluster chain.
struct Chain {
    uint32_t* c = nullptr;
    uint32_t n = 0, cap = 0;
    ~Chain() { free(c); }
    bool push(uint32_t x) {
        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 16;
            uint32_t* q = (uint32_t*)malloc(nc * sizeof(uint32_t));
            if (!q) return false;
            if (c) memcpy(q, c, n * sizeof(uint32_t));
            free(c);
            c = q; cap = nc;
        }
        c[n++] = x;
        return true;
    }
    void clear() { n = 0; }
};

// ---------------------------------------------------------------- volume state
struct Volume {
    FatDisk disk{};
    bool mounted = false;
    uint32_t spc = 0, csize = 0;           // sectors / bytes per cluster
    uint32_t fat_lba = 0, fat_secs = 0, nfats = 0;
    uint32_t root_clus = 0;                // the root directory is a cluster chain
    uint32_t active_fat = 0;               // the FAT copy read at mount (BPB ext flags)
    uint32_t data_lba = 0, nclus = 0;      // valid clusters are 2 .. nclus+1
    uint8_t* fat = nullptr;                // first FAT copy, in memory
    uint8_t* fat_dirty = nullptr;          // per FAT sector
    uint32_t hint = 2;                     // where to start looking for free clusters
} v;

// One-sector write-back cache for directory sectors.
uint8_t cache[SEC];
uint32_t cache_lba = NONE;
bool cache_dirty = false;

bool cache_flush() {
    if (!cache_dirty) return true;
    if (!v.disk.write(cache_lba, 1, cache)) return false;
    cache_dirty = false;
    return true;
}
uint8_t* sec_get(uint32_t lba) {
    if (lba == cache_lba) return cache;
    if (!cache_flush()) return nullptr;
    cache_lba = NONE;
    if (!v.disk.read(lba, 1, cache)) return nullptr;
    cache_lba = lba;
    return cache;
}
// Bulk transfers bypass the cache; keep it coherent.
bool raw_write(uint32_t lba, uint32_t n, const void* p) {
    if (cache_lba != NONE && cache_lba >= lba && cache_lba < lba + n) {
        if (cache_dirty && !cache_flush()) return false;
        cache_lba = NONE;
    }
    return v.disk.write(lba, n, p);
}
bool raw_read(uint32_t lba, uint32_t n, void* p) {
    if (cache_dirty && cache_lba >= lba && cache_lba < lba + n && !cache_flush()) return false;
    return v.disk.read(lba, n, p);
}
void idle() { if (v.disk.idle) v.disk.idle(); }

// ---------------------------------------------------------------- FAT
bool valid(uint32_t c) { return c >= 2 && c <= v.nclus + 1; }
constexpr uint32_t EOC = 0x0FFFFFFF;
bool is_eoc(uint32_t x) { return x >= 0x0FFFFFF8; }
uint32_t clus_lba(uint32_t c) { return v.data_lba + (c - 2) * v.spc; }
// Directories are named by their first cluster, with 0 for the root (as
// FAT32 itself writes it in "..").
uint32_t dstart(uint32_t d) { return d ? d : v.root_clus; }
uint32_t norm_dir(uint32_t c) { return c == v.root_clus ? 0 : c; }

// FAT32 entries are 28 bits; the top 4 are reserved and kept as found.
uint32_t fat_get(uint32_t c) {
    uint32_t x;
    memcpy(&x, v.fat + c * 4, 4);
    return x & 0x0FFFFFFF;
}
void fat_set(uint32_t c, uint32_t x) {
    uint32_t old;
    memcpy(&old, v.fat + c * 4, 4);
    x = (old & 0xF0000000u) | (x & 0x0FFFFFFF);
    memcpy(v.fat + c * 4, &x, 4);
    v.fat_dirty[c * 4 / SEC] = 1;
}
// A directory entry's first cluster (high and low halves).
uint32_t ecl(const struct Dirent& e);
void set_ecl(struct Dirent& e, uint32_t c);
// Write the changed FAT sectors to every copy.
bool fat_flush() {
    for (uint32_t s = 0; s < v.fat_secs;) {
        if (!v.fat_dirty[s]) { s++; continue; }
        uint32_t e = s;
        while (e < v.fat_secs && v.fat_dirty[e]) e++;
        for (uint32_t k = 0; k < v.nfats; k++)
            if (!raw_write(v.fat_lba + k * v.fat_secs + s, e - s, &v.fat[s * SEC])) return false;
        for (uint32_t i = s; i < e; i++) v.fat_dirty[i] = 0;
        s = e;
    }
    return true;
}
void free_chain(uint32_t c) {
    for (uint32_t n = 0; valid(c) && n <= v.nclus; n++) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if (c < v.hint) v.hint = c;
        c = next;
    }
}
bool chain(uint32_t c, Chain& out) {
    out.clear();
    while (valid(c)) {
        if (out.n > v.nclus || !out.push(c)) return false;   // loop in the FAT, or no memory
        c = fat_get(c);
    }
    return c == 0 ? out.n == 0 : is_eoc(c);
}
// Allocate n linked clusters (in memory only). Returns the first, or 0.
uint32_t alloc_chain(uint32_t n, Chain& got) {
    got.clear();
    if (n == 0) return 0;
    for (uint32_t i = 0; i < v.nclus && got.n < n; i++) {
        uint32_t c = 2 + (v.hint - 2 + i) % v.nclus;
        if (fat_get(c) == 0) {
            if (!got.push(c)) break;
            fat_set(c, EOC);
        }
    }
    if (got.n < n) { for (uint32_t i = 0; i < got.n; i++) fat_set(got.c[i], 0); got.clear(); return 0; }
    for (uint32_t i = 0; i + 1 < got.n; i++) fat_set(got.c[i], got.c[i + 1]);
    uint32_t last = got.c[got.n - 1];
    v.hint = last + 1 > v.nclus + 1 ? 2 : last + 1;
    return got.c[0];
}
// Write `len` bytes over the clusters (zero-padding the last one), merging
// runs. Whole clusters go straight from the caller's buffer.
bool write_clusters(const Chain& cl, const void* data, size_t len) {
    const uint8_t* src = (const uint8_t*)data;
    uint32_t full = (uint32_t)(len / v.csize);           // clusters filled completely
    for (uint32_t i = 0; i < full && i < cl.n;) {
        uint32_t j = i + 1;
        while (j < full && j < cl.n && cl.c[j] == cl.c[j - 1] + 1) j++;
        if (!raw_write(clus_lba(cl.c[i]), (j - i) * v.spc, src + (size_t)i * v.csize)) return false;
        i = j;
    }
    if (full < cl.n) {                                    // the partial (or empty) tail
        Mem buf(v.csize);
        if (!buf.p) return false;
        memset(buf.p, 0, v.csize);
        memcpy(buf.p, src + (size_t)full * v.csize, len - (size_t)full * v.csize);
        for (uint32_t i = full; i < cl.n; i++) {
            if (!raw_write(clus_lba(cl.c[i]), v.spc, buf.p)) return false;
            if (i == full) memset(buf.p, 0, v.csize);
        }
    }
    return true;
}
// Read the first `len` bytes of the chain straight into `out`.
bool read_clusters(const Chain& cl, size_t len, void* out) {
    uint8_t* dst = (uint8_t*)out;
    uint32_t full = (uint32_t)(len / v.csize);
    for (uint32_t i = 0; i < full;) {
        uint32_t j = i + 1;
        while (j < full && cl.c[j] == cl.c[j - 1] + 1) j++;
        if (!raw_read(clus_lba(cl.c[i]), (j - i) * v.spc, dst + (size_t)i * v.csize)) return false;
        i = j;
    }
    size_t rest = len - (size_t)full * v.csize;
    if (rest) {
        Mem buf(v.csize);
        if (!buf.p || !raw_read(clus_lba(cl.c[full]), v.spc, buf.p)) return false;
        memcpy(dst + (size_t)full * v.csize, buf.p, rest);
    }
    return true;
}

// ---------------------------------------------------------------- time
void stamp(uint16_t& date, uint16_t& tm) {
    long t = fat_clock();
    long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    days += 719468;                                       // civil_from_days (H. Hinnant)
    long era = (days >= 0 ? days : days - 146096) / 146097;
    long doe = days - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y < 1980) { y = 1980; m = 1; d = 1; secs = 0; }
    if (y > 2107) y = 2107;
    date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
    tm = (uint16_t)((secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2);
}

// ---------------------------------------------------------------- names
struct Name {
    char s[NAME_MAX_LEN + 1];
    size_t n = 0;
    Name() { s[0] = 0; }
    bool set(const char* p, size_t len) {
        if (len > NAME_MAX_LEN) return false;
        memcpy(s, p, len); s[len] = 0; n = len;
        return true;
    }
    bool is(const char* lit) const { size_t k = strlen(lit); return n == k && !memcmp(s, lit, k); }
};
char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
bool same_name(const Name& a, const char* b, size_t n) {
    if (a.n != n) return false;
    for (size_t i = 0; i < n; i++) if (up(a.s[i]) != up(b[i])) return false;
    return true;
}
bool in_set(char c, const char* set) { for (; *set; set++) if (*set == c) return true; return false; }
bool sfn_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c & 0x80) || in_set(c, "!#$%&'()-@^_`{}~");
}
bool valid_long(const Name& s) {
    if (s.n == 0 || s.is(".") || s.is("..")) return false;
    for (size_t i = 0; i < s.n; i++)
        if ((unsigned char)s.s[i] < 0x20 || in_set(s.s[i], "\"*/:<>?\\|")) return false;
    return s.s[s.n - 1] != ' ' && s.s[s.n - 1] != '.';
}
void short_name(const Dirent& e, Name& out) {
    out.n = 0;
    for (int i = 0; i < 8 && e.name[i] != ' '; i++) {
        char c = (char)(i == 0 && e.name[0] == 0x05 ? 0xE5 : e.name[i]);
        out.s[out.n++] = (e.ntres & NT_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e.name[8] != ' ') {
        out.s[out.n++] = '.';
        for (int i = 8; i < 11 && e.name[i] != ' '; i++) {
            char c = (char)e.name[i];
            out.s[out.n++] = (e.ntres & NT_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out.s[out.n] = 0;
}
uint8_t lfn_sum(const uint8_t* n) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n[i]);
    return s;
}

// ---------------------------------------------------------------- directories
// A directory is its first cluster (0: the root). Entries are addressed by
// index (16 per sector).
bool dir_lba(uint32_t d, uint32_t k, uint32_t& lba) {
    uint32_t c = dstart(d);
    if (k / v.spc > v.nclus) return false;          // longer than the disk: a loop in the FAT
    for (uint32_t i = k / v.spc; i; i--) { c = fat_get(c); if (!valid(c)) return false; }
    lba = clus_lba(c) + k % v.spc;
    return true;
}
Dirent* ent_get(uint32_t d, uint32_t idx) {
    uint32_t lba;
    if (!dir_lba(d, idx / 16, lba)) return nullptr;
    uint8_t* s = sec_get(lba);
    return s ? (Dirent*)(s + (idx % 16) * 32) : nullptr;
}
bool ent_put(uint32_t d, uint32_t idx, const void* e) {
    Dirent* p = ent_get(d, idx);
    if (!p) return false;
    memcpy(p, e, 32);
    cache_dirty = true;
    return true;
}

struct Entry {
    Dirent e;
    Name name;
    uint32_t idx;      // the 8.3 entry
    uint32_t first;    // first entry belonging to it (its first LFN entry, or idx)
};

// Visit every live entry of directory d (volume labels skipped). fn returns
// false to stop. Returns false only on a disk error.
template <class F> bool dir_walk(uint32_t d, F fn) {
    uint16_t lfn[20 * 13 + 1];
    int lfn_next = 0;                   // the next ordinal expected (0: none, -1: complete)
    uint8_t lfn_csum = 0;
    uint32_t lfn_first = 0;
    Entry en;
    for (uint32_t idx = 0;; idx++) {
        uint32_t lba;
        if (!dir_lba(d, idx / 16, lba)) return true;
        Dirent* p = ent_get(d, idx);
        if (!p) return false;
        Dirent e = *p;
        if (e.name[0] == 0x00) return true;
        if (e.name[0] == 0xE5) { lfn_next = 0; continue; }
        if (e.attr == A_LFN) {
            const LfnEnt& l = (const LfnEnt&)e;
            int ord = l.ord & 0x1F;
            if (l.ord & 0x40) {
                if (ord < 1 || ord > 20) { lfn_next = 0; continue; }
                memset(lfn, 0, sizeof lfn);
                lfn_next = ord; lfn_csum = l.sum; lfn_first = idx;
            } else if (ord != lfn_next || l.sum != lfn_csum) { lfn_next = 0; continue; }
            uint16_t* o = &lfn[(ord - 1) * 13];
            memcpy(o, l.n1, 10); memcpy(o + 5, l.n2, 12); memcpy(o + 11, l.n3, 4);
            lfn_next = ord - 1;
            if (lfn_next == 0) lfn_next = -1;            // complete; the 8.3 entry comes next
            continue;
        }
        if (e.attr & A_VOL) { lfn_next = 0; continue; }
        en.e = e; en.idx = en.first = idx;
        if (lfn_next == -1 && lfn_sum(e.name) == lfn_csum) {
            en.name.n = 0;
            for (int i = 0; i < 20 * 13 && lfn[i] && lfn[i] != 0xFFFF && en.name.n < NAME_MAX_LEN; i++)
                en.name.s[en.name.n++] = lfn[i] < 0x80 ? (char)lfn[i] : '?';
            en.name.s[en.name.n] = 0;
            en.first = lfn_first;
        } else {
            short_name(e, en.name);
        }
        lfn_next = 0;
        if (!fn(en)) return true;
    }
}

// 0 = not found, 1 = found, -1 = disk error.
int dir_find(uint32_t d, const char* name, size_t n, Entry& out) {
    bool hit = false;
    bool ok = dir_walk(d, [&](const Entry& en) {
        if (same_name(en.name, name, n)) { out = en; hit = true; return false; }
        return true;
    });
    return !ok ? -1 : hit ? 1 : 0;
}

bool make_dir_in(uint32_t parent, const Name& name, uint32_t& made);
// Split a path into directory (cluster) and final component. Missing
// directories fail unless `make` is set, in which case they're created.
bool resolve_parent(const char* path, uint32_t& d, Name& leaf, bool make = false) {
    d = 0;
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;
        const char* e = p;
        while (*e && *e != '/') e++;
        const char* rest = e;
        while (*rest == '/') rest++;
        if (!*rest) return leaf.set(p, e - p);           // last component
        size_t n = e - p;
        if (n == 1 && p[0] == '.') { p = e; continue; }
        Entry en;
        int r = dir_find(d, p, n, en);
        if (r < 0) return false;
        if (r == 0) {
            Name nm;
            if (!make || (n == 2 && p[0] == '.' && p[1] == '.') || !nm.set(p, n)) return false;
            uint32_t made;
            if (!make_dir_in(d, nm, made)) return false;
            d = made;
        } else {
            if (!(en.e.attr & A_DIR)) return false;
            d = norm_dir(ecl(en.e));                     // ".." to the root is cluster 0
        }
        p = e;
    }
}
// Look up a whole path. The root itself is reported with is_root set.
int lookup(const char* path, uint32_t& parent, Entry& en, bool& is_root) {
    Name leaf;
    is_root = false;
    if (!resolve_parent(path, parent, leaf)) return 0;
    if (leaf.n == 0 || leaf.is(".")) {
        if (parent == 0) { is_root = true; return 1; }
        return dir_find(parent, ".", 1, en);
    }
    return dir_find(parent, leaf.s, leaf.n, en);
}

// The name fits in an 8.3 entry as is (using the lowercase flags for an
// all-lowercase base or extension).
bool exact_83(const Name& s, uint8_t out[11], uint8_t& nt) {
    size_t dot = s.n;
    for (size_t i = 0; i < s.n; i++) if (s.s[i] == '.') { dot = i; break; }
    size_t bl = dot, el = dot < s.n ? s.n - dot - 1 : 0;
    if (bl == 0 || bl > 8 || el > 3 || (dot < s.n && el == 0)) return false;
    nt = 0;
    auto part = [&](const char* p, size_t len, uint8_t* o, size_t w, uint8_t flag) {
        bool lo = false, upc = false;
        memset(o, ' ', w);
        for (size_t i = 0; i < len; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') { lo = true; c -= 32; } else if (c >= 'A' && c <= 'Z') upc = true;
            if (!sfn_char(c)) return false;              // also rejects a second '.'
            o[i] = (uint8_t)c;
        }
        if (lo && upc) return false;
        if (lo) nt |= flag;
        return true;
    };
    if (!part(s.s, bl, out, 8, NT_LOWER_BASE) || !part(s.s + dot + 1, el, out + 8, 3, NT_LOWER_EXT)) return false;
    if (out[0] == 0xE5) out[0] = 0x05;
    return true;
}
bool sfn_taken(uint32_t d, const uint8_t n[11], bool& err) {
    bool taken = false;
    err = !dir_walk(d, [&](const Entry& en) {
        if (!memcmp(en.e.name, n, 11)) { taken = true; return false; }
        return true;
    });
    return taken;
}
// A unique 8.3 alias in the Windows style: NAME~1.EXT, NAME~2.EXT, ...
bool make_alias(uint32_t d, const Name& s, uint8_t out[11]) {
    char base[9], ext[4];
    size_t bl = 0, el = 0;
    size_t dot = s.n;
    for (size_t i = s.n; i-- > 1;) if (s.s[i] == '.') { dot = i; break; }
    auto clean = [](char c) -> char { c = up(c); return sfn_char(c) ? c : '_'; };
    for (size_t i = 0; i < dot; i++)
        if (s.s[i] != ' ' && s.s[i] != '.' && bl < 8) base[bl++] = clean(s.s[i]);
    for (size_t i = dot + 1; i < s.n && el < 3; i++)
        if (s.s[i] != ' ' && s.s[i] != '.') ext[el++] = clean(s.s[i]);
    if (bl == 0) base[bl++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8], digits[8];
        int tl = 0, dl = 0;
        for (uint32_t x = n; x; x /= 10) digits[dl++] = (char)('0' + x % 10);
        tail[tl++] = '~';
        while (dl) tail[tl++] = digits[--dl];
        size_t keep = bl < (size_t)(8 - tl) ? bl : (size_t)(8 - tl);
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, tl);
        memcpy(out + 8, ext, el);
        if (out[0] == 0xE5) out[0] = 0x05;
        bool err;
        if (!sfn_taken(d, out, err)) return !err;
    }
    return false;
}

// Make room for `n` consecutive entries in directory d; returns the index.
bool find_slots(uint32_t d, uint32_t n, uint32_t& at) {
    uint32_t run = 0, start = 0;
    for (uint32_t idx = 0;; idx++) {
        Dirent* p = ent_get(d, idx);
        if (!p) {
            uint32_t lba;
            if (dir_lba(d, idx / 16, lba)) return false;  // disk error, not the end
            // Grow the directory (the root too) by one zeroed cluster.
            Chain cl, got;
            if (!chain(dstart(d), cl) || !cl.n || !alloc_chain(1, got)) return false;
            Mem z(v.csize);
            if (!z.p) { fat_set(got.c[0], 0); return false; }
            memset(z.p, 0, v.csize);
            if (!raw_write(clus_lba(got.c[0]), v.spc, z.p)) { fat_set(got.c[0], 0); return false; }
            fat_set(cl.c[cl.n - 1], got.c[0]);
            if (!fat_flush()) return false;
            if (run == 0) start = idx;
            at = start;                                   // the run continues into the new cluster
            return true;
        }
        if (p->name[0] == 0x00 || p->name[0] == 0xE5) {
            if (run++ == 0) start = idx;
            if (run == n) { at = start; return true; }
        } else {
            run = 0;
        }
    }
}

// Add an entry named `name` to directory d, filled from `tmpl` (attr, cluster,
// size, times). Leaves the directory sector in the cache; caller flushes.
bool dir_add(uint32_t d, const Name& name, const Dirent& tmpl) {
    if (!valid_long(name)) return false;
    Dirent e = tmpl;
    uint8_t nt = 0;
    uint32_t nlfn = 0;
    bool err = false;
    if (exact_83(name, e.name, nt) && !sfn_taken(d, e.name, err)) {
        e.ntres = nt;
    } else {
        if (err || !make_alias(d, name, e.name)) return false;
        e.ntres = 0;
        nlfn = (uint32_t)(name.n + 12) / 13;
    }
    uint32_t at;
    if (!find_slots(d, nlfn + 1, at)) return false;
    uint8_t sum = lfn_sum(e.name);
    for (uint32_t i = 0; i < nlfn; i++) {
        uint32_t ord = nlfn - i;
        LfnEnt l;
        memset(&l, 0, sizeof l);
        l.ord = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        l.attr = A_LFN;
        l.sum = sum;
        uint16_t ch[13];
        for (int k = 0; k < 13; k++) {
            size_t pos = (ord - 1) * 13 + k;
            ch[k] = pos < name.n ? (uint8_t)name.s[pos] : pos == name.n ? 0x0000 : 0xFFFF;
        }
        memcpy(l.n1, ch, 10); memcpy(l.n2, ch + 5, 12); memcpy(l.n3, ch + 11, 4);
        if (!ent_put(d, at + i, &l)) return false;
    }
    return ent_put(d, at + nlfn, &e);
}
bool dir_del(uint32_t d, const Entry& en) {
    for (uint32_t i = en.first; i <= en.idx; i++) {
        Dirent* p = ent_get(d, i);
        if (!p) return false;
        p->name[0] = 0xE5;
        cache_dirty = true;
    }
    return true;
}
Dirent new_dirent(uint8_t attr, uint32_t clus, uint32_t size) {
    Dirent e;
    memset(&e, 0, sizeof e);
    e.attr = attr;
    set_ecl(e, clus);
    e.size = size;
    uint16_t dt, tm;
    stamp(dt, tm);
    e.cdate = e.mdate = e.adate = dt;
    e.ctime = e.mtime = tm;
    return e;
}

bool make_dir_in(uint32_t parent, const Name& name, uint32_t& made) {
    if (!valid_long(name)) return false;
    Chain got;
    if (!alloc_chain(1, got)) return false;
    uint32_t c = got.c[0];
    Mem buf(v.csize);
    if (!buf.p) { fat_set(c, 0); return false; }
    memset(buf.p, 0, v.csize);
    Dirent dot = new_dirent(A_DIR, c, 0), dotdot = new_dirent(A_DIR, parent, 0);
    memset(dot.name, ' ', 11); dot.name[0] = '.';
    memset(dotdot.name, ' ', 11); dotdot.name[0] = dotdot.name[1] = '.';
    memcpy(buf.p, &dot, 32);
    memcpy(buf.p + 32, &dotdot, 32);
    if (!raw_write(clus_lba(c), v.spc, buf.p) || !fat_flush()) { fat_set(c, 0); return false; }
    if (!dir_add(parent, name, new_dirent(A_DIR, c, 0)) || !cache_flush()) {
        fat_set(c, 0); fat_flush();
        return false;
    }
    made = c;
    return true;
}

bool dir_empty(uint32_t d, bool& empty) {
    empty = true;
    return dir_walk(d, [&](const Entry& en) {
        if (en.name.is(".") || en.name.is("..")) return true;
        empty = false;
        return false;
    });
}
// The ".." cluster of a subdirectory (0 for the root's children).
bool parent_of(uint32_t d, uint32_t& p) {
    Dirent* e = ent_get(d, 1);
    if (!e) return false;
    p = norm_dir(ecl(*e));
    return true;
}

struct Op { ~Op() { cache_flush(); idle(); } };   // end of every public call

uint32_t ecl(const Dirent& e) { return (uint32_t)e.clus_hi << 16 | e.clus; }
void set_ecl(Dirent& e, uint32_t c) { e.clus = (uint16_t)c; e.clus_hi = (uint16_t)(c >> 16); }

} // namespace

// ---------------------------------------------------------------- public API
__attribute__((weak)) long fat_clock() { return 0; }

bool fat_mount(const FatDisk& disk) {
    fat_unmount();
    v.disk = disk;
    Op op;
    uint8_t b[SEC];
    if (!v.disk.read(0, 1, b)) return false;
    auto u16 = [&](int o) { return (uint32_t)(b[o] | b[o + 1] << 8); };
    auto u32 = [&](int o) { return u16(o) | u16(o + 2) << 16; };
    uint32_t bps = u16(11), spc = b[13], rsv = u16(14), nfats = b[16], roots = u16(17);
    uint32_t total = u16(19) ? u16(19) : u32(32);
    // FAT32: no fixed root directory, the FAT size in the extended BPB.
    if (bps != SEC || !spc || (spc & (spc - 1)) || !rsv || !nfats || roots || u16(22)) return false;
    uint32_t fatsz = u32(36), ext = u16(40);
    if (!fatsz) return false;
    v.spc = spc; v.csize = spc * SEC;
    v.fat_lba = rsv; v.fat_secs = fatsz; v.nfats = nfats;
    v.active_fat = (ext & 0x80) ? (ext & 0x0F) % nfats : 0;
    v.data_lba = rsv + nfats * fatsz;
    if (total <= v.data_lba) return false;
    v.nclus = (total - v.data_lba) / spc;
    uint64_t fit = (uint64_t)fatsz * SEC / 4;            // entries the FAT can hold
    if (fit < 3) return false;
    if (v.nclus + 2 > fit) v.nclus = (uint32_t)(fit - 2);
    if (v.nclus > 0x0FFFFFF5) v.nclus = 0x0FFFFFF5;
    v.root_clus = u32(44);
    if (!valid(v.root_clus)) return false;
    v.fat = (uint8_t*)malloc((size_t)fatsz * SEC);
    v.fat_dirty = (uint8_t*)malloc(fatsz);
    if (!v.fat || !v.fat_dirty) return false;
    memset(v.fat_dirty, 0, fatsz);
    bool ok = v.disk.read(v.fat_lba + v.active_fat * fatsz, fatsz, v.fat);
    for (uint32_t k = 0; k < nfats && !ok; k++) ok = v.disk.read(v.fat_lba + k * fatsz, fatsz, v.fat);
    if (!ok) return false;
    // FSInfo: we don't keep its free-cluster count, so mark it unknown
    // (anyone mounting the image later recounts).
    uint32_t fsinfo = u16(48);
    if (fsinfo && fsinfo < rsv && v.disk.read(fsinfo, 1, b) && u32(0) == 0x41615252 && u32(484) == 0x61417272) {
        memset(b + 488, 0xFF, 8);
        v.disk.write(fsinfo, 1, b);
    }
    cache_lba = NONE; cache_dirty = false;
    v.hint = 2;
    v.mounted = true;
    return true;
}
bool fat_mounted() { return v.mounted; }
void fat_unmount() {
    if (v.mounted) { cache_flush(); fat_flush(); idle(); }
    v.mounted = false;
    free(v.fat); v.fat = nullptr;
    free(v.fat_dirty); v.fat_dirty = nullptr;
    cache_lba = NONE; cache_dirty = false;
}

bool fat_exists(const char* path, bool* is_dir, uint32_t* size) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1) return false;
    if (is_dir) *is_dir = root || (en.e.attr & A_DIR);
    if (size) *size = root ? 0 : en.e.size;
    return true;
}

bool fat_read(const char* path, void* buf, size_t cap, size_t* len) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1 || root || (en.e.attr & A_DIR)) return false;
    if (len) *len = en.e.size;
    if (en.e.size > cap) return false;
    Chain cl;
    if (!chain(ecl(en.e), cl)) return false;
    if ((uint64_t)cl.n * v.csize < en.e.size) return false;
    return read_clusters(cl, en.e.size, buf);
}

bool fat_extent(const char* path, uint32_t* first_lba, uint32_t* size) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1 || root || (en.e.attr & A_DIR)) return false;
    // Walk the chain in place (no allocation: this runs when memory is short).
    uint32_t first = ecl(en.e), c = first, n = 0;
    while (valid(c)) {
        uint32_t next = fat_get(c);
        n++;
        if (n > v.nclus) return false;                   // a loop in the FAT
        if (valid(next) && next != c + 1) return false;  // not one run
        c = next;
    }
    if (!(c == 0 ? n == 0 : is_eoc(c)) || (uint64_t)n * v.csize < en.e.size) return false;
    *first_lba = n ? clus_lba(first) : 0;
    *size = en.e.size;
    return true;
}

bool fat_write(const char* path, const void* data, size_t len) {
    if (!v.mounted || (uint64_t)len > 0xFFFFFFFFu) return false;
    Op op;
    uint32_t d;
    Name leaf;
    if (!resolve_parent(path, d, leaf)) return false;
    Entry en;
    int r = dir_find(d, leaf.s, leaf.n, en);
    if (r < 0 || (r == 1 && (en.e.attr & (A_DIR | A_RO)))) return false;
    if (r == 0 && !valid_long(leaf)) return false;

    // 1. Data into fresh clusters, then the FAT that links them.
    Chain cl;
    uint32_t n = (uint32_t)((len + v.csize - 1) / v.csize);
    uint32_t c = alloc_chain(n, cl);
    if (n && !c) return false;
    if (!write_clusters(cl, data, len) || !fat_flush()) { free_chain(c); fat_flush(); return false; }

    // 2. Point the directory entry at them.
    if (r == 1) {
        Dirent* p = ent_get(d, en.idx);
        if (!p) { free_chain(c); fat_flush(); return false; }
        uint32_t old = ecl(*p);
        uint16_t dt, tm;
        stamp(dt, tm);
        set_ecl(*p, c);
        p->size = (uint32_t)len;
        p->attr |= A_ARC;
        p->mdate = p->adate = dt;
        p->mtime = tm;
        cache_dirty = true;
        if (!cache_flush()) return false;
        // 3. Only now release the old data.
        free_chain(old);
        return fat_flush();
    }
    if (!dir_add(d, leaf, new_dirent(A_ARC, c, (uint32_t)len)) || !cache_flush()) {
        free_chain(c); fat_flush();
        return false;
    }
    return true;
}

bool fat_append(const char* path, const void* data, size_t len) {
    bool dir = false;
    uint32_t size = 0;
    bool exists = fat_exists(path, &dir, &size);
    if (exists && dir) return false;
    if (!exists) size = 0;
    Mem all(size + len);
    if (!all.p) return false;
    size_t got = 0;
    if (size && (!fat_read(path, all.p, size, &got) || got != size)) return false;
    if (len) memcpy(all.p + size, data, len);
    return fat_write(path, all.p, size + len);
}

bool fat_mkdir(const char* path) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d;
    Name leaf;
    if (!resolve_parent(path, d, leaf, true)) return false;
    if (leaf.n == 0 || leaf.is(".")) return true;
    Entry en;
    int r = dir_find(d, leaf.s, leaf.n, en);
    if (r < 0) return false;
    if (r == 1) return (en.e.attr & A_DIR) != 0;
    uint32_t made;
    return make_dir_in(d, leaf, made);
}

bool fat_remove(const char* path) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1 || root || en.name.is(".") || en.name.is("..")) return false;
    if (en.e.attr & A_DIR) {
        bool empty;
        if (!dir_empty(ecl(en.e), empty) || !empty) return false;
    }
    if (!dir_del(d, en) || !cache_flush()) return false;
    free_chain(ecl(en.e));
    return fat_flush();
}

bool fat_rename(const char* from, const char* to) {
    if (!v.mounted) return false;
    Op op;
    uint32_t sd; Entry src; bool root;
    if (lookup(from, sd, src, root) != 1 || root || src.name.is(".") || src.name.is("..")) return false;
    uint32_t dd;
    Name leaf;
    if (!resolve_parent(to, dd, leaf) || !valid_long(leaf)) return false;
    bool is_dir = src.e.attr & A_DIR;
    if (is_dir) {                                       // not into itself or a descendant
        for (uint32_t p = dd, n = 0; p != 0; n++) {
            if (p == ecl(src.e) || n > v.nclus) return false;
            if (!parent_of(p, p)) return false;
        }
    }
    Entry dst;
    int r = dir_find(dd, leaf.s, leaf.n, dst);
    if (r < 0) return false;
    bool same = r == 1 && dd == sd && dst.idx == src.idx;
    if (r == 1 && !same) {
        if (is_dir || (dst.e.attr & (A_DIR | A_RO))) return false;
        if (!dir_del(dd, dst) || !cache_flush()) return false;      // replace an existing file
        free_chain(ecl(dst.e));
        if (!fat_flush()) return false;
    }
    if (same) {                                         // only the case changes
        if (!dir_del(sd, src) || !dir_add(dd, leaf, src.e)) return false;
        return cache_flush();
    }
    if (!dir_add(dd, leaf, src.e) || !cache_flush()) return false;
    // Find the source again (the directory may have grown) and drop it.
    Name sleaf;
    uint32_t sd2;
    Entry again;
    if (!resolve_parent(from, sd2, sleaf) || dir_find(sd2, sleaf.s, sleaf.n, again) != 1) return false;
    if (!dir_del(sd2, again)) return false;
    if (is_dir && sd != dd) {                           // fix the moved directory's ".."
        Dirent* p = ent_get(ecl(src.e), 1);
        if (!p) return false;
        set_ecl(*p, dd);
        cache_dirty = true;
    }
    return cache_flush();
}

bool fat_list(const char* path, bool (*fn)(const char*, uint32_t, bool, void*), void* ctx) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1) return false;
    if (!root && !(en.e.attr & A_DIR)) return false;
    uint32_t dir = root ? 0 : norm_dir(ecl(en.e));
    // fn may use the disk too, so walk by index and re-find our place each time.
    for (uint32_t skip = 0;; skip++) {
        uint32_t seen = 0;
        Entry hit;
        bool found = false;
        if (!dir_walk(dir, [&](const Entry& e) {
                if (e.name.is(".") || e.name.is("..")) return true;
                if (seen++ < skip) return true;
                hit = e; found = true;
                return false;
            }))
            return false;
        if (!found) return true;
        if (!fn(hit.name.s, hit.e.size, (hit.e.attr & A_DIR) != 0, ctx)) return true;
    }
}

uint64_t fat_free_bytes() {
    if (!v.mounted) return 0;
    uint64_t n = 0;
    for (uint32_t c = 2; c <= v.nclus + 1; c++) n += fat_get(c) == 0;
    return n * v.csize;
}
