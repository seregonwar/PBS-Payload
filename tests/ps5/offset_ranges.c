// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"
#include <assert.h>
#include <stdio.h>

static void check(uint32_t firmware, uint32_t expected, uint32_t size) {
  const pbs_ps5_offset_profile *p = pbs_ps5_find_offsets(firmware);
  if (!expected) { assert(!p); return; }
  assert(p && p->firmware == expected && p->avcap2.request_size == size);
}

int main(void) {
  /* Exact entries must win even when another profile describes the family. */
  static const uint32_t exact[][2] = {
    {0x03200004,0x218}, {0x06020004,0x2a8}, {0x07200005,0x2a8},
    {0x08400005,0x2a8}, {0x09200005,0x2b0}, {0x09400008,0x2b0},
    {0x10010000,0x2b8}, {0x10600006,0x2b8}, {0x11200005,0x2e0},
    {0x11400005,0x2e0}, {0x11600005,0x2e0}, {0x12000043,0x2e0},
    {0x12700001,0x2e0}, {0x13000040,0x2e0}, {0x13420000,0x2e8},
    {0x13600007,0x2e8}
  };
  for (unsigned i = 0; i < sizeof exact / sizeof exact[0]; ++i)
    check(exact[i][0],exact[i][0],exact[i][1]);

  /* Release endpoints include every build revision, not just the dumped ID. */
  static const uint32_t ranges[][4] = {
    {0x06020000,0x08400000,0x06020004,0x2a8},
    {0x09200000,0x09400000,0x09200005,0x2b0},
    {0x10010000,0x10600000,0x10010000,0x2b8},
    {0x11400000,0x12700000,0x11600005,0x2e0},
    {0x13400000,0x13600000,0x13600007,0x2e8}
  };
  for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
    const uint32_t *r = ranges[i];
    check(r[0],r[2],r[3]);
    check(r[0] | 0xffff,r[2],r[3]);
    check(r[1],r[2],r[3]);
    check(r[1] | 0xffff,r[2],r[3]);
    check(r[0] - 1,0,0);
    check(r[1] + 0x10000,0,0);
  }
  check(0x07000009,0x06020004,0x2a8);
  check(0x06500009,0x06020004,0x2a8);
  check(0x07610009,0x06020004,0x2a8);
  check(0x08200009,0x06020004,0x2a8);
  check(0x10200010,0x10010000,0x2b8);
  check(0x10400010,0x10010000,0x2b8);
  check(0x12020001,0x11600005,0x2e0);
  check(0x12200001,0x11600005,0x2e0);
  check(0x12600001,0x11600005,0x2e0);
  check(0x13420001,0x13600007,0x2e8);
  /* 13.00 is exact-only: its geometry (0x2e0) differs from the 13.40/13.60
   * family (0x2e8), so no range may stand in for another 13.00 build. */
  check(0x13000041,0,0);
  /* 13.20 is exact-only too: the 2026-10-28 dump measured build 0x13200006
   * (request 0x2e0), so no other 13.20 build may borrow that geometry. */
  check(0x13200006,0x13200006,0x2e0);
  check(0x13200000,0,0);
  check(0x13200001,0,0);
  /* 11.20 is exact-only as well: the 2026-10-29 dump measured build 0x11200005
   * (request 0x2e0, constructor 0x56888, not the 11.40 0x56fc8), so another
   * 11.20 build must be refused instead of borrowing that geometry. */
  check(0x11200005,0x11200005,0x2e0);
  check(0x11200000,0,0);
  check(0x11200001,0,0);
  /* Numerically inside a family but absent from the supplied release list. */
  check(0x09300009,0,0);
  check(0x11500001,0,0);
  check(0x12100001,0,0);
  check(0x13500001,0,0);
  check(0x092a0001,0,0);
  check(0x09000000,0,0);
  check(0x09600000,0,0);
  check(0x11000000,0,0);
  check(0x13000000,0,0);
  check(0x14000000,0,0);
  check(0xffffffff,0,0);
  /* 4.00 is measured since 2026-10-29: the dump's size store decodes at 0x1b7f12
   * as `mov qword ptr [rdi+0x10], 0x230` and 0x33f is the Avcap2 prologue, so the
   * profile is selected without any opt-in define. Exact-only like 3.20. */
  check(0x04000042,0x04000042,0x230);
  check(0x04000043,0,0);
  /* 4.51 repeats the 4.00 layout one byte later (open_video 0x340, IPC 0x3ac5,
   * size store 0x1b7f13): it has its own dump, so it has its own exact ID. */
  check(0x04510001,0x04510001,0x230);
  check(0x04510000,0,0);
  puts("PASS: exact profile precedence, family interiors, build revisions, endpoints and gaps");
  return 0;
}
