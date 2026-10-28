// Host test for fat32.cpp: runs the driver against a FAT32 image file and
// prints what it did. tools/fat32_test.sh checks the result with fsck.fat
// and mtools.
//
//   g++ -std=gnu++17 -I.. fat32_test.cpp ../fat32.cpp -o fat32_test
//   ./fat32_test image.img
#include "fat32.hpp"
#include <stdio.h>
#include <string.h>
#include <string>

// The driver reads into a caller's buffer; the checks below want strings.
static bool fat_read(const char* path, std::string& out) {
    uint32_t size;
    bool dir;
    if (!fat_exists(path, &dir, &size) || dir) return false;
    out.assign(size, 0);
    size_t got;
    return fat_read(path, &out[0], size, &got) && got == size;
}

static FILE* img;
static bool rd(uint32_t lba, uint32_t n, void* b) {
    return fseek(img, (long)lba * 512, SEEK_SET) == 0 && fread(b, 512, n, img) == n;
}
static bool wr(uint32_t lba, uint32_t n, const void* b) {
    return fseek(img, (long)lba * 512, SEEK_SET) == 0 && fwrite(b, 512, n, img) == n;
}

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static bool show(const char* name, uint32_t size, bool dir, void*) {
    printf("  %-24s %s %u\n", name, dir ? "<DIR>" : "     ", size);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2 || !(img = fopen(argv[1], "r+b"))) { printf("usage: fat32_test image\n"); return 2; }
    CHECK(fat_mount({rd, wr, nullptr}));
    uint64_t free0 = fat_free_bytes();
    printf("free: %llu bytes\n", (unsigned long long)free0);

    // What mtools put there.
    std::string s;
    CHECK(fat_read("/boot/grub/grub.cfg", s) && s.find("multiboot") != std::string::npos);
    CHECK(fat_read("PRESET.TXT", s) && s == "made by mtools\n");
    CHECK(fat_read("/preset long name.txt", s) && s == "long\n");
    if (argc > 2) {                                          // a WAD mtools copied in, byte for byte
        FILE* w = fopen(argv[2], "rb");
        std::string want;
        char tmp[65536];
        size_t k;
        while (w && (k = fread(tmp, 1, sizeof tmp, w)) > 0) want.append(tmp, k);
        if (w) fclose(w);
        CHECK(fat_read("/GAME.WAD", s) && s == want);
        printf("game.wad: %zu bytes read back\n", s.size());
        uint32_t lba = 0, size = 0;
        CHECK(fat_extent("/game.wad", &lba, &size) && size == want.size() && lba);   // mtools writes it in one run
    }

    // The game's files.
    CHECK(fat_mkdir("/saves/"));
    CHECK(fat_mkdir("/saves"));                                  // already there: fine
    const char* stats = "played 3\nwins 2\ncurrent_streak 1\n";
    CHECK(fat_write("/saves/stats.txt", stats, strlen(stats)));
    CHECK(fat_write("/saves/stats-easy.txt", "played 1\n", 9));
    CHECK(fat_write("/saves/stats-expert.txt", "played 7\n", 9));
    CHECK(fat_write("/saves/mode.txt", "1\n", 2));
    for (int i = 0; i < 300; i++) {                                    // grows past one cluster
        char line[64];
        int n = snprintf(line, sizeof line, "2026-10-01 07:%02d,crane,%d/6,0\n", i % 60, i % 6 + 1);
        CHECK(fat_append("/saves/history.csv", line, n));
    }
    CHECK(fat_read("/SAVES/STATS.TXT", s) && s == stats);      // case-insensitive
    CHECK(fat_write("/saves/stats.txt", "played 4\n", 9));       // replace, shorter
    CHECK(fat_read("/saves/stats.txt", s) && s == "played 4\n");

    // Big file across many tracks, then shrink it.
    std::string big(200000, 0);
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 131 + i / 512);
    CHECK(fat_write("/data/deep/er/big.bin", big.data(), big.size()) == false);   // no parent
    CHECK(fat_mkdir("/data/deep/er"));
    CHECK(fat_write("/data/deep/er/big.bin", big.data(), big.size()));
    CHECK(fat_read("/data/deep/er/big.bin", s) && s == big);
    CHECK(fat_write("/data/deep/er/big.bin", "x", 1));
    CHECK(fat_write("/data/empty.txt", "", 0));
    CHECK(fat_read("/data/empty.txt", s) && s.empty());

    // A directory with enough long names to need several clusters.
    for (int i = 0; i < 40; i++) {
        char p[96];
        snprintf(p, sizeof p, "/many/A rather long file name number %02d.text", i);
        if (i == 0) CHECK(fat_mkdir("/many"));
        CHECK(fat_write(p, p, strlen(p)));
    }
    CHECK(fat_read("/many/a RATHER long FILE name number 39.TEXT", s) && s == "/many/A rather long file name number 39.text");

    // Lots of files in the root: FAT32's root grows like any directory.
    for (int i = 0; i < 200; i++) {
        char p[64];
        snprintf(p, sizeof p, "/root file %03d.txt", i);
        CHECK(fat_write(p, p, strlen(p)));
    }
    CHECK(fat_read("/ROOT FILE 199.TXT", s) && s == "/root file 199.txt");
    for (int i = 0; i < 200; i++) {
        char p[64];
        snprintf(p, sizeof p, "/root file %03d.txt", i);
        CHECK(fat_remove(p));
    }

    // Rename / move / delete.
    CHECK(fat_rename("/saves/mode.txt", "/saves/Mode.txt"));
    CHECK(fat_rename("/data/empty.txt", "/saves/moved here.txt"));
    CHECK(fat_rename("/data/deep", "/saves/deep"));
    CHECK(fat_rename("/saves", "/saves/deep/x") == false);  // into itself
    CHECK(fat_read("/saves/deep/er/big.bin", s) && s == "x");
    CHECK(fat_remove("/many") == false);                               // not empty
    for (int i = 0; i < 40; i += 2) {
        char p[96];
        snprintf(p, sizeof p, "/many/A rather long file name number %02d.text", i);
        CHECK(fat_remove(p));
    }
    CHECK(fat_write("/many/reuses a deleted slot.txt", "r", 1));
    CHECK(fat_exists("/many/A rather long file name number 02.text") == false);

    CHECK(fat_write("bad:name", "x", 1) == false);
    CHECK(fat_write("/saves", "x", 1) == false);                  // is a directory

    printf("/saves:\n");
    fat_list("/saves", show, nullptr);
    printf("/:\n");
    fat_list("/", show, nullptr);
    fat_unmount();

    // Remount and read back.
    CHECK(fat_mount({rd, wr, nullptr}));
    CHECK(fat_read("/saves/history.csv", s) && s.size() == 300 * 29);
    printf("history.csv: %zu bytes, free now %llu\n", s.size(), (unsigned long long)fat_free_bytes());
    fat_unmount();
    fclose(img);
    printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
