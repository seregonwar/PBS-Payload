#!/usr/bin/env sh
# Host-only request/lifecycle matrix. Never contacts a console.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
mkdir -p build/diagnostics-940/real120-analysis
for height in 720 1080 1440 2160; do
  for fps_mode in 1 2; do
    for capture in 0 1; do
      cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
        -I tests/stubs -I payload/src "-DPBS_940_HEIGHT=$height" \
        "-DPBS_940_CAPTURE=$capture" "-DPBS_940_FPS_MODE=$fps_mode" \
        tests/ps5/diagnostics_940_mock.c payload/src/platform/ps5/offset/940.c \
        -o build/diagnostics-940/real120-analysis/mock-profile
      echo "PROFILE height=$height enum=$fps_mode capture=$capture"
      build/diagnostics-940/real120-analysis/mock-profile
    done
  done
done
