#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline regression only. Never connects to a console or its ELF loader.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
cc=${CC:-cc}
out=build/ps4-tests
mkdir -p "$out"
flags=(-std=gnu11 -O2 -Wall -Wextra -Werror -Ipayload/src -Ipayload/build/generated -Itests/stubs -Itools)
run() {
  local name=$1
  shift
  "$cc" "${flags[@]}" "$@" -o "$out/$name"
  "$out/$name"
}
run firmware tests/ps4/system/firmware.c
run ps5-capture-cleanup tests/ps5/capture_cleanup.c
run ps5-capture-resolution tests/ps5/capture_resolution.c
run ps5-request-initializers tests/ps5/request_initializers.c payload/src/platform/ps5/offset/*.c
run ps5-request-template -DPBS_TEST tests/ps5/request_template.c payload/src/platform/ps5/request_template.c payload/src/platform/ps5/offset/*.c
run ps5-offset-dump tests/ps5/offset_dump_dependencies.c payload/src/platform/ps5/offset_dump.c
run ps5-capture-audio tests/ps5/capture_audio_request.c
run ps5-controller tests/ps5/controller_backend.c payload/src/platform/ps5/offset/*.c
run ps5-controller-disabled -pthread -DPBS_PS5_CONTROLLER=0 tests/ps5/controller_disabled.c payload/src/common/input/controller.c
run problem-log tests/common/problem_log.c payload/src/common/diagnostics/problem_log.c
run config tests/common/config.c payload/src/common/config/config.c
run log -pthread tests/common/log.c payload/src/common/log/log.c payload/src/common/config/config.c
run offset-report tests/ps4/dump/offset_report.c tools/dump/ps4/offset_report.c
run kernel-image tests/ps4/dump/kernel_image.c tools/dump/ps4/kernel_image.c
run layout tests/ps4/system/layout.c payload/src/platform/ps4/system/layout.c
python3 -m unittest discover -s tests -p test_ps4_672_layout.py
python3 -m unittest discover -s tests -p test_embed_assets.py
python3 -m unittest discover -s tests -p test_ps5_dump_names.py
run layout-runtime -DPBS_PS4_DYNAMIC_LAYOUT=1 -DPBS_TEST_CONTROL -DKERNEL_IMAGE_SIZE=0x100000 tests/ps4/system/layout_runtime.c payload/src/platform/ps4/system/layout_runtime.c
run export-reader tests/ps4/capture/export_reader.c payload/src/platform/ps4/capture/export_reader.c
run remote-exported -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_EXPORTED_READ=1 tests/ps4/capture/remote_exported.c payload/src/platform/ps4/capture/remote.c payload/src/platform/ps4/capture/export_reader.c
run snapshot -DPBS_PS4 tests/ps4/diagnostics/snapshot.c payload/src/platform/ps4/diagnostics/snapshot.c
run admission -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4/diagnostics/admission.c payload/src/platform/ps4/diagnostics/admission.c
run encoder-snapshot -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4/diagnostics/encoder_snapshot.c payload/src/platform/ps4/diagnostics/admission.c
run venc-journal -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4/diagnostics/venc_journal.c payload/src/platform/ps4/diagnostics/venc_journal.c
run latch-patch -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4/diagnostics/latch_patch.c payload/src/platform/ps4/diagnostics/latch_patch.c
run source-cap -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=0 -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4/diagnostics/latch_patch.c payload/src/platform/ps4/diagnostics/latch_patch.c
run request-native-source -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4/capture/video_request.c
run snapshot-native-source -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4/diagnostics/encoder_snapshot.c payload/src/platform/ps4/diagnostics/admission.c
run remote-native-source -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_REMOTE_1080=1 -DPBS_PS4_NATIVE_SOURCE=1 -DPBS_PS4_LOW_MEMORY=1 tests/ps4/capture/latch_remote.c payload/src/platform/ps4/capture/remote.c
run latch-patch-only -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=0 tests/ps4/diagnostics/latch_patch.c payload/src/platform/ps4/diagnostics/latch_patch.c
run daemon-dump -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4/diagnostics/daemon_dump.c payload/src/platform/ps4/diagnostics/daemon_dump.c
run trampoline -mavx -no-pie tests/ps4/system/trampoline.c
run request tests/ps4/capture/video_request.c
run request-memory5 -DPBS_PS4_MEMORY_COUNT=5 tests/ps4/capture/video_request.c
run privacy tests/ps4/capture/video_privacy.c
run worker-socket -pthread tests/ps4/worker/socket.c payload/src/platform/ps4/worker/client.c payload/src/platform/ps4/worker/remote.c
run worker-shared -pthread tests/ps4/worker/shared_memory.c payload/src/platform/ps4/worker/client.c payload/src/platform/ps4/worker/remote.c payload/src/platform/ps4/worker/entry.S
run worker-boot -DPBS_PS4 tests/ps4/worker/boot.c payload/src/platform/ps4/worker/boot.c payload/src/platform/ps4/worker/remote.c
run ps5-worker tests/ps5/worker_boot.c payload/src/platform/ps5/worker.c payload/src/platform/ps4/worker/remote.c
"$out/ps5-worker" create-timeout
"$out/ps5-worker" handle-read
"$out/ps5-worker" exit-failure
run ps5-capture-fallback tests/ps5/test_capture_fallback.c
run worker-probe -DPBS_PS4 tests/ps4/diagnostics/worker_probe.c payload/src/platform/ps4/diagnostics/worker_probe.c
for worker in 0 1; do
  run control-$worker -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=$worker tests/ps4/system/control_rollback.c payload/src/platform/ps4/system/control.c
  for scenario in init-timeout set-timeout set-negative replaced-process identity-unavailable gate-mismatch; do
    "$out/control-$worker" "$scenario"
  done
done
"$cc" "${flags[@]}" -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_REQUEST=0 tests/ps4/system/control_rollback.c payload/src/platform/ps4/system/control.c -o "$out/control-generic"
"$out/control-generic" generic-profile
"$cc" "${flags[@]}" -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_TEST_FW_VERSION=0x12008001u tests/ps4/system/control_rollback.c payload/src/platform/ps4/system/control.c -o "$out/unsupported"
"$out/unsupported" unsupported-firmware
for worker in 0 1; do
  run remote-$worker -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=$worker -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4/capture/remote_capture.c payload/src/platform/ps4/capture/remote.c
done
run remote-rp24 -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_REMOTE_1080=1 tests/ps4/capture/remote_capture.c payload/src/platform/ps4/capture/remote.c
run remote-admission -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 tests/ps4/capture/remote_capture.c payload/src/platform/ps4/capture/remote.c
run remote-latch -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 tests/ps4/capture/latch_remote.c payload/src/platform/ps4/capture/remote.c
run remote-lowmem -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 -DPBS_PS4_LOW_MEMORY=1 tests/ps4/capture/latch_remote.c payload/src/platform/ps4/capture/remote.c
run remote-budget -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4/capture/latch_remote.c payload/src/platform/ps4/capture/remote.c
run remote-budget-only -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4/capture/budget_remote.c payload/src/platform/ps4/capture/remote.c
run ring -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 tests/ps4/capture/remote_ring.c payload/src/platform/ps4/capture/remote.c
run ring-worker -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 tests/ps4/capture/remote_ring.c payload/src/platform/ps4/capture/remote.c
run ownership tests/ps4/capture/capture_ownership.c payload/src/platform/ps4/capture/capture.c
for scenario in local-term-fail local-init-negative native-activated; do
  "$out/ownership" "$scenario"
done
run lifecycle -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_REMOTE_LIFECYCLE=1 -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4/capture/remote_lifecycle.c payload/src/platform/ps4/capture/remote.c
run lifecycle-release -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_REMOTE_LIFECYCLE=1 -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4/capture/lifecycle_release.c payload/src/platform/ps4/capture/capture.c payload/src/platform/ps4/capture/remote.c
python3 payload/embed_assets.py
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -Ipayload/src tests/media/rtp_packetizer.cpp -o "$out/rtp-packetizer"
"$out/rtp-packetizer"
run http-shutdown -pthread tests/http/shutdown.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-frame-rate -pthread tests/http/frame_rate.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-port-scan -pthread tests/http/port_scan.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-banner -pthread tests/http/banner.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-keyframe-refresh -pthread -Wl,--wrap=clock_gettime tests/http/keyframe_refresh.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-stream-jitter -pthread -Wl,--wrap=send -Wl,--wrap=sendto tests/http/stream_jitter.c payload/src/common/http/server.c payload/src/common/input/controller.c
"$out/http-stream-jitter" --rtp
run http-profiles -pthread tests/http/profiles.c payload/src/common/http/server.c payload/src/common/input/controller.c
run http-controller -pthread tests/http/controller.c payload/src/common/http/server.c payload/src/common/input/controller.c
if command -v node >/dev/null 2>&1; then
  node tests/media/h264_mux_rate.js
  node tests/input/controller.cjs
  node tests/input/lifecycle.cjs
  if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then
    node tests/media/avc_recording.cjs
  fi
  node tests/ps5/test_dashboard_recovery.js
  node tests/ps5/test_videojs_audio.js
  node tests/ps5/test_dashboard_recovery.js --managed
else
  printf 'SKIP browser frame-rate regression: Node.js unavailable.\n'
fi
record_dir=$(mktemp -d "$PWD/$out/recording.XXXXXX")
"$cc" "${flags[@]}" -pthread "-DPBS_RECORD_DIR=\"$record_dir\"" tests/http/record_restart.c payload/src/common/http/server.c payload/src/common/input/controller.c -o "$out/http-record"
"$out/http-record" "$record_dir"
storage_dir=$(mktemp -d "$PWD/$out/storage.XXXXXX")
"$cc" "${flags[@]}" -pthread "-DPBS_RECORD_DIR=\"$storage_dir\"" "-DPBS_RECORD_CONFIG=\"$storage_dir/destination\"" tests/http/recording_management.c payload/src/common/http/server.c payload/src/common/input/controller.c -o "$out/http-recording-management"
"$out/http-recording-management"
python3 -m unittest discover -s tests -p test_verify_ps4_capture.py
python3 -m unittest discover -s tests -p test_inspect_ps4_venc_journal.py
python3 -m unittest discover -s tests -p test_diagnose_ps4_log.py
python3 -m unittest discover -s tests -p test_ps4_firmware_table.py
python3 -m unittest discover -s tests -p test_ps4_offsets.py
python3 -m unittest discover -s tests -p test_ps5_firmware_table.py
python3 -m unittest discover -s tests -p test_store_release.py
# KERNEL_ADDRESS_IMAGE_BASE is a negative intptr_t on hardware: a "> 0" test
# compiles and silently disables discovery. Only pbs_ps4_layout_available() may
# decide whether the kernel image is readable.
if grep -rnE 'KERNEL_ADDRESS_IMAGE_BASE[[:space:]]*[<>]' payload/src; then
  printf 'KERNEL_ADDRESS_IMAGE_BASE compared with a signed relational operator.\n' >&2
  printf 'Use pbs_ps4_layout_available(base, size) from system/layout.h.\n' >&2
  exit 1
fi
# 13.20 has a measured profile: the temporary experimental gate must never come
# back, in the payload, the build plumbing or CI. 4.00 followed the same path on
# 2026-10-29, after its dump was re-checked instruction by instruction.
if grep -rn --exclude=test_ps4_host.sh -E 'PBS_PS5_EXPERIMENTAL_FW(1320|400)' payload .github tools; then
  printf '13.20 and 4.00 are measured; remove the experimental gate and its build switch.\n' >&2
  exit 1
fi
# The selector is the only place where a gated profile can leak into a build:
# compile it and run the selection checks.
"$cc" "${flags[@]}" -Ipayload/src tests/ps5/offset_ranges.c \
  payload/src/platform/ps5/offset/*.c -o "$out/ps5-offset-ranges"
"$out/ps5-offset-ranges"
printf 'Offline regression passed (PS4 payload + PS5 firmware ledger + selector matrix). No console connection made.\n'
