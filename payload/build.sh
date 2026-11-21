#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
platform=${1:-ps5}
kind=${2:-stream}
config=${PBS_BUILD_CONFIG:-Release}
case "$config" in
  Release) optimization=-O2; debug_flags=() ;;
  Debug) optimization=-O0; debug_flags=(-g) ;;
  *) echo 'PBS_BUILD_CONFIG must be Debug or Release' >&2; exit 2 ;;
esac
# The owner's banner is branding, not code: opt in, otherwise it is not
# embedded and /banner.png answers 404 (no logo in the viewer).
web_logo=${PBS_WEB_LOGO:-0}
ps5_controller=${PBS_PS5_CONTROLLER:-1}
if [[ $ps5_controller != 0 && $ps5_controller != 1 ]]; then
  echo 'PBS_PS5_CONTROLLER must be 0 or 1 (default 1)' >&2; exit 2
fi
if [[ $web_logo != 0 && $web_logo != 1 ]]; then
  echo 'PBS_WEB_LOGO must be 0 or 1 (1 embeds web/banner.png, default 0)' >&2; exit 2
fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
case "$platform" in
  ps4) sdk=${PS4_PAYLOAD_SDK:?Set PS4_PAYLOAD_SDK}; compiler="$sdk/bin/orbis-clang" ;;
  ps5) sdk=${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK}; compiler="$sdk/bin/prospero-clang" ;;
  *) echo 'Usage: build.sh ps4|ps5 [stream|smoke|probe|abi-probe|dump]' >&2; exit 2 ;;
esac
includes=(-I"$root/src" -I"$root/build/generated")
out="$root/build/$platform"
mkdir -p "$out"
if [[ ${PBS_PS4_WORKER_PROBE:-0} == 1 && $kind == smoke && $platform == ps4 ]]; then
  kind=worker-probe
fi
if [[ $kind == worker-probe && $platform == ps4 ]]; then
  "$compiler" "${includes[@]}" -O2 "${debug_flags[@]}" -Wall -Wextra -Wno-unused-parameter -DPBS_PS4 \
    "$root/src/platform/ps4/diagnostics/worker_probe_main.c" "$root/src/platform/ps4/diagnostics/worker_probe.c" \
    "$root/src/platform/ps4/worker/boot.c" "$root/src/platform/ps4/worker/client.c" \
    "$root/src/platform/ps4/worker/remote.c" "$root/src/common/process/inject.c" \
    "$root/src/platform/ps4/system/credentials.c" "$root/src/platform/ps4/system/control.c" \
    -o "$out/pbs-worker-probe.elf"
  python3 "$root/../tools/verify_ps4_worker_blob.py" "$out/pbs-worker-probe.elf"
elif [[ $kind == probe && $platform == ps4 ]]; then
  "$compiler" "${includes[@]}" -O2 "${debug_flags[@]}" -Wall -Wextra -Werror "$root/src/platform/ps4/diagnostics/probe.c" -o "$out/pbs-probe.elf"
elif [[ $kind == abi-probe && $platform == ps4 ]]; then
  "$compiler" "${includes[@]}" -O2 "${debug_flags[@]}" -Wall -Wextra -Werror -DPBS_PS4_ABI_PROBE=1 \
    "$root/src/platform/ps4/diagnostics/probe.c" -o "$out/pbs-abi-probe.elf"
elif [[ $kind == dump && $platform == ps4 ]]; then
  # Read-only offset report. No capture, no patch, no module image here: run
  # the probe payload for the module ELFs and tools/ps4_offsets.py offline.
  # The payload sources themselves live in tools/dump/ps4: this is a
  # measurement tool, not part of the capture payload.
  "$compiler" "${includes[@]}" -I"$root/../tools" -O2 "${debug_flags[@]}" -Wall -Wextra -Werror \
    "$root/../tools/dump/ps4/dump_main.c" "$root/../tools/dump/ps4/offset_report.c" \
    "$root/../tools/dump/ps4/kernel_image.c" \
    "$root/src/platform/ps4/system/layout.c" -o "$out/pbs-dump.elf"
elif [[ $kind == stream || ( $kind == smoke && $platform == ps4 ) ]]; then
  if [[ $web_logo == 1 ]]; then python3 "$root/embed_assets.py" --logo
  else python3 "$root/embed_assets.py"; fi
  if [[ $platform == ps4 ]]; then
    capture_height=${PBS_PS4_CAPTURE_HEIGHT:-720}
    if [[ $capture_height != 720 && $capture_height != 1080 ]]; then
      echo 'PBS_PS4_CAPTURE_HEIGHT must be 720 or 1080' >&2; exit 2
    fi
    capture_fps=${PBS_PS4_CAPTURE_FPS:-60}
    if [[ $capture_fps != 30 && $capture_fps != 60 ]]; then
      echo 'PBS_PS4_CAPTURE_FPS must be 30 or 60' >&2; exit 2
    fi
    if [[ $capture_fps == 30 && $capture_height != 1080 ]]; then
      echo 'The experimental 30fps route requires 1080p' >&2; exit 2
    fi
    capture_usage=${PBS_PS4_CAPTURE_USAGE:-0}
    image_paste=${PBS_PS4_IMAGE_PASTE:-0}
    read_wait=${PBS_PS4_READ_WAIT_EVENTS:-0}
    # The supported stream path uses the worker and runtime-discovered layout.
    # Keep the local client available only for bounded diagnostic smokes.
    if [[ $kind == stream || ( $kind == smoke && $capture_usage == 0 ) ]]; then
      remote_avcap=${PBS_PS4_REMOTE_AVCAP:-1}
    else
      remote_avcap=${PBS_PS4_REMOTE_AVCAP:-0}
    fi
    lifecycle=${PBS_PS4_REMOTE_LIFECYCLE:-0}
    idle_run=${PBS_PS4_REMOTE_IDLE_RUN:-0}
    if [[ $idle_run != 0 && $idle_run != 1 ]]; then
      echo 'PBS_PS4_REMOTE_IDLE_RUN must be 0 or 1' >&2; exit 2
    fi
    if [[ $idle_run == 1 && ( $kind != smoke || $remote_avcap != 1 ) ]]; then
      echo 'PBS_PS4_REMOTE_IDLE_RUN=1 requires a remote smoke build' >&2; exit 2
    fi
    # The capture runs on a dedicated worker thread inside SceRemotePlay so the
    # daemon is never frozen during the session. Default on for smoke builds,
    # off for production until hardware validation is complete.
    worker_boot=${PBS_PS4_WORKER_BOOT:--1}
    if [[ $worker_boot == -1 ]]; then
      worker_boot=0
      if [[ $remote_avcap == 1 ]]; then worker_boot=1; fi
    fi
    if [[ $worker_boot != 0 && $worker_boot != 1 ]]; then
      echo 'PBS_PS4_WORKER_BOOT must be 0 or 1' >&2; exit 2
    fi
    if [[ $worker_boot == 1 && $remote_avcap != 1 ]]; then
      echo 'PBS_PS4_WORKER_BOOT=1 requires PBS_PS4_REMOTE_AVCAP=1' >&2; exit 2
    fi
    if [[ $worker_boot == 1 && $read_wait == 1 ]]; then
      echo 'Exported worker ReadVideo requires nonblocking reads; disable PBS_PS4_READ_WAIT_EVENTS' >&2; exit 2
    fi
    if [[ $kind == stream && $remote_avcap == 1 && $worker_boot != 1 ]]; then
      echo 'Remote streaming requires the dedicated worker; use smoke for ptrace experiments' >&2; exit 2
    fi
    if [[ $capture_usage != 0 && $capture_usage != 1 ]]; then
      echo 'PBS_PS4_CAPTURE_USAGE must be 0 or 1' >&2; exit 2
    fi
    if [[ $image_paste != 0 && $image_paste != 1 ]]; then
      echo 'PBS_PS4_IMAGE_PASTE must be 0 or 1' >&2; exit 2
    fi
    if [[ $read_wait != 0 && $read_wait != 1 ]]; then
      echo 'PBS_PS4_READ_WAIT_EVENTS must be 0 or 1' >&2; exit 2
    fi
    if [[ $remote_avcap != 0 && $remote_avcap != 1 ]]; then
      echo 'PBS_PS4_REMOTE_AVCAP must be 0 or 1' >&2; exit 2
    fi
    if [[ $lifecycle != 0 && $lifecycle != 1 ]]; then
      echo 'PBS_PS4_REMOTE_LIFECYCLE must be 0 or 1' >&2; exit 2
    fi
    if [[ $lifecycle == 1 && $remote_avcap == 0 ]]; then
      echo 'PBS_PS4_REMOTE_LIFECYCLE=1 requires PBS_PS4_REMOTE_AVCAP=1' >&2; exit 2
    fi
    defines=(-DPBS_PS4 "-DPBS_PS4_CAPTURE_USAGE=$capture_usage" "-DPBS_PS4_IMAGE_PASTE=$image_paste" "-DPBS_PS4_READ_WAIT_EVENTS=$read_wait" "-DPBS_PS4_REMOTE_AVCAP=$remote_avcap" "-DPBS_PS4_REMOTE_LIFECYCLE=$lifecycle")
    if [[ $capture_fps == 30 && ( $capture_usage != 0 || $image_paste != 0 || $remote_avcap != 1 ) ]]; then
      echo 'The generic 1080p30 experiment requires remote usage=0 without ImagePaste' >&2; exit 2
    fi
    remote_1080=${PBS_PS4_REMOTE_1080:-0}
    if [[ $remote_1080 != 0 && $remote_1080 != 1 ]]; then
      echo 'PBS_PS4_REMOTE_1080 must be 0 or 1' >&2; exit 2
    fi
    remote_request=3
    if [[ $capture_fps == 30 && $remote_1080 == 0 ]]; then remote_request=0; fi
    admission_probe=${PBS_PS4_ADMISSION_PROBE:-0}
    latch_bypass=${PBS_PS4_LATCH_BYPASS:-0}
    if [[ $latch_bypass != 0 && $latch_bypass != 1 ]]; then
      echo 'PBS_PS4_LATCH_BYPASS must be 0 or 1' >&2; exit 2
    fi
    budget_unlock=${PBS_PS4_BUDGET_UNLOCK:-0}
    if [[ $budget_unlock != 0 && $budget_unlock != 1 ]]; then
      echo 'PBS_PS4_BUDGET_UNLOCK must be 0 or 1' >&2; exit 2
    fi
    low_memory=${PBS_PS4_LOW_MEMORY:-0}
    if [[ $low_memory != 0 && $low_memory != 1 ]]; then
      echo 'PBS_PS4_LOW_MEMORY must be 0 or 1' >&2; exit 2
    fi
    if [[ $remote_1080 == 1 ]]; then
      low_memory=1
      if [[ $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $remote_avcap != 1 || $worker_boot != 1 || $capture_usage != 0 || $image_paste != 0 || $latch_bypass != 0 || $budget_unlock != 0 || $admission_probe != 0 || $lifecycle != 0 || $idle_run != 0 ]]; then
        echo 'Remote1080 requires a normal worker 1080p30 smoke without latch/budget/admission/lifecycle/idle flags' >&2; exit 2
      fi
    elif [[ $low_memory == 1 && ( $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $latch_bypass != 1 || $budget_unlock != 0 ) ]]; then
      echo 'Low-memory 1080p30 requires a latch smoke with the Neo budget flag disabled' >&2; exit 2
    fi
    venc_journal=${PBS_PS4_VENC_JOURNAL:-0}
    if [[ $venc_journal != 0 && $venc_journal != 1 ]]; then
      echo 'PBS_PS4_VENC_JOURNAL must be 0 or 1' >&2; exit 2
    fi
    if [[ $venc_journal == 1 && $remote_1080 != 1 ]]; then
      echo 'Venc printf journal requires the bounded Remote1080 experiment' >&2; exit 2
    fi
    native_source=${PBS_PS4_NATIVE_SOURCE:-0}
    if [[ $native_source != 0 && $native_source != 1 ]]; then
      echo 'PBS_PS4_NATIVE_SOURCE must be 0 or 1' >&2; exit 2
    fi
    if [[ $native_source == 1 && ( $remote_1080 != 1 || $venc_journal != 0 ) ]]; then
      echo 'Native source requires Remote1080 without journal' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_NATIVE_SOURCE=$native_source")
    memory_count=${PBS_PS4_MEMORY_COUNT:-3}
    if [[ $memory_count != 3 && $memory_count != 4 && $memory_count != 5 ]]; then
      echo 'PBS_PS4_MEMORY_COUNT must be 3, 4 or 5' >&2; exit 2
    fi
    if [[ $memory_count != 3 && $low_memory != 1 ]]; then
      echo 'Nondefault memory count requires the bounded low-memory experiment' >&2; exit 2
    fi
    daemon_dump=${PBS_PS4_DAEMON_DUMP:-0}
    if [[ $daemon_dump != 0 && $daemon_dump != 1 ]]; then
      echo 'PBS_PS4_DAEMON_DUMP must be 0 or 1' >&2; exit 2
    fi
    encoder_state=${PBS_PS4_ENCODER_STATE:-0}
    if [[ $encoder_state != 0 && $encoder_state != 1 ]]; then
      echo 'PBS_PS4_ENCODER_STATE must be 0 or 1' >&2; exit 2
    fi
    if [[ $encoder_state == 1 ]]; then
      if [[ $kind != smoke || $capture_height != 720 || $capture_fps != 60 || $remote_avcap != 1 || $worker_boot != 1 || $capture_usage != 0 || $image_paste != 0 || $lifecycle != 0 || $idle_run != 0 || $low_memory != 0 || $admission_probe != 0 || $latch_bypass != 0 || $budget_unlock != 0 || $venc_journal != 0 || $native_source != 0 || $daemon_dump != 0 ]]; then
        echo 'PBS_PS4_ENCODER_STATE=1 requires the standard working 720p60 worker smoke without other experiments' >&2; exit 2
      fi
    fi
    defines+=("-DPBS_PS4_ENCODER_STATE=$encoder_state")
    if [[ $latch_bypass == 1 ]]; then admission_probe=1; fi
    if [[ $budget_unlock == 1 ]]; then admission_probe=1; fi
    if [[ $admission_probe != 0 && $admission_probe != 1 ]]; then
      echo 'PBS_PS4_ADMISSION_PROBE must be 0 or 1' >&2; exit 2
    fi
    if [[ $admission_probe == 1 && ( $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $remote_avcap != 1 || $worker_boot != 1 || $lifecycle != 0 || $idle_run != 0 ) ]]; then
      echo 'Admission diagnostics require a worker 1080p30 smoke without lifecycle/idle experiments' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_ADMISSION_PROBE=$admission_probe")
    defines+=("-DPBS_PS4_LATCH_BYPASS=$latch_bypass")
    defines+=("-DPBS_PS4_BUDGET_UNLOCK=$budget_unlock")
    defines+=("-DPBS_PS4_LOW_MEMORY=$low_memory" "-DPBS_PS4_REMOTE_1080=$remote_1080" "-DPBS_PS4_MEMORY_COUNT=$memory_count")
    defines+=("-DPBS_PS4_DAEMON_DUMP=$daemon_dump" "-DPBS_PS4_VENC_JOURNAL=$venc_journal")
    defines+=("-DPBS_PS4_CAPTURE_HEIGHT=$capture_height" "-DPBS_PS4_CAPTURE_FPS=$capture_fps" "-DPBS_PS4_REMOTE_REQUEST=$remote_request")
    defines+=("-DPBS_PS4_REMOTE_IDLE_RUN=$idle_run")
    defines+=("-DPBS_PS4_WORKER_BOOT=$worker_boot")
    # Running worker uses exported ReadVideo; no private ring/readiness ABI.
    defines+=("-DPBS_PS4_EXPORTED_READ=$worker_boot")
    dynamic_layout=$worker_boot
    # Firmware-specific diagnostic experiments remain in their explicit ABI.
    if [[ $admission_probe == 1 || $latch_bypass == 1 || $budget_unlock == 1 || $low_memory == 1 || $encoder_state == 1 || $venc_journal == 1 || $native_source == 1 || $daemon_dump == 1 ]]; then dynamic_layout=0; fi
    defines+=("-DPBS_PS4_DYNAMIC_LAYOUT=$dynamic_layout")
    if [[ $kind == stream && $dynamic_layout != 1 ]]; then
      echo 'PS4 stream requires runtime layout discovery; legacy firmware offsets are diagnostic-only' >&2; exit 2
    fi
    sources=("$root/src/platform/ps4/main.c" "$root/src/platform/ps4/capture/capture.c")
    if [[ $remote_avcap == 1 ]]; then
      sources+=("$root/src/platform/ps4/capture/remote.c" "$root/src/platform/ps4/worker/client.c" "$root/src/platform/ps4/worker/remote.c" "$root/src/platform/ps4/worker/boot.c")
      if [[ $worker_boot == 1 ]]; then sources+=("$root/src/platform/ps4/capture/export_reader.c"); fi
    fi
    sources+=("$root/src/platform/ps4/system/control.c" "$root/src/common/process/inject.c" "$root/src/common/http/server.c" "$root/src/common/input/controller.c" "$root/src/common/diagnostics/problem_log.c" "$root/src/platform/ps4/system/credentials.c")
    if [[ $dynamic_layout == 1 ]]; then sources+=("$root/src/platform/ps4/system/layout.c" "$root/src/platform/ps4/system/layout_runtime.c"); fi
    if [[ $admission_probe == 1 || $low_memory == 1 || $encoder_state == 1 ]]; then
      sources+=("$root/src/platform/ps4/diagnostics/snapshot.c" "$root/src/platform/ps4/diagnostics/admission.c")
    fi
    if [[ $latch_bypass == 1 || $budget_unlock == 1 || $native_source == 1 ]]; then sources+=("$root/src/platform/ps4/diagnostics/latch_patch.c"); fi
    if [[ $venc_journal == 1 ]]; then sources+=("$root/src/platform/ps4/diagnostics/venc_journal.c"); fi
    if [[ $daemon_dump == 1 ]]; then sources+=("$root/src/platform/ps4/diagnostics/daemon_dump.c"); fi
  else
    sources=("$root/src/platform/ps5/main.c" "$root/src/platform/ps5/offset_dump.c"
      "$root/src/platform/ps5/module_dump.c" "$root/src/platform/ps5/request_template.c"
      "$root/src/platform/ps5/worker.c"
      "$root/src/platform/ps4/worker/client.c" "$root/src/platform/ps4/worker/remote.c"
      "$root/src/common/process/inject.c"
      "$root/src/common/http/server.c" "$root/src/common/input/controller.c"
      "$root/src/common/config/config.c" "$root/src/common/log/log.c")
    for offset_source in "$root"/src/platform/ps5/offset/*.c; do
      sources+=("$offset_source")
    done
    defines=("-DPBS_PS5_CONTROLLER=$ps5_controller")
    if [[ $ps5_controller == 1 ]]; then
      sources+=("$root/src/platform/ps5/controller.c")
    fi
  fi
  filename=pbs.elf
  if [[ $platform == ps4 && $kind == stream && $remote_avcap == 1 ]]; then
    filename=pbs-remote-stream.elf
  fi
  if [[ $kind == smoke ]]; then
    capture_seconds=${PBS_PS4_CAPTURE_SECONDS:-10}
    if [[ ! $capture_seconds =~ ^[1-9][0-9]*$ ]] || (( capture_seconds > 3600 )); then
      echo 'PBS_PS4_CAPTURE_SECONDS must be 1..3600' >&2; exit 2
    fi
    if [[ $platform == ps4 && $low_memory == 1 ]] && (( capture_seconds > 10 )); then
      echo 'Low-memory 1080p experiment is limited to 10 seconds' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_CAPTURE_SECONDS=$capture_seconds" -DPBS_PS4_OBSERVE_REMOTEPLAY_STATUS=1)
    observers=${PBS_PS4_SMOKE_OBSERVERS:-0}
    if [[ $observers != 0 && $observers != 1 ]]; then
      echo 'PBS_PS4_SMOKE_OBSERVERS must be 0 or 1' >&2; exit 2
    fi
    # FW11 hardware smoke crashed in the optional AvSetting/VideoOut
    # observation phase before Init/Open. Keep its ABI-dependent dlopen and
    # GetCurrentOutputMode2_ out of the normal capture path; preserve this
    # instrumentation only in a separately named diagnostic ELF.
    if [[ $observers == 1 ]]; then
      defines+=(-DPBS_PS4_OBSERVE_VIDEO_MODE=1 -DPBS_PS4_OBSERVE_VIDEOOUT_BUS=1)
    fi
    source_wait=${PBS_PS4_SMOKE_WAIT_SOURCE:-0}
    if [[ $source_wait != 0 && $source_wait != 1 ]]; then
      echo 'PBS_PS4_SMOKE_WAIT_SOURCE must be 0 or 1' >&2; exit 2
    fi
    if [[ $source_wait == 1 ]]; then
      if [[ $observers != 1 ]]; then
        echo 'PBS_PS4_SMOKE_WAIT_SOURCE=1 requires PBS_PS4_SMOKE_OBSERVERS=1' >&2; exit 2
      fi
      defines+=(-DPBS_PS4_SMOKE_WAIT_SOURCE=1)
    fi
    filename=pbs-smoke.elf
    if [[ $capture_seconds != 10 ]]; then filename=pbs-smoke-${capture_seconds}s.elf; fi
  fi
  if [[ $platform == ps4 && $capture_height == 1080 ]]; then
    filename=${filename%.elf}-1080p.elf
  fi
  if [[ $platform == ps4 && $capture_fps == 30 ]]; then
    filename=${filename%.elf}-30fps.elf
  fi
  if [[ $platform == ps4 && $capture_usage == 1 ]]; then
    filename=${filename%.elf}-usage1.elf
  fi
  if [[ $platform == ps4 && $image_paste == 1 ]]; then
    filename=${filename%.elf}-imagepaste.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $observers == 1 ]]; then
    filename=${filename%.elf}-observers.elf
  fi
  if [[ $platform == ps4 && $read_wait == 1 ]]; then
    filename=${filename%.elf}-nativewait.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $capture_usage == 0 && $remote_avcap == 0 ]]; then
    filename=${filename%.elf}-local.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $lifecycle == 1 ]]; then
    filename=${filename%.elf}-lifecycle.elf
  fi
  if [[ $platform == ps4 && $idle_run == 1 ]]; then
    filename=${filename%.elf}-idle-run.elf
  fi
  if [[ $platform == ps4 && $admission_probe == 1 ]]; then
    filename=${filename%.elf}-admission.elf
  fi
  if [[ $platform == ps4 && $latch_bypass == 1 ]]; then
    filename=${filename%.elf}-latch.elf
  fi
  if [[ $platform == ps4 && $budget_unlock == 1 ]]; then
    filename=${filename%.elf}-budget.elf
  fi
  if [[ $platform == ps4 && $daemon_dump == 1 ]]; then
    filename=${filename%.elf}-daemondump.elf
  fi
  if [[ $platform == ps4 && $encoder_state == 1 ]]; then
    filename=${filename%.elf}-encoderstate.elf
  fi
  if [[ $platform == ps4 && $low_memory == 1 ]]; then
    filename=${filename%.elf}-lowmem.elf
  fi
  if [[ $platform == ps4 && $remote_1080 == 1 ]]; then
    filename=${filename%.elf}-rp24.elf
  fi
  if [[ $platform == ps4 && $memory_count != 3 ]]; then
    filename=${filename%.elf}-memory${memory_count}.elf
  fi
  if [[ $platform == ps4 && $venc_journal == 1 ]]; then
    filename=${filename%.elf}-vencjournal.elf
  fi
  if [[ $platform == ps4 && $native_source == 1 ]]; then
    filename=${filename%.elf}-nativesource.elf
  fi
  "$compiler" "${includes[@]}" "$optimization" "${debug_flags[@]}" -Wall -Wextra -Wno-unused-parameter "${defines[@]}" "${sources[@]}" -o "$out/$filename"
  if [[ $platform == ps5 ]]; then
    python3 "$root/../tools/verify_ps5_elf.py" "$out/$filename"
  fi
  if [[ $platform == ps4 && $remote_avcap == 1 ]]; then
    python3 "$root/../tools/verify_ps4_worker_blob.py" "$out/$filename"
  fi
else
  echo 'Unsupported platform/build combination' >&2; exit 2
fi
