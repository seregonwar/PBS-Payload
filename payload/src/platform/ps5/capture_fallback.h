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
 * The other pairs are directly observed, not decoded HDR/4K enums. */
static inline int pbs_ps5_prefer_1080_before_open(
    uint32_t firmware, uint32_t video_mode) {
  return firmware == 0x13420000u ||
         (firmware == 0x12700001u && video_mode == 0x144u) ||
         (firmware == 0x13600007u && video_mode == 0x14u);
}

/* An actual 0x81950008 rejection of an automatic 1440p request warrants a
 * retry at 1080p, after successful cleanup. The firmware's opaque video mode
 * is not a prerequisite; other errors are deliberately not conflated. */
static inline int pbs_ps5_should_fallback_1080(
    uint32_t firmware, uint32_t open_result, uint32_t video_mode,
    int width, int height) {
  (void)firmware;
  (void)video_mode;
  return width == 2560 && height == 1440 &&
         open_result == 0x81950008u;
}

/* The capture height the console's own video output implies. Only measured
 * pairs are encoded: a 2160p output (0x13c) rejects the 1440p feature with
 * 0x81950008 while accepting 1080p, and a 1440p output (0x138) accepts 1440p.
 * 0 means "not measured for this mode". */
static inline int pbs_ps5_profile_height_for_video_mode(uint32_t video_mode) {
  switch (video_mode) {
    case 0x10u: return 1080;
    case 0x138u: return 1440;
    case 0x13cu: return 2160;
    default: return 0;
  }
}

/* An automatic open must not request a profile this output mode is known to
 * reject: a rejected open still has to unwind scan-in and the arbitrator
 * callback, and that repair path runs on the host's main thread. Returns the
 * height to open instead, or 0 when the request can go ahead unchanged. */
static inline int pbs_ps5_avoid_rejected_open(
    uint32_t video_mode, int height, int automatic) {
  if (!automatic) return 0;
  int natural = pbs_ps5_profile_height_for_video_mode(video_mode);
  return natural && height == 1440 && natural == 2160 ? natural : 0;
}

/* Height to retry after a cleanly rejected automatic open, or 0 when the
 * rejection is not one this policy covers. A rejected 1440p request retries at
 * the profile the output drives (1080p when the mode is not measured); a
 * rejected 4K request retries 1080p, the only smaller profile a 2160p output
 * has been observed to accept, so a console that refuses 4K still streams. */
static inline int pbs_ps5_retry_height(
    uint32_t open_result, uint32_t video_mode, int width, int height) {
  if (open_result != 0x81950008u) return 0;
  if (width == 2560 && height == 1440) {
    int natural = pbs_ps5_profile_height_for_video_mode(video_mode);
    return natural ? natural : 1080;
  }
  if (width == 3840 && height == 2160) return 1080;
  return 0;
}