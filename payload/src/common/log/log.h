// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include "common/config/config.h"   /* pbs_log_level, pbs_log_level_name */

/* Levelled operational log for the payload's control paths.
 *
 * This is not a replacement for common/diagnostics/problem_log.h. The problem
 * index stays the single recorder inside the capture loop: it is
 * single-threaded by design, uses a dedicated descriptor and is safe from a
 * crash handler. This layer is for control-path events that several threads
 * can emit (startup, configuration, HTTP server lifecycle), so it takes a
 * mutex and must never be called per frame.
 *
 * Failure policy: every entry point returns void and no failure is ever
 * propagated. A log that cannot be opened, written or rotated is abandoned
 * and the payload continues; pbs_log_dropped() counts the lost records.
 *
 * Threading: pbs_log_open() and pbs_log_close() are NOT thread-safe and must
 * run before any logging thread starts and after they have all joined. The
 * level is fixed by pbs_log_open(), which is what lets pbs_log_write() filter
 * without taking the lock. pbs_log_write() is safe from any thread and holds
 * the lock only around the rotation check and the write itself; the record is
 * formatted before the lock is taken. */

#define PBS_LOG_LINE_MAX 512
#define PBS_LOG_DEFAULT_MAX_BYTES (256u * 1024u)

/* Open (append) `path` at `level` and stamp a header. `max_bytes` is the size
 * at which the log is rotated to "<path>.1"; 0 selects the default. Replaces
 * any previously open log. Silently does nothing useful on failure. */
void pbs_log_open(const char *path, int level, unsigned long max_bytes);

/* One record. Never writes a configuration secret by itself: callers must not
 * pass pbs_config_t::auth_token, and pbs_config_describe() is the safe way to
 * record what was configured. */
void pbs_log_write(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define pbs_log_error(...) pbs_log_write(PBS_LOG_ERROR, __VA_ARGS__)
#define pbs_log_warn(...)  pbs_log_write(PBS_LOG_WARN, __VA_ARGS__)
#define pbs_log_info(...)  pbs_log_write(PBS_LOG_INFO, __VA_ARGS__)
#define pbs_log_debug(...) pbs_log_write(PBS_LOG_DEBUG, __VA_ARGS__)

/* Records lost to a formatting, write or rotation failure since the log was
 * opened. Stops counting at UINT_MAX. */
unsigned pbs_log_dropped(void);

void pbs_log_close(void);
