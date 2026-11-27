#!/bin/sh
# Build and run the STARLANE desktop simulator (needs g++; on Windows run it
# in WSL:  wsl sh sim/run_starlane.sh). Frames land in sim/out/starlane as
# PPM; run  python sim/starlane_png.py  for PNGs and a contact sheet.
#   SAN=1 sh sim/run_starlane.sh   builds with address/undefined sanitizers
set -e
cd "$(dirname "$0")"
mkdir -p out/starlane build
FLAGS="-O2 -g"
if [ -n "$SAN" ]; then FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"; fi
g++ $FLAGS -std=c++17 -Wall -Wextra -Wdouble-promotion -Wno-unused-parameter \
  -o build/starlane_sim starlane_sim.cpp ../src/starlane/sl_*.cpp -lm
rm -f out/starlane/*.ppm out/starlane/*.png
./build/starlane_sim out/starlane
