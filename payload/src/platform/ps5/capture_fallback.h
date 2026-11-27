// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* Some firmware/output-state combinations reject an automatic 1440p
 * request. Prefer 1080p on the first open: a rejected attempt can leave the
 * encoder locked even after clearing the callback.
 *
 * FW13.42: a user reported OpenVideo=0x81950008, but did not provide the
 * request parameters or video-out mode. It has the same firmware-native
 * request layout as FW13.60 (verified against both memory dumps). Choose the
 * conservative profile regardless of an available video-out mode value,
 * pending hardware confirmation. Explicit 1440p/4K requests are unaffected.
 * FW10.01 is selected unconditionally because its supplied hardware log shows
 * the automatic 1440p request rejected on the first OpenVideo. The other pairs
 * are directly observed, not decoded HDR/4K enums.
 * FW11.40: OpenVideo=0x81950008 observed on hardware with automatic 1440p.
 * Selected unconditionally pending video-out mode confirmation.
 * FW13.60 also needs the conservative profile when the mode is unavailable
 * or differs from 0x14 only in the bits set by 0x14. */
static inline int pbs_ps5_prefer_1080_before_open(
    uint32_t firmware, uint32_t video_mode) {
  return firmware == 0x10010000u ||
         firmware == 0x11400005u ||
         firmware == 0x13420000u ||
         (firmware == 0x12400009u && video_mode == 0x140u) ||
         (firmware == 0x12700001u && video_mode == 0x144u) ||
         (firmware == 0x13600007u && (video_mode & ~0x14u) == 0u);
}

/* Successful STOP/ClearCallback/Terminate calls prove only that PBS released
 * the public objects it acquired. They do not prove that a rejected OpenVideo
 * released every backend encoder slot. Reopen only after this execution owned
 * and closed a real video handle. */
static inline int pbs_ps5_can_reopen_capture(
    int cleanup_complete, int remote_asserted,
    int opened_session, int server_running) {
  return cleanup_complete && !remote_asserted &&
         opened_session && server_running;
}
