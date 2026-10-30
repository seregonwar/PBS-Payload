// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* The capture feature and size are one profile. Never reuse the 2160p
 * feature when reopening at 1080p (or vice versa). */
typedef struct { int width, height; unsigned feature; } pbs_capture_profile;
static inline int pbs_capture_profile_for_height(int height, pbs_capture_profile *out) {
  pbs_capture_profile p;
  switch (height) {
    case 720: p = (pbs_capture_profile){1280, 720, 12}; break;
    case 1080: p = (pbs_capture_profile){1920, 1080, 12}; break;
    case 1440: p = (pbs_capture_profile){2560, 1440, 11}; break;
    case 2160: p = (pbs_capture_profile){3840, 2160, 10}; break;
    default: return -1;
  }
  if (out) *out = p;
  return 0;
}
/* The highest capture profile a console video output mode can drive. 0 means
 * "not measured". Measured pairs: 0x10 rejects 1440p/2160p, 0x138 accepts
 * 1440p, 0x13c rejects 1440p while accepting 1080p (so it is a 2160p mode). */
static inline int pbs_capture_ceiling_for_video_mode(uint32_t mode) {
  switch (mode) {
    case 0x10u: return 1080;
    case 0x138u: return 1440;
    case 0x13cu: return 2160;
    default: return 0;
  }
}

/* Whether the firmware refuses this height at the given output mode. Above the
 * output ceiling a reopen is always rejected; the 1440p feature is also
 * rejected by a 2160p output, which only accepts its own size and 1080p. */
static inline int pbs_capture_refused_for_video_mode(uint32_t mode, int height) {
  int ceiling = pbs_capture_ceiling_for_video_mode(mode);
  if (ceiling && height > ceiling) return 1;
  return mode == 0x13cu && height == 1440;
}
