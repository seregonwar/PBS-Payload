// SPDX-License-Identifier: GPL-3.0-or-later
/* Host regression for the operational log. No console, no network.
 * Exercises level filtering, rotation, failure tolerance, the promise that a
 * configuration token never reaches the file, and concurrent writers. */
#include "common/log/log.h"
#include "common/config/config.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRET "s3cr3t-TOKEN-value"
#define LOG_PATH "pbs_log_test.log"
#define ROTATED LOG_PATH ".1"
#define BIG_MAX (4ul * 1024ul * 1024ul)

static char buffer[1 << 20];

static char *slurp(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  if (!f) { if (length) *length = 0; return NULL; }
  size_t got = fread(buffer, 1, sizeof buffer - 1, f);
  buffer[got] = '\0';
  fclose(f);
  if (length) *length = got;
  return buffer;
}

static int count_lines(const char *text) {
  int lines = 0;
  for (const char *p = text; *p; p++) if (*p == '\n') lines++;
  return lines;
}

static void clean(void) { remove(LOG_PATH); remove(ROTATED); }

static void verify_levels_and_header(void) {
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, BIG_MAX);
  pbs_log_error("an error %d", 1);
  pbs_log_warn("a warning");
  pbs_log_info("an info");
  pbs_log_debug("a debug line that must be filtered out");
  pbs_log_close();

  const char *text = slurp(LOG_PATH, NULL);
  assert(text);
  assert(strstr(text, "log opened epoch=") && strstr(text, "level=info"));
  assert(strstr(text, "an error 1") && strstr(text, "a warning") && strstr(text, "an info"));
  assert(!strstr(text, "filtered out"));
  assert(count_lines(text) == 4);              /* header plus three records */
  assert(text[0] == '[');                       /* every record starts with the stamp */
  assert(pbs_log_dropped() == 0);

  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_DEBUG, BIG_MAX);
  pbs_log_debug("now visible");
  pbs_log_close();
  assert(strstr(slurp(LOG_PATH, NULL), "now visible"));

  /* error level keeps only errors; the header is written as an error */
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_ERROR, BIG_MAX);
  pbs_log_warn("suppressed warning");
  pbs_log_error("kept error");
  pbs_log_close();
  const char *quiet = slurp(LOG_PATH, NULL);
  assert(!strstr(quiet, "suppressed") && strstr(quiet, "kept error"));
  clean();
}

static void verify_append_and_truncation(void) {
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, BIG_MAX);
  pbs_log_info("first session");
  pbs_log_close();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, BIG_MAX);   /* reopening appends */
  pbs_log_info("second session");
  char huge[4096];
  memset(huge, 'x', sizeof huge - 1);
  huge[sizeof huge - 1] = '\0';
  pbs_log_info("%s", huge);                        /* longer than PBS_LOG_LINE_MAX */
  pbs_log_info("after the long line");
  pbs_log_close();

  size_t length;
  const char *text = slurp(LOG_PATH, &length);
  assert(strstr(text, "first session") && strstr(text, "second session"));
  assert(strstr(text, "after the long line"));
  /* The oversized record is truncated but stays a single well-formed line. */
  for (const char *p = text; *p; ) {
    const char *end = strchr(p, '\n');
    assert(end);                                   /* no record without a newline */
    assert((size_t)(end - p) < PBS_LOG_LINE_MAX);
    p = end + 1;
  }
  assert(text[length - 1] == '\n');
  clean();
}

static void verify_rotation(void) {
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, 2048);
  for (int i = 0; i < 400; i++) pbs_log_info("rotation filler record %04d", i);
  pbs_log_close();

  size_t current, previous;
  const char *text = slurp(LOG_PATH, &current);
  assert(text && current > 0 && current <= 2048 + PBS_LOG_LINE_MAX);
  assert(strstr(text, "rotation filler record 0399"));   /* the newest record is kept */
  assert(slurp(ROTATED, &previous) && previous > 0);     /* the previous file survives */
  assert(pbs_log_dropped() == 0);
  clean();
}

/* A log that cannot be opened, and a log that is closed, must both be
 * harmless: no crash, no abort, the caller simply keeps running. */
static void verify_failures_never_stop_the_payload(void) {
  clean();
  pbs_log_write(PBS_LOG_ERROR, "before any open");          /* no log yet */
  pbs_log_open("/pbs-nonexistent-directory-xyz/pbs.log", PBS_LOG_INFO, 0);
  pbs_log_error("into a log that could not be opened");
  pbs_log_info("still running");
  pbs_log_close();
  pbs_log_close();                                          /* a second close is safe */
  pbs_log_write(PBS_LOG_ERROR, "after close");

  char too_long[512];
  memset(too_long, 'p', sizeof too_long - 1);
  too_long[sizeof too_long - 1] = '\0';
  pbs_log_open(too_long, PBS_LOG_INFO, 0);                  /* path longer than the limit */
  pbs_log_error("ignored");
  pbs_log_close();

  pbs_log_open(NULL, PBS_LOG_INFO, 0);
  pbs_log_error("ignored too");
  pbs_log_close();

  /* The layer still works normally afterwards. */
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, BIG_MAX);
  pbs_log_info("recovered");
  pbs_log_close();
  assert(strstr(slurp(LOG_PATH, NULL), "recovered"));
  clean();
}

/* The token must never reach the log, neither through the describe() summary
 * the payload records at startup nor through a parser issue message. */
static void verify_token_never_reaches_the_log(void) {
  static const char *const hostile[] = {
    "auth_token=" SECRET "\n",
    SECRET "\n",
    SECRET "=1\n",
    "port=" SECRET "\n",
    "auth_token=" SECRET "\nport=8080\nmystery=1\n",
  };
  for (size_t i = 0; i < sizeof hostile / sizeof *hostile; i++) {
    clean();
    pbs_log_open(LOG_PATH, PBS_LOG_DEBUG, BIG_MAX);
    pbs_config_t cfg;
    pbs_config_init(&cfg);
    pbs_config_parse(&cfg, hostile[i], strlen(hostile[i]), NULL, NULL);
    char summary[192];
    pbs_config_describe(&cfg, summary, sizeof summary);
    pbs_log_info("config %s", summary);          /* exactly what the payload records */
    pbs_log_close();
    const char *text = slurp(LOG_PATH, NULL);
    assert(text && !strstr(text, SECRET));
  }

  /* A parser issue message routed into the log stays clean too. */
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_DEBUG, BIG_MAX);
  pbs_config_t cfg;
  pbs_config_init(&cfg);
  const char *mixed = "auth_token=short\n" SECRET "\n";
  pbs_config_parse(&cfg, mixed, strlen(mixed), NULL, NULL);
  pbs_log_warn("config line %u: %s", 1u,
               "invalid 'auth_token' (8..64 printable characters, no spaces)");
  char summary[192];
  pbs_config_describe(&cfg, summary, sizeof summary);
  pbs_log_info("config %s", summary);
  pbs_log_close();
  const char *text = slurp(LOG_PATH, NULL);
  assert(text && !strstr(text, SECRET));
  assert(strstr(text, "auth_token=unset"));
  clean();
}

#define THREADS 8
#define PER_THREAD 400

static void *writer(void *arg) {
  int id = (int)(intptr_t)arg;
  for (int i = 0; i < PER_THREAD; i++) pbs_log_info("t=%d i=%d", id, i);
  return NULL;
}

static void start_writers(pthread_t *threads) {
  for (intptr_t t = 0; t < THREADS; t++)
    assert(!pthread_create(&threads[t], NULL, writer, (void *)t));
}

static void join_writers(pthread_t *threads) {
  for (int t = 0; t < THREADS; t++) assert(!pthread_join(threads[t], NULL));
}

/* With the mutex held around the write, records from concurrent threads must
 * never interleave inside a line and none may be lost. */
static void verify_concurrent_writers(void) {
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, BIG_MAX);   /* large enough: no rotation */
  pthread_t threads[THREADS];
  start_writers(threads);
  join_writers(threads);
  pbs_log_close();

  static char seen[THREADS][PER_THREAD];
  memset(seen, 0, sizeof seen);
  char *text = slurp(LOG_PATH, NULL);
  assert(text);
  int records = 0, header = 0;
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned long stamp; char level[16]; int id, index;
    if (strstr(line, "log opened")) { header++; continue; }
    /* A torn line fails this match: every field must be present and sane. */
    assert(sscanf(line, "[%lu] %15s t=%d i=%d", &stamp, level, &id, &index) == 4);
    assert(!strcmp(level, "info"));
    assert(id >= 0 && id < THREADS && index >= 0 && index < PER_THREAD);
    assert(!seen[id][index]);                      /* no record written twice */
    seen[id][index] = 1;
    records++;
  }
  assert(header == 1);
  assert(records == THREADS * PER_THREAD);         /* no record lost */
  for (int t = 0; t < THREADS; t++)
    for (int i = 0; i < PER_THREAD; i++) assert(seen[t][i]);
  assert(pbs_log_dropped() == 0);
  clean();
}

/* Same writers, but small enough to rotate while they run: lines must stay
 * whole in both files and the live file must stay bounded. */
static void verify_concurrent_writers_with_rotation(void) {
  clean();
  pbs_log_open(LOG_PATH, PBS_LOG_INFO, 8192);
  pthread_t threads[THREADS];
  start_writers(threads);
  join_writers(threads);
  pbs_log_close();

  const char *files[] = {LOG_PATH, ROTATED};
  for (size_t f = 0; f < 2; f++) {
    size_t length;
    char *text = slurp(files[f], &length);
    assert(text && length > 0);
    if (f == 0) assert(length <= 8192 + PBS_LOG_LINE_MAX);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
      unsigned long stamp; char level[16]; int id, index;
      if (strstr(line, "log opened")) continue;
      assert(sscanf(line, "[%lu] %15s t=%d i=%d", &stamp, level, &id, &index) == 4);
      assert(id >= 0 && id < THREADS && index >= 0 && index < PER_THREAD);
    }
  }
  assert(pbs_log_dropped() == 0);
  clean();
}

int main(void) {
  verify_levels_and_header();
  verify_append_and_truncation();
  verify_rotation();
  verify_failures_never_stop_the_payload();
  verify_token_never_reaches_the_log();
  verify_concurrent_writers();
  verify_concurrent_writers_with_rotation();
  clean();
  puts("log tests passed");
  return 0;
}
