// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_451 = {
  .firmware = 0x04510001u,
  .avcap2 = {
    .open_video_rva = 0x340u,
    .ipc_command_instruction_rva = 0x3ac5u,
    .request_size = 0x230u,
    .kind_offset = 0x100u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x1b7f13u,
    .rate = {
      .offset = 0x90u,
      .size = 0x68u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x148u,
      .size = 0xe8u,
      .descriptor_size = 0x40u,
    },
    .width_offset = 0x168u,
    .height_offset = 0x16cu,
    .paste_offset = 0x190u,
    .video_mode_offset = 0x218u,
    .mask_mode_offset = 0x21cu,
    .mask_span = 0x14u,
  },
};
