// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Optional runtime configuration read from a plain `key=value` file.
 *
 * Every field starts "unset" so the caller can tell a configured value from a
 * default and let command-line arguments win over the file. Parsing never
 * aborts on a bad entry: the entry is skipped and reported through the issue
 * callback.
 *
 * Secrecy: issue messages carry the line number and the *key* only, never a
 * value, and pbs_config_describe() never prints auth_token. Callers must not
 * log cfg->auth_token themselves. */

#define PBS_CONFIG_MAX_BYTES 8192
#define PBS_CONFIG_MAX_LINE 255
#define PBS_CONFIG_TOKEN_MIN 8
#define PBS_CONFIG_TOKEN_MAX 64

typedef enum {
  PBS_LOG_ERROR = 0,
  PBS_LOG_WARN = 1,
  PBS_LOG_INFO = 2,
  PBS_LOG_DEBUG = 3
} pbs_log_level;

typedef struct {
  int port;            /* 1..65535, 0 = unset */
  int height;          /* 720, 1080, 1440 or 2160, 0 = unset */
  uint32_t bitrate;    /* bit/s, 1000000..100000000, 0 = unset */
  int minqp;           /* 18..35, 0 = unset */
  int audio;           /* 0 or 1, -1 = unset */
  int controller;      /* 0 or 1, -1 = unset */
  int log_level;       /* pbs_log_level, -1 = unset */
  int has_auth_token;  /* 1 when a valid token was read */
  char auth_token[PBS_CONFIG_TOKEN_MAX + 1];
} pbs_config_t;

/* message never contains a configuration value. */
typedef void (*pbs_config_issue_fn)(void *user, unsigned line, const char *message);

void pbs_config_init(pbs_config_t *cfg);

/* Parse `text` (not necessarily NUL-terminated). Later duplicates win.
 * Returns the number of rejected lines. */
int pbs_config_parse(pbs_config_t *cfg, const char *text, size_t length,
                     pbs_config_issue_fn issue, void *user);

/* Load a file. Returns 0 when read, 1 when it does not exist (cfg untouched),
 * -1 on read error or when the file exceeds PBS_CONFIG_MAX_BYTES. Rejected
 * lines are reported through `issue` and do not change the result. */
int pbs_config_load(pbs_config_t *cfg, const char *path,
                    pbs_config_issue_fn issue, void *user);

/* One-line summary that is safe to log: auth_token is shown as set/unset. */
void pbs_config_describe(const pbs_config_t *cfg, char *out, size_t size);

/* Name for a pbs_log_level, "error" when out of range. */
const char *pbs_log_level_name(int level);
