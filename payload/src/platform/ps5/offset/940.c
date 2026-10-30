// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_940 = {
  .firmware = 0x09400008u,
  .avcap2 = {
    .open_video_rva = 0x340u,
    .ipc_command_instruction_rva = 0x3f45u,
    .request_size = 0x2b0u,
    .kind_offset = 0x168u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x4da1du,
    .rate = {
      .offset = 0x90u,
      .size = 0xd0u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x1b0u,
      .size = 0x100u,
      .descriptor_size = 0x40u,
    },
    .width_offset = 0x1d0u,
    .height_offset = 0x1d4u,
    .paste_offset = 0x1f8u,
    .video_mode_offset = 0x280u,
    .mask_mode_offset = 0x284u,
  },
};
