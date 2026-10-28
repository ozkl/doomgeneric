#!/bin/sh
# Exercise fat32.cpp on a fresh FAT32 image (with a file mtools put there
# and, if one is given, a WAD) and check the result with fsck.fat and mtools.
#   tools/fat32_test.sh [file.wad]
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkfs.fat -C -F 32 -S 512 -s 1 "$T/f.img" 40960 >/dev/null
mmd -i "$T/f.img" ::/boot ::/boot/grub
echo "multiboot /boot/x" > "$T/g"; mcopy -i "$T/f.img" "$T/g" ::/boot/grub/grub.cfg
printf 'made by mtools\n' > "$T/p"; mcopy -i "$T/f.img" "$T/p" ::/PRESET.TXT
printf 'long\n' > "$T/l"; mcopy -i "$T/f.img" "$T/l" "::/preset long name.txt"
WAD=
if [ -n "$1" ]; then cp "$1" "$T/w"; mcopy -i "$T/f.img" "$T/w" ::/game.wad; WAD="$T/w"; fi

${CXX:-g++} -std=gnu++17 -O1 -g -fsanitize=address,undefined -I.. fat32_test.cpp ../fat32.cpp -o "$T/t"
"$T/t" "$T/f.img" $WAD

echo "--- fsck.fat"
fsck.fat -n -v "$T/f.img" | tail -3
echo "--- mtools"
mdir -i "$T/f.img" ::/saves
mtype -i "$T/f.img" ::/saves/stats-expert.txt
mtype -i "$T/f.img" ::/saves/history.csv | tail -2
mtype -i "$T/f.img" "::/many/A rather long file name number 39.text"; echo
mdir -i "$T/f.img" ::/many | tail -4
