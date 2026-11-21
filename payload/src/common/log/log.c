// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/log/log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH_MAX 192

static struct {
  pthread_mutex_t lock;
  int fd;                 /* guarded by lock after open */
  int level;              /* fixed by pbs_log_open before any thread starts */
  unsigned long bytes;    /* guarded by lock */
  unsigned long max_bytes;
  unsigned dropped;       /* guarded by lock */
  char path[LOG_PATH_MAX];
} logger = {.lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .level = PBS_LOG_INFO};

/* Monotonic milliseconds since the log was opened, like the problem index. */
static unsigned long started_ms;

static unsigned long monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
  return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000l);
}

/* Returns 0 when every byte reached the file. Partial writes are resumed and
 * EINTR is retried; any other error abandons the record. */
static int write_all(int fd, const char *data, size_t length) {
  while (length) {
    ssize_t n = write(fd, data, length);
    if (n > 0) { data += n; length -= (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static void drop_locked(void) {
  if (logger.dropped < UINT_MAX) logger.dropped++;
}

/* Caller holds the lock. Rotation is rare, so doing it here keeps the common
 * path to one write() while still serialising the rename. */
static void rotate_locked(void) {
  char previous[LOG_PATH_MAX + 2];
  int n = snprintf(previous, sizeof previous, "%s.1", logger.path);
  if (n < 0 || (size_t)n >= sizeof previous) { drop_locked(); return; }
  close(logger.fd);
  logger.fd = -1;
  /* A failed rename is not fatal: reopening truncates instead of appending,
   * so the log still bounds itself. */
  int renamed = rename(logger.path, previous) == 0;
  int fd = open(logger.path, O_WRONLY | O_CREAT | (renamed ? O_APPEND : O_TRUNC), 0666);
  if (fd < 0) { drop_locked(); return; }
  logger.fd = fd;
  logger.bytes = 0;
}

void pbs_log_open(const char *path, int level, unsigned long max_bytes) {
  pbs_log_close();
  if (!path) return;
  size_t length = strlen(path);
  if (!length || length >= LOG_PATH_MAX) return;
  memcpy(logger.path, path, length + 1);
  logger.level = level < PBS_LOG_ERROR ? PBS_LOG_ERROR : (level > PBS_LOG_DEBUG ? PBS_LOG_DEBUG : level);
  logger.max_bytes = max_bytes ? max_bytes : PBS_LOG_DEFAULT_MAX_BYTES;
  logger.bytes = 0;
  logger.dropped = 0;
  started_ms = monotonic_ms();

  int fd = open(logger.path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) { logger.path[0] = '\0'; return; }  /* run on without a log */
  logger.fd = fd;
  struct stat st;
  if (!fstat(fd, &st) && st.st_size > 0) logger.bytes = (unsigned long)st.st_size;
  pbs_log_write(PBS_LOG_ERROR, "log opened epoch=%lld level=%s max_bytes=%lu",
                (long long)time(NULL), pbs_log_level_name(logger.level), logger.max_bytes);
}

void pbs_log_write(int level, const char *fmt, ...) {
  if (level > logger.level) return;              /* level is immutable here */
  char line[PBS_LOG_LINE_MAX];
  int head = snprintf(line, sizeof line, "[%lu] %-5s ", monotonic_ms() - started_ms,
                      pbs_log_level_name(level));
  if (head < 0 || (size_t)head >= sizeof line) head = 0;
  va_list args;
  va_start(args, fmt);
  int body = vsnprintf(line + head, sizeof line - (size_t)head - 1, fmt, args);
  va_end(args);
  if (body < 0) {
    pthread_mutex_lock(&logger.lock);
    drop_locked();
    pthread_mutex_unlock(&logger.lock);
    return;
  }
  size_t used = (size_t)head + (size_t)body;
  if (used > sizeof line - 2) used = sizeof line - 2;   /* truncated, still one line */
  line[used++] = '\n';

  pthread_mutex_lock(&logger.lock);
  if (logger.fd >= 0) {
    if (logger.bytes + used > logger.max_bytes) rotate_locked();
    if (logger.fd >= 0) {
      if (write_all(logger.fd, line, used)) drop_locked();
      else logger.bytes += used;
    }
  }
  pthread_mutex_unlock(&logger.lock);
}

unsigned pbs_log_dropped(void) {
  pthread_mutex_lock(&logger.lock);
  unsigned dropped = logger.dropped;
  pthread_mutex_unlock(&logger.lock);
  return dropped;
}

void pbs_log_close(void) {
  if (logger.fd >= 0) close(logger.fd);
  logger.fd = -1;
  logger.bytes = 0;
  logger.path[0] = '\0';
}
