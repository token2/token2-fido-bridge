#!/bin/sh
# Convenience native build.
set -e
cmake -B build -S .
cmake --build build -j"$(nproc)"
echo "Built: build/token2-fido-bridge"
