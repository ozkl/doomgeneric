#!/bin/bash
# Compile the WAH host (Windows EXE)
# Run from the project root (c:/dev/doomwah) in MINGW64 shell

set -e
echo "=== Compiling WAH host ==="
gcc -O2 host/main.c -o doomwah.exe -lgdi32 -lwinmm -luser32
echo "=== Build complete: doomwah.exe ==="
ls -la doomwah.exe
