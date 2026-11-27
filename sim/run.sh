#!/bin/sh
# Build and run the desktop simulator (needs g++; on Windows run it in WSL:
#   wsl sh sim/run.sh). Frames land in sim/out as PPM; pass them to
# sim/to_png.py for PNGs and a contact sheet.
set -e
cd "$(dirname "$0")"
mkdir -p out build
g++ -O2 -std=c++17 -DWHACK_SIM -o build/whack_sim whack_sim.cpp ../src/whack_core.cpp -lm
g++ -O2 -std=c++17 -Istubs -o build/flap_bench flap_bench.cpp ../src/neon_flap.cpp -lm
rm -f out/*.ppm
./build/whack_sim out
./build/flap_bench
