// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>
#include "platform/ps5/offset/profile.h"

#ifndef PBS_940_HEIGHT
#define PBS_940_HEIGHT 1080
#endif
#ifndef PBS_940_FPS_MODE
#define PBS_940_FPS_MODE 1
#endif
#if PBS_940_HEIGHT != 720 && PBS_940_HEIGHT != 1080 && PBS_940_HEIGHT != 1440 && PBS_940_HEIGHT != 2160
#error Unsupported diagnostic height
#endif
#if PBS_940_FPS_MODE != 1 && PBS_940_FPS_MODE != 2
#error Unsupported diagnostic frame-rate enum
#endif
#define PBS_940_WIDTH (PBS_940_HEIGHT == 2160 ? 3840 : PBS_940_HEIGHT == 1440 ? 2560 : PBS_940_HEIGHT == 1080 ? 1920 : 1280)
#define PBS_940_FEATURE (PBS_940_HEIGHT == 2160 ? 10u : PBS_940_HEIGHT == 1440 ? 11u : 12u)

static void pbs_940_u32(unsigned char *p, unsigned offset, uint32_t v) { memcpy(p+offset,&v,4); }
static void pbs_940_u64(unsigned char *p, unsigned offset, uint64_t v) { memcpy(p+offset,&v,8); }

/* Same fixed native request for both diagnostics; handle starts at byte 0. */
static void pbs_940_params(unsigned char out[0x2b8], const pbs_ps5_offset_profile *layout,
                           uint32_t video_mode) {
  unsigned char *p=out+8;
  const pbs_ps5_remoteplay_offsets *r=&layout->remoteplay;
  memset(out,0,0x2b8);
  pbs_940_u64(p,0,layout->avcap2.request_size);
  pbs_940_u64(p,8,0x28);
  pbs_940_u32(p,0x14,2); /* usage */
  pbs_940_u32(p,0x18,PBS_940_HEIGHT == 2160 ? 6 : PBS_940_HEIGHT == 1440 ? 5 : PBS_940_HEIGHT == 1080 ? 4 : 3);
  pbs_940_u32(p,0x1c,PBS_940_FPS_MODE); /* enum 2 is a candidate, not verified 120 FPS */
  pbs_940_u32(p,0x28,PBS_940_FEATURE);
  pbs_940_u64(p,r->rate.offset,r->rate.size);
  pbs_940_u64(p,r->rate.offset+8,r->rate.descriptor_size);
  pbs_940_u32(p,r->rate.offset+0x10,30000000);
  pbs_940_u32(p,r->rate.offset+0x14,30000000);
  /* Keep known 60 FPS capacity as an upper bound in the candidate test:
   * only the FPS enum changes in the 1080p comparison. */
  pbs_940_u32(p,r->rate.offset+0x18,500500); /* ceil(30M*1001/60000) */
  pbs_940_u32(p,r->rate.offset+0x1c,35);
  pbs_940_u32(p,r->rate.offset+0x20,51);
  pbs_940_u64(p,r->image.offset,r->image.size);
  pbs_940_u64(p,r->image.offset+8,r->image.descriptor_size);
  pbs_940_u32(p,r->width_offset,PBS_940_WIDTH);
  pbs_940_u32(p,r->height_offset,PBS_940_HEIGHT);
  pbs_940_u64(p,r->paste_offset,0x88);
  memset(p+r->paste_offset+8,0xff,0x80);
  pbs_940_u32(p,r->video_mode_offset,video_mode);
  pbs_940_u32(p,r->mask_mode_offset,2);
  pbs_940_u64(p,r->mask_mode_offset+4,0x10);
  pbs_940_u32(p,r->mask_mode_offset+0xc,1);
  p[r->mask_mode_offset+0x10]=1;
  pbs_940_u64(p,r->mask_mode_offset+0x14,0x10);
  p[r->mask_mode_offset+0x1c]=1;
  pbs_940_u32(p,r->mask_mode_offset+0x20,0x6e);
}
