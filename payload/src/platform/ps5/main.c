// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS streaming payload. Research commands remain in the legacy repository. */
#define PBS_DATA_DIR "/data/pbs"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>



#include "common/http/server.h"
#include "common/media/profiles.h"
#include "common/process/inject.h"
#include "platform/ps5/capture_fallback.h"
#include "platform/ps5/offset_dump.h"
#include "platform/ps5/offset/profile.h"
#include "platform/ps5/module_dump.h"



typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, uint32_t *);

static int notify(const char *text) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, text, sizeof req.message - 1);
  return sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static FILE *open_report(const char *name, const char *mode) {
  char path[160];
  snprintf(path, sizeof path, "%s/%s", PBS_DATA_DIR, name);
  FILE *f = fopen(path, mode);
  if (f) setvbuf(f, NULL, _IONBF, 0);
  return f;
}

static unsigned char scratch_zero[4096];
static uint32_t requested_video_bitrate;
static uint32_t requested_video_budget;
static int native_rate_config;
static uint32_t requested_min_qp = 35;
static unsigned stress_keyframe_period;
static int stress_live;
static int live_request;
static unsigned live_start_attempts;
static int live_server_started;
/* Set when the listener could not be created at all, so the closing
 * notification reports the port conflict instead of a capture failure. */
static char stream_failure[96];
static int live_profile_ready;
static int bitrate_after_start;

static const char *fmt_rc(long rc);

static void set_video_bitrate(FILE *f, pbs_inject_t *ctx, uint64_t handle) {
  intptr_t fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2SetVideoBitrate");
  unsigned char params[16] = {0};
  uint64_t size = sizeof params;
  memcpy(params, &size, 8); memcpy(params + 8, &requested_video_bitrate, 4);
  memcpy(params + 12, &requested_video_budget, 4);
  if (requested_video_budget) fprintf(f, "SetVideoBitrate companion=%u\n", requested_video_budget);
  if (fn && !pbs_inject_write_at(ctx, ctx->scratch + 0x780, params, sizeof params))
    fprintf(f, "SetVideoBitrate(%u%s) -> %s\n", requested_video_bitrate,
      bitrate_after_start ? " after Start" : "", fmt_rc(pbs_inject_call_fast(ctx, fn,
      (uint64_t[6]){handle, (uint64_t)ctx->scratch + 0x780, 0, 0, 0, 0})));
}

static void param_u64(unsigned char *p, size_t offset, uint64_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static void param_u32(unsigned char *p, size_t offset, uint32_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static const char *fmt_rc(long rc) {
  static char buf[64];
  if (rc == LONG_MIN) {
    snprintf(buf, sizeof buf, "NO RESPONSE");
  } else {
    snprintf(buf, sizeof buf, "0x%08lx%s", (unsigned long)rc,
             ((int32_t)(uint32_t)rc >= 0 ? " (ok)" : " (error)"));
  }
  return buf;
}

static void dump_scratch(FILE *f, pbs_inject_t *ctx, size_t len) {
  unsigned char buf[4096];
  if (len > sizeof buf) len = sizeof buf;
  if (pbs_inject_read(ctx, buf, len)) {
    fprintf(f, "  scratch: read failed errno=%d\n", errno);
    return;
  }
  fprintf(f, "  scratch: ");
  for (size_t i = 0; i < len; i++) fprintf(f, "%02x", buf[i]);
  fprintf(f, "\n");
  fflush(f);
}

static uint64_t capture_clock_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* The encoder can deliver fewer than the requested 60 frames/s. Timestamp
 * actual publication time so playback does not consume the stream faster
 * than capture produces it. The HTTP wire clock wraps naturally at 32 bits. */
static uint64_t capture_clock_90k(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 90000 + (uint64_t)t.tv_nsec * 9 / 100000;
}

typedef struct {
  uint64_t handle;
  intptr_t read_fn, stop_fn, close_fn, alloc_fn, free_fn;
  intptr_t buffer;
  FILE *raw, *info;
  uint64_t packets, bytes;
  int publish; /* Publish audio blocks to the HTTP server instead of a file. */
  int analyze; /* Accumulate float32 sample statistics. */
  uint32_t last_pts;
  uint32_t vol_original;
  intptr_t setvol_fn;
  int vol_changed;
  double sumsq;
  uint64_t floats;
  float peak;
} pbs_audio_t;

/* Open the audio channel with the verified 0x38-byte parameter structure: common size 0x18, usage 2, format, arbitrator feature and 128 kbit/s rate. */
static int pbs_audio_open(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, int width, uint32_t format) {
  memset(audio, 0, sizeof *audio);
  intptr_t open_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
  intptr_t start_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  audio->read_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadAudio");
  audio->stop_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  audio->close_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Close");
  audio->alloc_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  audio->free_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  if (!open_fn || !start_fn || !audio->read_fn || !audio->alloc_fn) return -1;
  unsigned char params[0x440] = {0};
  param_u64(params, 0x408, 0x38);
  param_u64(params, 0x410, 0x18);
  param_u32(params, 0x41c, 2);
  param_u32(params, 0x420, format);
  param_u32(params, 0x424, width == 2560 ? 11 : 12);
  param_u64(params, 0x428, 0x10);
  param_u32(params, 0x430, 128000);
  if (pbs_inject_write(ctx, params, sizeof params)) return -1;
  long rc = pbs_inject_call_fast(ctx, open_fn, (uint64_t[6]){
      (uint64_t)ctx->scratch + 0x400, (uint64_t)ctx->scratch + 0x408, 0, 0, 0, 0});
  fprintf(f, "Audio OpenAudio(format=%u) -> %s%s\n", format, fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0 || ctx->asserted) return -1;
  if (pbs_inject_read_at(ctx, ctx->scratch + 0x400, &audio->handle, sizeof audio->handle) || !audio->handle) {
    fprintf(f, "Audio handle mancante\n");
    return -1;
  }
  audio->buffer = (intptr_t)pbs_inject_call_fast(ctx, audio->alloc_fn, (uint64_t[6]){65536, 0, 0, 0, 0, 0});
  if (audio->buffer <= 0) return -1;
  rc = pbs_inject_call_fast(ctx, start_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0});
  fprintf(f, "Audio Start -> %s%s\n", fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0) return -1;
  return 0;
}

static void pbs_audio_close(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio) {
  if (audio->vol_changed && audio->setvol_fn && audio->handle && !ctx->asserted) {
    float original;
    memcpy(&original, &audio->vol_original, sizeof original);
    fprintf(f, "Audio SetAudioOutVolume(restore %.6f) -> %s\n", (double)original,
            fmt_rc(pbs_inject_call_fast_fp(ctx, audio->setvol_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0}, original)));
    audio->vol_changed = 0;
  }
  if (audio->handle && !ctx->asserted) {
    if (audio->stop_fn) fprintf(f, "Audio Stop -> %s\n", fmt_rc(pbs_inject_call_fast(ctx, audio->stop_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0})));
    if (audio->close_fn) fprintf(f, "Audio Close -> %s\n", fmt_rc(pbs_inject_call_fast(ctx, audio->close_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0})));
  }
  if (audio->buffer > 0 && !ctx->asserted && audio->free_fn)
    pbs_inject_call_fast(ctx, audio->free_fn, (uint64_t[6]){(uint64_t)audio->buffer, 0, 0, 0, 0, 0});
  if (audio->raw) fclose(audio->raw);
  if (audio->info) fclose(audio->info);
  if (audio->analyze && audio->floats)
    fprintf(f, "Audio analisi: rms=%.6f picco=%.6f campioni=%llu\n",
            sqrt(audio->sumsq / (double)audio->floats), (double)audio->peak,
            (unsigned long long)audio->floats);
  fprintf(f, "Audio captured packets=%llu bytes=%llu\n",
          (unsigned long long)audio->packets, (unsigned long long)audio->bytes);
  memset(audio, 0, sizeof *audio);
}

/* Drain at most rounds available audio packets into the reusable local frame buffer. */
static void pbs_audio_drain(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, unsigned char *frame, int rounds) {
  for (int i = 0; i < rounds && audio->read_fn; i++) {
    long rc = pbs_inject_call_fast(ctx, audio->read_fn, (uint64_t[6]){audio->handle,
        (uint64_t)audio->buffer, 65536, (uint64_t)ctx->scratch + 0x500, 1, 0});
    if (rc == LONG_MIN) { fprintf(f, "Audio ReadAudio timeout\n"); break; }
    int32_t count = (int32_t)(uint32_t)rc;
    if (count <= 0) {
      if (!audio->packets) fprintf(f, "Audio ReadAudio -> %s\n", fmt_rc(rc));
      break;
    }
    if (count > 65536 || pbs_inject_read_at(ctx, audio->buffer, frame, count)) break;
    unsigned char info[16] = {0};
    pbs_inject_read_at(ctx, ctx->scratch + 0x500, info, sizeof info);
    uint32_t pts = 0;
    memcpy(&pts, info + 8, sizeof pts);
    audio->last_pts = pts;
    if (audio->publish) {
      if (pbs_audio_publish(frame, (size_t)count, pts)) break;
    } else {
      if (audio->info) { uint32_t size = (uint32_t)count; fwrite(&size, 1, 4, audio->info); fwrite(info, 1, 16, audio->info); }
      if (audio->raw) fwrite(frame, 1, count, audio->raw);
    }
    if (audio->analyze && (count & 3) == 0) {
      const float *samples = (const float *)frame;
      for (int j = 0; j < count / 4; j++) {
        double v = samples[j];
        audio->sumsq += v * v;
        float a = v < 0 ? (float)-v : (float)v;
        if (a > audio->peak) audio->peak = a;
      }
      audio->floats += (uint64_t)(count / 4);
    }
    if (!audio->packets) {
      fprintf(f, "Audio first packet bytes=%d prefix=", count);
      for (unsigned j = 0; j < 24 && j < (unsigned)count; j++) fprintf(f, "%02x", frame[j]);
      fprintf(f, " metadata=");
      for (unsigned j = 0; j < sizeof info; j++) fprintf(f, "%02x", info[j]);
      fprintf(f, "\n");
    }
    audio->packets++; audio->bytes += (uint64_t)count;
  }
}

static int capture_stream(FILE *f, pbs_inject_t *ctx, uint64_t handle, int seconds, int port, int width, int height, int audio_mode) {
  if (seconds < 0 || seconds > 60) return -1;
  int serving = seconds == 0 || stress_live;
  int recover = 0;
  intptr_t start = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  intptr_t stop = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  intptr_t read = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadVideo");
  intptr_t alloc = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  intptr_t release = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  intptr_t request_key = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2RequestKeyFrame");
  if (!start || !stop || !read || !alloc || !release) return -1;
  long buffer = pbs_inject_call_fast(ctx, alloc, (uint64_t[6]){0x200000, 0, 0, 0, 0, 0});
  if (buffer <= 0) return -1;
  unsigned char *frame = malloc(0x200000);
  if (!frame) goto capture_free;
  FILE *out = fopen("/data/pbs/h264.stream", "wb");
  if (!out) goto capture_free;
  if (requested_video_bitrate && !bitrate_after_start) set_video_bitrate(f, ctx, handle);
  pbs_audio_t audio;
  memset(&audio, 0, sizeof audio);
  if (audio_mode == 2 && !pbs_audio_open(f, ctx, &audio, width, 0)) audio.publish = 1;
  long rc = pbs_inject_call_fast(ctx, start, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
  fprintf(f, "Start() -> %s\n", fmt_rc(rc));
  if (serving && native_rate_config && requested_video_bitrate > 30000000 &&
      rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && !ctx->asserted) {
    /* Close this caller-owned handle before the outer loop retries the
     * conservative profile. Never retry a failed remote call/assertion. */
    fprintf(f, "High bitrate Start rejected; next capture uses 30M QP=35..51\n");
    requested_video_bitrate = requested_video_budget = 30000000;
    requested_min_qp = 35;
  }
  uint64_t total = 0, frames = 0;
  uint64_t measurement_start = 0, measurement_end = 0;
  uint64_t last_stress_key_frame = UINT64_MAX;
  if (serving && rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && pbs_h264_server_running()) recover = 1;
  if (rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0) {
    if (requested_video_bitrate && bitrate_after_start) set_video_bitrate(f, ctx, handle);
    intptr_t headers = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2GetNonVclNalUnitStream");
    if (headers) {
      long hr = pbs_inject_call_fast(ctx, headers, (uint64_t[6]){handle, (uint64_t)buffer, 0, 0, 0, 0});
      fprintf(f, "GetNonVclNalUnitStream() -> %s\n", fmt_rc(hr));
      unsigned char metadata[0x1a8];
      if (hr != LONG_MIN && (int32_t)(uint32_t)hr >= 0 &&
          !pbs_inject_read_at(ctx, buffer, metadata, sizeof metadata)) {
        FILE *hf = fopen("/data/pbs/h264.headers", "wb");
        if (hf) { fwrite(metadata, 1, sizeof metadata, hf); fclose(hf); }
        uint64_t sps_len, pps_len;
        memcpy(&sps_len, metadata + 0x10, 8);
        memcpy(&pps_len, metadata + 0x98, 8);
        if (sps_len > 0 && sps_len <= 128 && pps_len > 0 && pps_len <= 128 &&
            (metadata[0x18] & 31) == 7 && (metadata[0xa0] & 31) == 8) {
          const unsigned char annexb[4] = {0, 0, 0, 1};
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0x18, 1, sps_len, out);
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0xa0, 1, pps_len, out);
          fprintf(f, "Annex B headers: SPS=%llu PPS=%llu\n",
                  (unsigned long long)sps_len, (unsigned long long)pps_len);
          if (serving) {
            int server_rc = pbs_h264_server_start(port, width, height, metadata + 0x18, sps_len, metadata + 0xa0, pps_len);
            int bound_port = server_rc ? 0 : pbs_h264_server_port();
            fprintf(f, "H.264 HTTP :%d -> %d\n", port, server_rc);
            if (!server_rc) {
              live_server_started = 1;
              live_profile_ready = 1;
              stream_failure[0] = 0;
              if (bound_port && bound_port != port)
                fprintf(f, "port %d busy, serving on %d\n", port, bound_port);
              if (audio.publish) pbs_audio_configure("pcm_f32le", 2, 48000);
              notify("PBS - Made By Seregonwar\n"
                     "Thanks to: stonedmodder, zer0day, nexgen999, NSTG, and ONE Jailbreak for testing");
              char ready_message[160];
              if (bound_port && bound_port != port)
                snprintf(ready_message, sizeof ready_message,
                         "PBS started successfully\nPort %d was busy: streaming and web dashboard on port %d",
                         port, bound_port);
              else
                snprintf(ready_message, sizeof ready_message,
                         "PBS started successfully\nStreaming and web dashboard ready on port %d", bound_port);
              notify(ready_message);
            } else {
              snprintf(stream_failure, sizeof stream_failure,
                       "PBS: no free port %d-%d", port, port + PBS_H264_PORT_SCAN - 1);
            }
          }
        } else fprintf(f, "unexpected SPS/PPS metadata layout\n");
      }
    }
    uint64_t until = capture_clock_ms() + (uint64_t)seconds * 1000;
    uint64_t last_video_ms = capture_clock_ms();
    int reports = 0;
    measurement_start = capture_clock_ms();
    const uint64_t video_clock_start = capture_clock_90k();
    while ((serving ? pbs_h264_server_running() && (!stress_live || capture_clock_ms() < until)
                    : capture_clock_ms() < until) && !ctx->asserted) {
      if (serving && pbs_h264_take_restart_request()) { recover = 1; fprintf(f, "capture restart requested\n"); break; }
      int key_requested = serving && pbs_h264_take_keyframe_request();
      int stress_key = (!serving || stress_live) && stress_keyframe_period &&
                      frames % stress_keyframe_period == 0 && frames != last_stress_key_frame;
      if (request_key && (key_requested || stress_key)) {
        if (stress_key) last_stress_key_frame = frames;
        long kr = pbs_inject_call_fast(ctx, request_key, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
        if (key_requested || kr == LONG_MIN || (int32_t)(uint32_t)kr < 0)
          fprintf(f, "RequestKeyFrame() -> %s%s\n", fmt_rc(kr), ctx->asserted ? " [ASSERT]" : "");
        if (kr == LONG_MIN || ctx->asserted) break;
      }
      rc = pbs_inject_call_fast(ctx, read, (uint64_t[6]){handle, (uint64_t)buffer,
          0x200000, (uint64_t)ctx->scratch + 0x300, 0, 0});
      if (rc == LONG_MIN) {
        fprintf(f, "ReadVideo timeout\n");
        recover = serving && !ctx->asserted;
        break;
      }
      int32_t bytes = (int32_t)(uint32_t)rc;
      if (bytes > 0 && bytes <= 0x200000) {
        if (pbs_inject_read_at(ctx, buffer, frame, bytes)) {
          fprintf(f, "frame memory read failed errno=%d\n", errno);
          break;
        }
        if (serving) {
          uint32_t pts = (uint32_t)(capture_clock_90k() - video_clock_start);
          if (pbs_h264_publish(frame, bytes, pts)) {
            fprintf(f, "frame publish failed\n");
            break;
          }
        } else if (fwrite(frame, 1, bytes, out) != (size_t)bytes) break;
        total += bytes;
        frames++;
        last_video_ms = capture_clock_ms();
      } else if (reports++ < 3) {
        fprintf(f, "ReadVideo() -> %s\n", fmt_rc(rc));
        dump_scratch(f, ctx, 16);
      }
      /* ReadVideo blocks until a frame is available; no extra delay after
       * a successful read. Back off only on an empty/error result. */
      if (bytes <= 0) usleep(1000);
      if (serving && capture_clock_ms() - last_video_ms >= 2000) {
        fprintf(f, "capture stalled: last ReadVideo=%s; reopening session\n", fmt_rc(rc));
        recover = 1;
        break;
      }
      if (audio.handle && !ctx->asserted) pbs_audio_drain(f, ctx, &audio, frame, 2);
    }
    if (serving) fprintf(f, "live loop exit running=%d asserted=%d\n", pbs_h264_server_running(), ctx->asserted);
    long sr = pbs_inject_call_fast(ctx, stop, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
    measurement_end = capture_clock_ms();
    fprintf(f, "Stop() -> %s\n", fmt_rc(sr));
  }
  if (serving) {
    if (recover) pbs_h264_discontinuity();
    else pbs_h264_server_stop();
  }
  if (audio.handle) pbs_audio_close(f, ctx, &audio);
  if (measurement_start && measurement_end > measurement_start) {
    double elapsed = (measurement_end - measurement_start) / 1000.0;
    fprintf(f, "capture metrics seconds=%.3f fps=%.3f Mbps=%.3f stress_keyframe_period=%u\n",
            elapsed, frames / elapsed, total * 8 / elapsed / 1000000, stress_keyframe_period);
  }
  fprintf(f, "capture-fast: %llu reads, %llu bytes -> /data/pbs/h264.stream\n",
          (unsigned long long)frames, (unsigned long long)total);
  fclose(out);
capture_free:
  free(frame);
  pbs_inject_call_fast(ctx, release, (uint64_t[6]){(uint64_t)buffer, 0, 0, 0, 0, 0});
  return recover;
}

/* Production entry point: exported capture APIs and caller-owned handles. */
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "avserve";
  if (strcmp(mode, "avserve")) { notify("PBS: use avserve [720p|1080p|1440p|2160p] [bitrate=N] [minqp=N] [norate] [offsetdump] [dependencydump] [moduledump] [noaudio] [port]"); return 1; }
  char *serve_args[] = {argv[0], "avopenfast", "SceRemotePlay", "grant", "11", "2", "0", "9999", "2560", "1440", "1"};
#if defined(PBS_PS5_DEFAULT_1080P)
  serve_args[4] = "12"; serve_args[8] = "1920"; serve_args[9] = "1080";
#elif defined(PBS_PS5_DEFAULT_720P)
  serve_args[4] = "12"; serve_args[8] = "1280"; serve_args[9] = "720";
#elif defined(PBS_PS5_DEFAULT_2160P)
  serve_args[4] = "10"; serve_args[8] = "3840"; serve_args[9] = "2160";
#endif
  int offset_dump = 0;
  int offset_dumped = 0;
  int dependency_dump = 0;
  int dependency_dumped = 0;
  int module_file_dump = 0;
  int diagnostic_open_only = 0;
#ifdef PBS_PS5_OFFSET_DUMP_DEFAULT
  offset_dump = 1;
#endif
#ifdef PBS_PS5_DEPENDENCY_DUMP_DEFAULT
  dependency_dump = 1;
#endif
#ifdef PBS_PS5_MODULE_FILE_DUMP_DEFAULT
  module_file_dump = 1;
#endif
#ifdef PBS_PS5_DIAGNOSTIC_OPEN_ONLY
  diagnostic_open_only = 1;
#endif
  int audio_mode = 2;
  int automatic_resolution = 1;
  if (!strcmp(mode, "avserve")) {
    live_request = 1;
    native_rate_config = 1;
#ifdef PBS_PS5_NO_NATIVE_RATE_DEFAULT
    native_rate_config = 0;
#endif
    requested_min_qp = 18;
    requested_video_bitrate = 60000000;
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) {
        char *end = NULL;
        unsigned long rate = strtoul(argv[i] + 8, &end, 10);
        if (!end || *end || rate < 1000000 || rate > 100000000) return 1;
        requested_video_bitrate = (uint32_t)rate;
      } else if (!strncmp(argv[i], "minqp=", 6)) {
        char *end = NULL;
        unsigned long qp = strtoul(argv[i] + 6, &end, 10);
        if (!end || *end || qp < 18 || qp > 35) return 1;
        requested_min_qp = (uint32_t)qp;
      } else if (!strcmp(argv[i], "norate")) {
        native_rate_config = 0;
      } else if (!strcmp(argv[i], "offsetdump")) {
        offset_dump = 1;
      } else if (!strcmp(argv[i], "dependencydump")) {
        dependency_dump = 1;
      } else if (!strcmp(argv[i], "moduledump")) {
        module_file_dump = 1;
      }
    }
    requested_video_budget = requested_video_bitrate;
    audio_mode = 2; /* Live PCM audio for connected viewers. */
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) continue;
      if (!strncmp(argv[i], "minqp=", 6)) continue;
      if (!strcmp(argv[i], "norate")) continue;
      if (!strcmp(argv[i], "offsetdump")) continue;
      if (!strcmp(argv[i], "dependencydump")) continue;
      if (!strcmp(argv[i], "moduledump")) continue;
      if (!strcmp(argv[i], "noaudio")) { audio_mode = 0; continue; }
      if (!strcmp(argv[i], "720p")) { automatic_resolution = 0; serve_args[4] = "12"; serve_args[8] = "1280"; serve_args[9] = "720"; continue; }
      if (!strcmp(argv[i], "1080p")) { automatic_resolution = 0; serve_args[4] = "12"; serve_args[8] = "1920"; serve_args[9] = "1080"; continue; }
      if (!strcmp(argv[i], "1440p") || !strcmp(argv[i], "2k")) { automatic_resolution = 0; serve_args[4] = "11"; serve_args[8] = "2560"; serve_args[9] = "1440"; continue; }
      if (!strcmp(argv[i], "2160p") || !strcmp(argv[i], "4k")) { automatic_resolution = 0; serve_args[4] = "10"; serve_args[8] = "3840"; serve_args[9] = "2160"; continue; }
      char *end = NULL;
      unsigned long port = strtoul(argv[i], &end, 10);
      if (!end || *end || port < 1 || port > 65535) return 1;
      serve_args[7] = argv[i];
    }
    argv = serve_args; argc = 10; mode = "avopenfast";
  }

  if (mkdir(PBS_DATA_DIR, 0777) && errno != EEXIST) {
    notify("PBS: cannot create /data/pbs");
    return 1;
  }
  FILE *startup_report = open_report("inject.txt", "a");
  if (startup_report) {
    uint32_t firmware = kernel_get_fw_version();
    fprintf(startup_report, "PBS startup firmware=0x%08x capture_profile=%s\n",
            firmware, pbs_ps5_find_offsets(firmware) ? "available" : "missing");
    fclose(startup_report);
  }
  if (module_file_dump) {
    FILE *dump_report = open_report("inject.txt", "a");
    if (dump_report) {
      pid_t target = pbs_find_pid("SceRemotePlay");
      if (target > 0) pbs_ps5_dump_module_files(dump_report, target, PBS_DATA_DIR "/dumps");
      else fprintf(dump_report, "module file dump: SceRemotePlay unavailable\n");
      fclose(dump_report);
    }
  }

  if (!strcmp(mode, "avopen1") || !strcmp(mode, "avopenfast")) {
    /* Open the capture session; recovery retries only caller-owned resources. */
    if (argc < 3) return 1;
    const uint32_t capture_firmware = kernel_get_fw_version();
    const pbs_ps5_offset_profile *layout = pbs_ps5_find_offsets(capture_firmware);
    if (!layout) {
      notify("PBS: firmware capture layout unavailable; use dump payload");
      return 1;
    }
    const pbs_ps5_avcap2_offsets *avcap = &layout->avcap2;
    const pbs_ps5_remoteplay_offsets *rp = &layout->remoteplay;
    /* FW4.00 ends its request after the short 0x14-byte mask descriptor. */
    const uint32_t mask_span = rp->mask_span ? rp->mask_span : 0x24;
    if ((mask_span != 0x14 && mask_span != 0x24) ||
        avcap->request_size > 0x400 || avcap->request_size > sizeof scratch_zero - 8 ||
        rp->rate.offset + 0x24 > avcap->request_size ||
        rp->image.offset + 0x10 > avcap->request_size ||
        rp->width_offset + 4 > avcap->request_size ||
        rp->height_offset + 4 > avcap->request_size ||
        rp->paste_offset + 0x88 > avcap->request_size ||
        rp->video_mode_offset + 4 > avcap->request_size ||
        avcap->kind_offset + 4 > avcap->request_size ||
        rp->mask_mode_offset + mask_span > avcap->request_size) {
      notify("PBS: invalid capture layout");
      return 1;
    }
    const char *how = argc > 3 ? argv[3] : "none";
    int width = argc > 8 ? atoi(argv[8]) : 1280;
    int height = argc > 9 ? atoi(argv[9]) : 720;
    unsigned capture_feature = (unsigned)strtoul(argv[4], NULL, 0);
    pbs_capture_profile previous_profile = {width, height, capture_feature};
    int changing_profile = 0;
    /* One pre-open switch per payload run: after it, the profile already
     * follows the console output and must not be recomputed in a loop. */
    int mode_switch_done = 0;
    if (live_request) pbs_h264_enable_profiles();
    /* Diagnostic native frame-rate enum: 0=30, 1=60; 2 is unverified.
     * Streaming keeps the verified 60 fps profile and matching timestamps. */
    uint32_t fps_mode = argc > 10 ? (uint32_t)strtoul(argv[10], NULL, 0) : 1;
    if (fps_mode > 2 || (argc > 6 && !atoi(argv[6]) && fps_mode != 1)) return 1;
    if (!((width == 1280 && height == 720) || (width == 1920 && height == 1080) ||
          (width == 2560 && height == 1440) || (width == 3840 && height == 2160))) return 1;
    long (*call)(pbs_inject_t *, intptr_t, const uint64_t[6]) =
        !strcmp(mode, "avopenfast") ? pbs_inject_call_fast : pbs_inject_call;
    char failure_note[96] = "PBS: capture unavailable";
avopen_retry:;
    uint32_t detected_video_mode = 0;
    uint32_t open_result = 0;
    live_profile_ready = 0;
    if(live_request)live_start_attempts++;
    int owned_session_closed = 1;
    int dump_this_open = 0;
    FILE *f = open_report("inject.txt", "a");
    if (!f) return 1;
    fprintf(f, "%s %s %s\n", mode, argv[2], how);
    pbs_inject_t ctx;
    if (pbs_inject_begin(argv[2], &ctx)) {
      /* pbs_find_pid does not set errno: a missing target and a failed attach
       * both land here, so print the process table that the lookup saw. */
      fprintf(f, "begin failed errno=%d target=%s; processes:\n", errno, argv[2]);
      if (pbs_list_processes(f, 48) < 0) fprintf(f, "process table unavailable\n");
      fclose(f);
      return 1;
    }
    const intptr_t base = ctx.scratch;
    if (automatic_resolution && width == 2560 && height == 1440) {
      intptr_t fn_probe_mode = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
      uint32_t output_mode = 0;
      if (fn_probe_mode && !pbs_inject_write_at(&ctx, base + 0x700, &output_mode, sizeof output_mode)) {
        long probe_rc = call(&ctx, fn_probe_mode,
                             (uint64_t[6]){(uint64_t)base + 0x700, 0, 0, 0, 0, 0});
        if (probe_rc != LONG_MIN && (int32_t)(uint32_t)probe_rc >= 0 &&
            !pbs_inject_read_at(&ctx, base + 0x700, &output_mode, sizeof output_mode)) {
          fprintf(f, "Preflight videoOutMode=0x%x\n", output_mode);
          int auto_height = pbs_ps5_avoid_rejected_open(output_mode, height, automatic_resolution);
          pbs_capture_profile natural;
          if (auto_height && !pbs_capture_profile_for_height(auto_height, &natural)) {
            width = natural.width; height = natural.height; capture_feature = natural.feature;
            fprintf(f, "Auto capture profile=%dp feature=%u (mode=0x%x rejects 1440p)\n",
                    auto_height, capture_feature, output_mode);
          } else if (output_mode == 0x10u ||
              pbs_ps5_prefer_1080_before_open(capture_firmware, output_mode)) {
            width = 1920;
            height = 1080;
            capture_feature = 12;
            fprintf(f, "Auto capture profile=1080p feature=12 (fw=0x%08x mode=0x%x)\n",
                    capture_firmware, output_mode);
          }
        } else {
          fprintf(f, "Preflight videoOutMode unavailable; keeping requested profile\n");
        }
        if (ctx.asserted) {
          fprintf(f, "Preflight videoOutMode asserted; stopping capture\n");
          pbs_inject_end(&ctx);
          fclose(f);
          notify("PBS: video mode query failed");
          return 1;
        }
      }
      /* FW13.42 was observed rejecting an OpenVideo request, but the screenshot
       * does not expose GetVideoOutMode. Its native layout matches 13.60, so
       * select the compatibility profile even when the preflight query fails.
       * This only affects automatic startup, never explicit resolution modes. */
      if (width == 2560 && height == 1440 &&
          pbs_ps5_prefer_1080_before_open(capture_firmware, 0u)) {
        width = 1920;
        height = 1080;
        capture_feature = 12;
        fprintf(f, "Auto capture profile=1080p feature=12 (fw=0x%08x, conservative startup)\n",
                capture_firmware);
      }
    }
    memset(scratch_zero, 0, sizeof scratch_zero);
    if (argc > 4) {
      /* Native SceRemotePlay constructor and Avcap2 must agree on this ABI. */
      uint64_t size = avcap->request_size, common_size = 0x28;
      uint32_t feature = capture_feature;
      uint32_t usage = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 2;
      uint32_t resolution = width == 3840 ? 6 : width == 2560 ? 5 : width == 1920 ? 4 : 3;
      memcpy(scratch_zero + 8, &size, 8);
      memcpy(scratch_zero + 0x10, &common_size, 8);
      memcpy(scratch_zero + 8 + 0x14, &usage, 4);
      memcpy(scratch_zero + 8 + 0x18, &resolution, 4);
      memcpy(scratch_zero + 8 + 0x1c, &fps_mode, 4);
      memcpy(scratch_zero + 8 + 0x28, &feature, 4);
      /* ImagePaste uses the full output rectangle and all surface masks; offsets are relative to Params. */
      unsigned char *params = scratch_zero + 8;
      if (native_rate_config) {
        /* Rate group layout comes from this firmware's native constructor.
         * Keep initial capacity >= the backend's budget-per-frame floor. */
        uint32_t denominator = fps_mode == 0 ? 30000 : 60000;
        uint32_t initial = (uint32_t)(((uint64_t)requested_video_budget * 1001 + denominator - 1) / denominator);
        param_u64(params, rp->rate.offset, rp->rate.size);
        param_u64(params, rp->rate.offset + 8, rp->rate.descriptor_size);
        param_u32(params, rp->rate.offset + 0x10, requested_video_bitrate);
        param_u32(params, rp->rate.offset + 0x14, requested_video_budget);
        param_u32(params, rp->rate.offset + 0x18, initial);
        param_u32(params, rp->rate.offset + 0x1c, requested_min_qp);
        param_u32(params, rp->rate.offset + 0x20, 51);
        fprintf(f, "Native rate config rate=%u companion=%u initial=%u QP=%u..51\n",
                requested_video_bitrate, requested_video_budget, initial, requested_min_qp);
      }
      param_u64(params, rp->image.offset, rp->image.size);
      param_u64(params, rp->image.offset + 8, rp->image.descriptor_size);
      param_u32(params, rp->width_offset, width);
      param_u32(params, rp->height_offset, height);
      param_u64(params, rp->paste_offset, 0x88);
      memset(params + rp->paste_offset + 8, 0xff, 0x80);
      param_u32(params, rp->mask_mode_offset, 2);
      param_u64(params, rp->mask_mode_offset + 4, 0x10);
      param_u32(params, rp->mask_mode_offset + 0xc, 1);
      params[rp->mask_mode_offset + 0x10] = 1;
      if (mask_span == 0x24) {
        param_u64(params, rp->mask_mode_offset + 0x14, 0x10);
        params[rp->mask_mode_offset + 0x1c] = 1;
        param_u32(params, rp->mask_mode_offset + 0x20, 0x6e);
      }
      fprintf(f, "Params size=0x%x feature=%u usage=%u %dx%d fps-mode=%u fw=0x%08x profile=0x%08x selection=%s\n",
              avcap->request_size, feature, usage, width, height, fps_mode,
              capture_firmware, layout->firmware,
              capture_firmware == layout->firmware ? "exact" : "family-range");
    }
    intptr_t fn_init = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Initialize");
    intptr_t fn_open = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenVideo");
    intptr_t fn_oaudio = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
    intptr_t fn_term = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Terminate");
    intptr_t fn_clear = 0;
    uint32_t grant_feature = 0;
    int granted_callback = 0;
    if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
      fprintf(f, "initial capture parameters could not be written\n");
      goto avopen_cleanup;
    }
    if (!fn_open) {
      fprintf(f, "OpenVideo export unavailable\n");
      snprintf(failure_note, sizeof failure_note, "PBS: OpenVideo export missing");
      goto avopen_cleanup;
    }
    uint32_t avcap_handle = 0;
    if (!kernel_dynlib_handle(ctx.pid, "libSceAvcap2.sprx", &avcap_handle)) {
      intptr_t avcap_base = kernel_dynlib_mapbase_addr(ctx.pid, avcap_handle);
      if (avcap_base > 0 && fn_open - avcap_base != (intptr_t)avcap->open_video_rva)
        fprintf(f, "OpenVideo RVA differs: resolved=0x%lx profile=0x%x\n",
                (unsigned long)(fn_open - avcap_base), avcap->open_video_rva);
    }
    if (dependency_dump && !dependency_dumped) {
      pbs_ps5_dump_dependencies(f, &ctx);
      pbs_ps5_dump_initial_user(f, &ctx);
      dependency_dumped = 1;
      if (ctx.asserted) goto avopen_cleanup;
    }
    if (!strcmp(how, "tinit") && fn_term) {
      long tr = call(&ctx, fn_term, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Terminate() -> %s\n", fmt_rc(tr));
    }
    if (strcmp(how, "none") && fn_init) {
      long ir = call(&ctx, fn_init, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Initialize() -> %s\n", fmt_rc(ir));
    }
    if (!strcmp(how, "grant")) {
      grant_feature = capture_feature;
      /* Use capture profiles from ra-config.json. Do not modify features owned by active services. */
      if (strcmp(mode, "avopenfast") ||
          grant_feature != (width == 3840 ? 10u : width == 2560 ? 11u : 12u)) goto avopen_cleanup;
      intptr_t fn_set = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorSetCallback");
      intptr_t fn_change = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorChangeState");
      fn_clear = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorClearCallback");
      /* Use getpid as a stable no-op callback that ignores notification arguments. */
      intptr_t callback = pbs_resolve_in(ctx.pid, "libkernel_sys.sprx", "getpid");
      if (!fn_set || !fn_change || !fn_clear || !callback) {
        fprintf(f, "missing resource arbitrator exports\n");
        goto avopen_cleanup;
      }
      long ar = call(&ctx, fn_set, (uint64_t[6]){grant_feature, (uint64_t)callback, 0, 0, 0, 0});
      fprintf(f, "SetCallback(%u) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar == LONG_MIN || (int32_t)(uint32_t)ar < 0) {
        snprintf(failure_note, sizeof failure_note, "PBS: SetCallback(%u) 0x%08x",
                 grant_feature, (unsigned)(uint32_t)ar);
        goto avopen_cleanup;
      }
      granted_callback = 1;
      char state[32] = "RUNNING";
      memcpy(scratch_zero + 0x600, state, sizeof state);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) goto avopen_cleanup;
      ar = call(&ctx, fn_change, (uint64_t[6]){grant_feature, (uint64_t)base + 0x600, 0, 0, 0, 0});
      fprintf(f, "ChangeState(%u, RUNNING) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar == LONG_MIN || (int32_t)(uint32_t)ar < 0) {
        snprintf(failure_note, sizeof failure_note, "PBS: ChangeState(%u) 0x%08x",
                 grant_feature, (unsigned)(uint32_t)ar);
        goto avopen_cleanup;
      }
      intptr_t fn_enable = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2EnableScanInEncode");
      if (fn_enable) {
        long er = call(&ctx, fn_enable, (uint64_t[6]){0, 0, 0, 0, 0, 0});
        fprintf(f, "EnableScanInEncode() -> %s\n", fmt_rc(er));
        if (er != LONG_MIN && (int32_t)(uint32_t)er < 0) {
          snprintf(failure_note, sizeof failure_note, "PBS: EnableScanInEncode 0x%08x",
                   (unsigned)(uint32_t)er);
          goto avopen_cleanup;
        }
      }
    }
    if (argc > 4) {
      intptr_t fn_mode = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
      uint32_t video_mode = 0;
      long mr = fn_mode ? call(&ctx, fn_mode,
          (uint64_t[6]){(uint64_t)base + 0x700, 0, 0, 0, 0, 0}) : LONG_MIN;
      if (mr == LONG_MIN || (int32_t)(uint32_t)mr < 0 ||
          pbs_inject_read_at(&ctx, base + 0x700, &video_mode, sizeof video_mode)) {
        fprintf(f, "GetVideoOutMode failed -> %s\n", fmt_rc(mr));
        goto avopen_cleanup;
      }
      /* Use the firmware's video-mode field, read immediately before OpenVideo. */
      memcpy(scratch_zero + 8 + rp->video_mode_offset, &video_mode, sizeof video_mode);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
        fprintf(f, "video output mode could not be written\n");
        goto avopen_cleanup;
      }
      fprintf(f, "Params videoOutMode=0x%x\n", video_mode);
      detected_video_mode = video_mode;
      /* Let the HTTP layer refuse a profile the console output cannot drive,
       * instead of attempting a reopen the firmware rejects. */
      pbs_h264_note_output_mode(video_mode);
      /* Never ask the encoder for a profile this output rejects: unwinding a
       * rejected open still has to restore scan-in ownership and the
       * arbitrator callback, and that repair runs on the host's main thread.
       * Switch first, then reopen with the profile the output drives. */
      int natural_height = mode_switch_done ? 0
          : pbs_ps5_avoid_rejected_open(video_mode, height, automatic_resolution);
      if (natural_height) {
        pbs_capture_profile natural;
        if (!pbs_capture_profile_for_height(natural_height, &natural)) {
          fprintf(f, "Auto capture profile=%dp feature=%u (mode=0x%x rejects %dp before open)\n",
                  natural_height, natural.feature, video_mode, height);
          width = natural.width; height = natural.height; capture_feature = natural.feature;
          mode_switch_done = 1;
          goto avopen_cleanup;
        }
      }
    }
    if (offset_dump && !offset_dumped) {
      pbs_ps5_dump_export_offsets(f, ctx.pid);
      pbs_ps5_dump_request(f, "before OpenVideo", scratch_zero + 8, avcap->request_size);
      offset_dumped = dump_this_open = 1;
    }
    long rc = fn_open ? call(&ctx, fn_open,
                              (uint64_t[6]){(uint64_t)base, (uint64_t)base + 8, 0, 0, 0, 0}) : LONG_MIN;
    fprintf(f, "OpenVideo() -> %s%s\n", fmt_rc(rc), ctx.asserted ? " [ASSERT]" : "");
    open_result = (uint32_t)rc;
    if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0 || ctx.asserted)
      snprintf(failure_note, sizeof failure_note, "PBS: OpenVideo 0x%08x%s",
               (unsigned)(uint32_t)rc, ctx.asserted ? " ASSERT" : "");
    if (dump_this_open && rc != LONG_MIN && !ctx.asserted) {
      unsigned char returned[0x400];
      if (pbs_inject_read_at(&ctx, base + 8, returned, avcap->request_size))
        fprintf(f, "offset dump: after OpenVideo request read failed errno=%d\n", errno);
      else {
        pbs_ps5_dump_request(f, "after OpenVideo", returned, avcap->request_size);
        pbs_ps5_dump_request_changes(f, scratch_zero + 8, returned, avcap->request_size);
      }
    }
    if (fn_oaudio && strcmp(mode, "avopenfast")) {
      long ar = pbs_inject_call(&ctx, fn_oaudio,
                                (uint64_t[6]){(uint64_t)base + 0x10, (uint64_t)base + 0x18, 0, 0, 0, 0});
      fprintf(f, "OpenAudio() -> %s%s\n", fmt_rc(ar), ctx.asserted ? " [ASSERT]" : "");
    }
    unsigned char hb[16];
    if (!pbs_inject_read(&ctx, hb, sizeof hb)) {
      uint64_t h = 0;
      memcpy(&h, hb, 8);
      fprintf(f, "handle=0x%llx\n", (unsigned long long)h);
      if (!strcmp(mode, "avopenfast") && rc != LONG_MIN &&
          (int32_t)(uint32_t)rc >= 0 && h) {
        owned_session_closed = 0;
        if (diagnostic_open_only) fprintf(f, "diagnostic open-only: closing handle without starting stream\n");
        else if (argc > 6) capture_stream(f, &ctx, h, atoi(argv[6]), argc > 7 ? atoi(argv[7]) : 9999, width, height, audio_mode);
        intptr_t close_fn = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Close");
        if (close_fn && !ctx.asserted) {
          long close_rc = call(&ctx, close_fn, (uint64_t[6]){h, 0, 0, 0, 0, 0});
          fprintf(f, "Close() -> %s\n", fmt_rc(close_rc));
          owned_session_closed = close_rc != LONG_MIN && (int32_t)(uint32_t)close_rc >= 0;
        }
      }
    }
avopen_cleanup:;
    int clear_ok = !granted_callback;
    if (granted_callback && !ctx.asserted) {
      long clear_rc = call(&ctx, fn_clear, (uint64_t[6]){grant_feature, 0, 0, 0, 0, 0});
      fprintf(f, "ClearCallback(%u) -> %s\n", grant_feature, fmt_rc(clear_rc));
      clear_ok = clear_rc != LONG_MIN && (int32_t)(uint32_t)clear_rc >= 0;
    }
    clear_ok = clear_ok && owned_session_closed;
    /* A session that ended badly (payload crash, killed stream) can leave
     * Avcap2 refusing every feature with 0x81950008: at a 2160p output one run
     * saw 1440p, 2160p and 1080p all rejected although 1080p had opened minutes
     * earlier with the same parameters. Terminate is safe with nothing open, so
     * re-arm the encoder before retrying instead of repeating the rejection. */
    if (open_result == 0x81950008u && !ctx.asserted && fn_term) {
      long tr = call(&ctx, fn_term, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Terminate() after rejected open -> %s\n", fmt_rc(tr));
    }
    /* Retrying another caller-owned profile is safe only after cleanup. Prefer
     * the profile the console output drives; 1080p stays the last resort for
     * modes that are not measured yet and for a rejected 4K open. */
    int fallback_height = 0;
    if (live_request && automatic_resolution && clear_ok && !ctx.asserted)
      fallback_height = pbs_ps5_retry_height(open_result, detected_video_mode, width, height);
    if (fallback_height)
      fprintf(f, "OpenVideo rejected %dp (0x%08x, video mode 0x%x); retrying %dp\n",
              height, open_result, detected_video_mode, fallback_height);
    pbs_inject_end(&ctx);
    fclose(f);
    /* Reconfigure only after Stop/Close/ClearCallback completed for the old
     * caller-owned session. Keep the HTTP listener for reconnecting viewers. */
    if (changing_profile && !live_profile_ready) {
      pbs_h264_profile_failed();
      width = previous_profile.width; height = previous_profile.height;
      capture_feature = previous_profile.feature;
    }
    changing_profile = 0;
    if (fallback_height) {
      pbs_capture_profile fallback;
      if (!pbs_capture_profile_for_height(fallback_height, &fallback)) {
        width = fallback.width; height = fallback.height; capture_feature = fallback.feature;
      }
    }
    int requested_height = 0;
    if (live_request && clear_ok && !ctx.asserted && pbs_h264_take_profile_request(&requested_height)) {
      pbs_capture_profile next;
      if (!pbs_capture_profile_for_height(requested_height, &next)) {
        previous_profile = (pbs_capture_profile){width, height, capture_feature};
        width = next.width; height = next.height; capture_feature = next.feature;
        changing_profile = 1;
        /* A viewer request is explicit: it outranks the automatic-startup
         * heuristic, which must never silently reopen at a smaller profile and
         * leave the requested one unanswered. */
        automatic_resolution = 0;
        fprintf(f, "Explicit profile request %dp; automatic startup profile disabled\n",
                requested_height);
      }
    }
    /* A clean OpenVideo rejection can be transient while scan-in ownership
     * settles. The pre-ac15239 path retried it; keep that bounded recovery. */
    int retry_start = clear_ok && !live_server_started && live_start_attempts < 6;
    if (live_request && clear_ok && !ctx.asserted && (pbs_h264_server_running() || retry_start)) {
      usleep(250000);
      if (pbs_h264_server_running() || retry_start) goto avopen_retry;
    }
    if (live_request) { pbs_h264_server_stop(); pbs_h264_server_wait(); }
    notify(live_server_started ? "PBS: streaming stopped"
                               : (stream_failure[0] ? stream_failure : failure_note));
    return live_server_started ? 0 : 1;
  }

  return 1;
}
