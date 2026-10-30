// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

extern const pbs_ps5_offset_profile pbs_ps5_offsets_320;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_400;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_451;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_602;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_720;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_840;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_920;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_940;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1001;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1060;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1120;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1140;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1160;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1200;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1240;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1270;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1300;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1320;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1342;
extern const pbs_ps5_offset_profile pbs_ps5_offsets_1360;

const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware) {
  switch (firmware) {
    case 0x03200004u: return &pbs_ps5_offsets_320;
    case 0x04000042u: return &pbs_ps5_offsets_400;
    case 0x04510001u: return &pbs_ps5_offsets_451;
    case 0x06020004u: return &pbs_ps5_offsets_602;
    case 0x07200005u: return &pbs_ps5_offsets_720;
    case 0x08400005u: return &pbs_ps5_offsets_840;
    case 0x09200005u: return &pbs_ps5_offsets_920;
    case 0x09400008u: return &pbs_ps5_offsets_940;
    case 0x10010000u: return &pbs_ps5_offsets_1001;
    case 0x10600006u: return &pbs_ps5_offsets_1060;
    case 0x11200005u: return &pbs_ps5_offsets_1120;
    case 0x11400005u: return &pbs_ps5_offsets_1140;
    case 0x11600005u: return &pbs_ps5_offsets_1160;
    case 0x12000043u: return &pbs_ps5_offsets_1200;
    case 0x12400009u: return &pbs_ps5_offsets_1240;
    case 0x12700001u: return &pbs_ps5_offsets_1270;
    case 0x13000040u: return &pbs_ps5_offsets_1300;
    case 0x13200006u: return &pbs_ps5_offsets_1320;
    case 0x13420000u: return &pbs_ps5_offsets_1342;
    case 0x13600007u: return &pbs_ps5_offsets_1360;
    default: break;
  }

  /* Exact profiles take precedence. Reuse geometry only inside the
   * audited family bounds, including other builds of each release. */
  const uint32_t version = firmware & 0xffff0000u;
  /* Only catalogued public releases may use a family fallback. */
  switch (version) {
    case 0x06020000u:
    case 0x06500000u:
    case 0x07000000u:
    case 0x07010000u:
    case 0x07200000u:
    case 0x07400000u:
    case 0x07600000u:
    case 0x07610000u:
    case 0x08000000u:
    case 0x08200000u:
    case 0x08400000u:
    case 0x09200000u:
    case 0x09400000u:
    case 0x10010000u:
    case 0x10200000u:
    case 0x10400000u:
    case 0x10600000u:
    case 0x11400000u:
    case 0x11600000u:
    case 0x12000000u:
    case 0x12020000u:
    case 0x12200000u:
    case 0x12400000u:
    case 0x12600000u:
    case 0x12700000u:
    case 0x13400000u:
    case 0x13420000u:
    case 0x13600000u:
      break;
    default: return 0;
  }
  if (version >= 0x06020000u && version <= 0x08400000u)
    return &pbs_ps5_offsets_602;
  if (version >= 0x09200000u && version <= 0x09400000u)
    return &pbs_ps5_offsets_920;
  if (version >= 0x10010000u && version <= 0x10600000u)
    return &pbs_ps5_offsets_1001;
  if (version >= 0x11400000u && version <= 0x12700000u)
    return &pbs_ps5_offsets_1160;
  if (version >= 0x13400000u && version <= 0x13600000u)
    return &pbs_ps5_offsets_1360;
  return 0;
}
