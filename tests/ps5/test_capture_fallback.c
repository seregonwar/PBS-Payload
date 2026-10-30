// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_fallback.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  /* Bypass a known-rejected 1440p configuration before touching the encoder. */
  assert(pbs_ps5_prefer_1080_before_open(0x12700001u, 0x144u));
  assert(pbs_ps5_prefer_1080_before_open(0x13600007u, 0x14u));
  /* 13.42 lacks a video-mode log: choose a conservative automatic startup
   * for either possible value, including an unavailable preflight query. */
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0u));
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0x14u));
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13600007u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12700001u, 0x14u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12400009u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13400001u, 0x14u));

  /* For all firmware, reject a failing automatic 1440p profile once and
   * retry 1080p after cleanup. Do not interpret mode values as HDR enums. */
  assert(pbs_ps5_should_fallback_1080(0x13420000u, 0x81950008u, 0x14u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x144u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12400009u, 0x81950008u, 0x144u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x06020004u, 0x81950008u, 0x0u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950001u, 0x14u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0u, 0x14u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 1920, 1080));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 3840, 2160));
  /* Measured output modes and the capture height they drive. */
  assert(pbs_ps5_profile_height_for_video_mode(0x10u) == 1080);
  assert(pbs_ps5_profile_height_for_video_mode(0x138u) == 1440);
  assert(pbs_ps5_profile_height_for_video_mode(0x13cu) == 2160);
  assert(pbs_ps5_profile_height_for_video_mode(0x144u) == 0);
  assert(pbs_ps5_profile_height_for_video_mode(0u) == 0);
  /* A 2160p output rejects the 1440p feature: switch before opening, but only
   * for an automatic start (an explicit viewer request stays explicit). */
  assert(pbs_ps5_avoid_rejected_open(0x13cu, 1440, 1) == 2160);
  assert(pbs_ps5_avoid_rejected_open(0x13cu, 1440, 0) == 0);
  assert(pbs_ps5_avoid_rejected_open(0x138u, 1440, 1) == 0);
  assert(pbs_ps5_avoid_rejected_open(0x10u, 1440, 1) == 0);
  assert(pbs_ps5_avoid_rejected_open(0x144u, 1440, 1) == 0);
  assert(pbs_ps5_avoid_rejected_open(0x13cu, 1080, 1) == 0);
  /* After a clean rejection, retry the profile the output drives; a rejected
   * 4K open retries 1080p so the console still streams. */
  assert(pbs_ps5_retry_height(0x81950008u, 0x13cu, 2560, 1440) == 2160);
  assert(pbs_ps5_retry_height(0x81950008u, 0x138u, 2560, 1440) == 1440);
  assert(pbs_ps5_retry_height(0x81950008u, 0x10u, 2560, 1440) == 1080);
  assert(pbs_ps5_retry_height(0x81950008u, 0u, 2560, 1440) == 1080);
  assert(pbs_ps5_retry_height(0x81950008u, 0x13cu, 3840, 2160) == 1080);
  assert(pbs_ps5_retry_height(0x81950001u, 0x13cu, 3840, 2160) == 0);
  assert(pbs_ps5_retry_height(0u, 0x13cu, 2560, 1440) == 0);
  assert(pbs_ps5_retry_height(0x81950008u, 0x13cu, 1920, 1080) == 0);
  puts("PASS: FW13.42 conservative auto startup, FW13.60/12.70 proactive 1080p, bounded fallback, output-mode profiles");
  return 0;
}
