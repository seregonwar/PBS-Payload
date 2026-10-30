// SPDX-License-Identifier: GPL-3.0-or-later
/* One-shot FW9.40 diagnostic. No HTTP, audio, output switching or reopen. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include "common/process/inject.h"
#include "platform/ps5/offset/profile.h"
#include "ps5_940_params.h"

#ifndef PBS_940_CAPTURE
#define PBS_940_CAPTURE 0
#endif
#define DIR "/data/pbs/diag-940"
#define CAPACITY 0x200000
extern const pbs_ps5_offset_profile pbs_ps5_offsets_940;

typedef struct {
  pbs_inject_t ctx;
  FILE *log;
  int uncertain;
} diagnostic;

static uint64_t now_us(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int ok(long rc) { return rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0; }

static long invoke(diagnostic *d, const char *name, intptr_t fn,
                   uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
  if (d->uncertain || d->ctx.asserted || !fn) return LONG_MIN;
  long rc = pbs_inject_call_fast(&d->ctx, fn, (uint64_t[6]){a,b,c,e,0,0});
  /* Do not flood the log on successful frame reads. */
  if (strcmp(name, "ReadVideo") || !ok(rc))
    fprintf(d->log, "%s -> %s0x%08x asserted=%d stage=%s errno=%d\n", name,
            rc == LONG_MIN ? "NO_RESPONSE " : "", (uint32_t)rc, d->ctx.asserted,
            d->ctx.call_stage ? d->ctx.call_stage : "unknown", d->ctx.call_errno);
  if (rc == LONG_MIN || d->ctx.asserted) {
    d->uncertain = 1;
    fprintf(d->log, "Remote state uncertain: no further remote calls; reboot before another test\n");
    return LONG_MIN;
  }
  return rc;
}

static intptr_t resolve(diagnostic *d, const char *module, const char *symbol) {
  intptr_t fn = pbs_resolve_in(d->ctx.pid, module, symbol);
  fprintf(d->log, "export %s=0x%llx\n", symbol, (unsigned long long)fn);
  return fn;
}

static int get_mode(diagnostic *d, intptr_t getter, const char *phase, uint32_t *value) {
  if (d->uncertain || d->ctx.asserted) return -1;
  uint32_t sentinel = 0xffffffff;
  intptr_t slot = d->ctx.scratch + 0x700;
  if (pbs_inject_write_at(&d->ctx, slot, &sentinel, 4)) return -1;
  long rc = invoke(d, "GetVideoOutMode", getter, slot, 0, 0, 0);
  if (!ok(rc) || pbs_inject_read_at(&d->ctx, slot, value, 4)) return -1;
  fprintf(d->log, "videoOutMode phase=%s raw=0x%08x (no Hz interpretation)\n", phase, *value);
  return 0;
}

#if PBS_940_CAPTURE
/* ReadVideo supplies Annex B data on the existing PBS path. Count VCL NALs
 * whose first_mb_in_slice is ue(0). SPS/PPS/AUD and extra slices do not count.
 * Keep read counts separate; ffprobe must confirm the saved stream offline. */
static unsigned picture_starts(const unsigned char *p, size_t n) {
  unsigned count = 0;
  for (size_t i = 0; i + 4 < n; ++i) {
    if (p[i] || p[i+1] || p[i+2] != 1) continue;
    unsigned type = p[i+3] & 31;
    if ((type == 1 || type == 5) && (p[i+4] & 0x80)) ++count;
  }
  return count;
}

static int capture(diagnostic *d, uint64_t handle, intptr_t start,
                   intptr_t read_fn, intptr_t stop, intptr_t alloc,
                   intptr_t release, intptr_t headers, int *stopped) {
  int result = -1, started = 0;
  long buffer = 0;
  unsigned char *frame = malloc(CAPACITY);
  FILE *out = fopen(DIR "/capture.h264", "wb");
  FILE *timing = fopen(DIR "/reads.csv", "w");
  uint64_t reads = 0, pictures = 0, bytes = 0, empty = 0, without_picture = 0;
  uint64_t begin = 0, end = 0, last_data = 0;
  *stopped = 1;
  if (!frame || !out || !timing) goto done;
  fprintf(timing, "elapsed_us,bytes,picture_starts\n");
  buffer = invoke(d, "malloc(2MiB)", alloc, CAPACITY, 0, 0, 0);
  if (buffer <= 0 || d->uncertain) goto done;
  /* Resolve and prepare everything before a single Start call. */
  if (!ok(invoke(d, "Start", start, handle, 0, 0, 0))) goto done;
  started = 1;
  *stopped = 0;
  if (!ok(invoke(d, "GetNonVclNalUnitStream", headers, handle, buffer, 0, 0))) goto done;
  unsigned char h[0x1a8];
  uint64_t slen = 0, plen = 0;
  if (pbs_inject_read_at(&d->ctx, buffer, h, sizeof h)) goto done;
  memcpy(&slen, h + 0x10, 8); memcpy(&plen, h + 0x98, 8);
  if (!slen || slen > 128 || !plen || plen > 128 ||
      (h[0x18] & 31) != 7 || (h[0xa0] & 31) != 8) {
    fprintf(d->log, "Unexpected SPS/PPS layout; abort capture\n");
    goto done;
  }
  const unsigned char prefix[] = {0,0,0,1};
  if (fwrite(prefix, 1, 4, out) != 4 || fwrite(h+0x18,1,slen,out) != slen ||
      fwrite(prefix, 1, 4, out) != 4 || fwrite(h+0xa0,1,plen,out) != plen) goto done;
  begin = last_data = now_us();
  if (!begin) goto done;
  result = 0;
  while (now_us() - begin < 10000000) {
    long rc = invoke(d, "ReadVideo", read_fn, handle, buffer, CAPACITY, d->ctx.scratch + 0x300);
    uint64_t sample = now_us();
    if (!ok(rc) || rc > CAPACITY) { result = -1; break; }
    if (!rc) {
      ++empty;
      if (sample - last_data >= 2000000) {
        fprintf(d->log, "No video for 2s; ending without reopen\n");
        result = -1; break;
      }
      usleep(1000);
      continue;
    }
    if (pbs_inject_read_at(&d->ctx, buffer, frame, (size_t)rc)) { result = -1; break; }
    unsigned count = picture_starts(frame, (size_t)rc);
    ++reads; pictures += count; bytes += rc; without_picture += count == 0;
    last_data = sample;
    if (fwrite(frame, 1, (size_t)rc, out) != (size_t)rc ||
        fprintf(timing, "%llu,%ld,%u\n", (unsigned long long)(sample-begin), rc, count) < 0) {
      result = -1; break;
    }
  }
  /* Stop latency is excluded from the measurement denominator. */
  end = now_us();
done:
  if (started && !d->uncertain) {
    *stopped = ok(invoke(d, "Stop", stop, handle, 0, 0, 0));
    if (!*stopped) { d->uncertain = 1; result = -1; }
  }
  if (buffer > 0 && *stopped && !d->uncertain)
    if (invoke(d, "free", release, (uint64_t)buffer, 0, 0, 0) == LONG_MIN) result = -1;
  if (out && fclose(out)) result = -1;
  if (timing && fclose(timing)) result = -1;
  free(frame);
  double elapsed = end > begin && begin ? (end-begin)/1000000.0 : 0;
  fprintf(d->log, "capture_summary result=%d elapsed_seconds=%.6f reads=%llu bytes=%llu empty=%llu "
          "picture_starts=%llu reads_without_picture=%llu read_rate=%.3f candidate_picture_fps=%.3f\n",
          result, elapsed, (unsigned long long)reads, (unsigned long long)bytes,
          (unsigned long long)empty, (unsigned long long)pictures,
          (unsigned long long)without_picture, elapsed ? reads/elapsed : 0,
          elapsed ? pictures/elapsed : 0);
  fprintf(d->log, "FPS is delivered Annex-B picture-start throughput including ptrace/copy/disk costs; "
          "confirm decoded frame count offline; it does not measure HDMI Hz or prove 120fps\n");
  return result;
}
#endif

int main(int argc, char **argv) {
  (void)argc; (void)argv; /* Fixed test: runtime options cannot enable recovery. */
  diagnostic d = {0};
  int result = 1, callback_owned = 0, handle_owned = 0, stopped = 1, clean = 1;
  uint64_t handle = 0;
  uint32_t mode = 0;
  if ((mkdir("/data/pbs",0777) && errno != EEXIST) ||
      (mkdir(DIR,0777) && errno != EEXIST)) return 1;
  d.log = fopen(PBS_940_CAPTURE ? DIR "/capture.log" : DIR "/open.log", "a");
  if (!d.log) return 1;
  setvbuf(d.log, NULL, _IONBF, 0);
  uint32_t firmware = kernel_get_fw_version();
  fprintf(d.log, "\nBEGIN kind=%s epoch=%lld monotonic_us=%llu firmware=0x%08x "
          "fixed=%dx%d feature=%u usage=2 fps_mode=%d fps_candidate=%d duration=10s attempts=1\n",
          PBS_940_CAPTURE ? "capture" : "open-only", (long long)time(NULL),
          (unsigned long long)now_us(), firmware, PBS_940_WIDTH, PBS_940_HEIGHT,
          PBS_940_FEATURE, PBS_940_FPS_MODE, PBS_940_FPS_MODE == 2 ? 120 : 60);
  if (firmware != 0x09400008u) {
    fprintf(d.log, "REFUSED: exact FW9.40 build 0x09400008 required\n"); goto finish;
  }
  if (pbs_inject_begin("SceRemotePlay", &d.ctx)) {
    fprintf(d.log, "Attach failed errno=%d\n", errno); goto finish;
  }
  intptr_t init = resolve(&d,"libSceAvcap2.sprx","sceAvcap2Initialize");
  intptr_t open_fn = resolve(&d,"libSceAvcap2.sprx","sceAvcap2OpenVideo");
  intptr_t close_fn = resolve(&d,"libSceAvcap2.sprx","sceAvcap2Close");
  intptr_t getter = resolve(&d,"libSceAvcap2.sprx","sceAvcap2GetVideoOutMode");
  intptr_t enable = resolve(&d,"libSceAvcap2.sprx","sceAvcap2EnableScanInEncode");
  intptr_t set = resolve(&d,"libSceResourceArbitrator.sprx","sceResourceArbitratorSetCallback");
  intptr_t change = resolve(&d,"libSceResourceArbitrator.sprx","sceResourceArbitratorChangeState");
  intptr_t clear = resolve(&d,"libSceResourceArbitrator.sprx","sceResourceArbitratorClearCallback");
  intptr_t cb = resolve(&d,"libkernel_sys.sprx","getpid");
#if PBS_940_CAPTURE
  intptr_t start = resolve(&d,"libSceAvcap2.sprx","sceAvcap2Start");
  intptr_t stop = resolve(&d,"libSceAvcap2.sprx","sceAvcap2Stop");
  intptr_t read_fn = resolve(&d,"libSceAvcap2.sprx","sceAvcap2ReadVideo");
  intptr_t headers = resolve(&d,"libSceAvcap2.sprx","sceAvcap2GetNonVclNalUnitStream");
  intptr_t alloc = resolve(&d,"libSceLibcInternal.sprx","malloc");
  intptr_t release = resolve(&d,"libSceLibcInternal.sprx","free");
  if (!start || !stop || !read_fn || !headers || !alloc || !release) goto cleanup;
#endif
  if (!init || !open_fn || !close_fn || !getter || !enable || !set || !change || !clear || !cb)
    goto cleanup;
  uint32_t module_handle = 0;
  if (kernel_dynlib_handle(d.ctx.pid,"libSceAvcap2.sprx",&module_handle)) goto cleanup;
  intptr_t base = kernel_dynlib_mapbase_addr(d.ctx.pid,module_handle);
  unsigned char command[5]={0}, size_store[8]={0};
  const unsigned char expected_command[] = {0xbe,0x01,0x00,0xa7,0x00};
  const unsigned char expected_size[] = {0x48,0xc7,0x45,0xe8,0xb0,0x02,0x00,0x00};
  /* PT_IO data reads cannot read execute-only text on this console (EFAULT).
   * Use the same read-only kernel copy API as the firmware memory dumper. */
  int command_read=kernel_proc_copyout(d.ctx.pid,base+0x3f45,command,5);
  int size_read=kernel_proc_copyout(d.ctx.pid,base+0x3f29,size_store,8);
  fprintf(d.log,"runtime base=0x%llx open_rva=0x%llx getter_rva=0x%llx reads=%d,%d command=",
          (unsigned long long)base,(unsigned long long)(open_fn-base),
          (unsigned long long)(getter-base),command_read,size_read);
  for(unsigned i=0;i<5;++i)fprintf(d.log,"%02x",command[i]);
  fprintf(d.log," size_store=");
  for(unsigned i=0;i<8;++i)fprintf(d.log,"%02x",size_store[i]);
  fputc('\n',d.log);
  if (base <= 0 || open_fn-base != 0x340 || getter-base != 0x1630 || command_read || size_read ||
      memcmp(command,expected_command,5) || memcmp(size_store,expected_size,8)) {
    fprintf(d.log, "REFUSED: Avcap2 runtime RVA/IPC fingerprint differs from measured dump\n");
    goto cleanup;
  }
  if (!ok(invoke(&d,"Initialize",init,0,0,0,0))) goto cleanup;
  if (get_mode(&d,getter,"before-grant",&mode)) goto cleanup;
  if (!ok(invoke(&d,"SetCallback(profile feature)",set,PBS_940_FEATURE,cb,0,0))) goto cleanup;
  callback_owned = 1;
  const char state[32] = "RUNNING";
  if (pbs_inject_write_at(&d.ctx,d.ctx.scratch+0x600,state,sizeof state)) goto cleanup;
  if (!ok(invoke(&d,"ChangeState(profile feature,RUNNING)",change,PBS_940_FEATURE,d.ctx.scratch+0x600,0,0))) goto cleanup;
  if (!ok(invoke(&d,"EnableScanInEncode",enable,0,0,0,0))) goto cleanup;
  if (get_mode(&d,getter,"before-open",&mode)) goto cleanup;
  unsigned char request[0x2b8];
  pbs_940_params(request, &pbs_ps5_offsets_940, mode);
  fprintf(d.log, "request hex (handle slot followed by Params):\n");
  for (unsigned i=0;i<sizeof request;i+=32) {
    char row[98];unsigned used=0;
    for(unsigned j=i;j<i+32 && j<sizeof request;++j)
      used+=(unsigned)snprintf(row+used,sizeof row-used,"%02x ",request[j]);
    row[used++]='\n';fwrite(row,1,used,d.log);
  }
  fputc('\n',d.log);
  if (pbs_inject_write(&d.ctx,request,sizeof request)) goto cleanup;
  long rc = invoke(&d,"OpenVideo",open_fn,d.ctx.scratch,d.ctx.scratch+8,0,0);
  if (!ok(rc)) goto cleanup;
  /* A successful open with unreadable/zero handle cannot be safely unwound. */
  if (pbs_inject_read(&d.ctx,&handle,8) || !handle) { clean=0; goto cleanup; }
  handle_owned = 1;
  fprintf(d.log,"owned_handle=0x%llx\n",(unsigned long long)handle);
#if PBS_940_CAPTURE
  result = capture(&d,handle,start,read_fn,stop,alloc,release,headers,&stopped) ? 1 : 0;
#else
  fprintf(d.log,"Open-only: Start/ReadVideo/Stop absent from this build\n");
  result = 0;
#endif
cleanup:
  if (handle_owned && stopped && !d.uncertain) {
    if (ok(invoke(&d,"Close",close_fn,handle,0,0,0))) handle_owned=0;
    else clean=0;
  }
  if (callback_owned && !handle_owned && clean && !d.uncertain) {
    if (ok(invoke(&d,"ClearCallback(profile feature)",clear,PBS_940_FEATURE,0,0,0))) callback_owned=0;
    else clean=0;
  }
  if (!result && !d.uncertain && !handle_owned && !callback_owned && clean)
    if (getter && get_mode(&d,getter,"after-cleanup",&mode)) clean=0;
  clean = clean && !d.uncertain && !handle_owned && !callback_owned && stopped;
finish:
  pbs_inject_end(&d.ctx);
  fprintf(d.log,"END result=%s cleanup=%s asserted=%d uncertain=%d; "
          "no retries, no global Terminate, shared RemotePlay initialization retained\n",
          result ? "FAIL" : "OK", clean ? "complete" : "INCOMPLETE_REBOOT_REQUIRED",
          d.ctx.asserted,d.uncertain);
  fclose(d.log);
  return result || !clean;
}
