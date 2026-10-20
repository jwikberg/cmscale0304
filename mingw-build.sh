#!/bin/sh
# Builds dsound.dll (32-bit) next to this script. Needs mingw-w64 (i686-w64-mingw32-gcc).
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -msse2 -mfpmath=sse -Wall -Wextra -Wno-unused-parameter -shared -static-libgcc \
    -o dsound.dll src/cmscale.c src/dsound.def -lopengl32 -lgdi32 -luser32 -lwinmm
