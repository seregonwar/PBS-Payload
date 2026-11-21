// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/config/config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pbs_config_init(pbs_config_t *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->audio = -1;
  cfg->controller = -1;
  cfg->log_level = -1;
}

const char *pbs_log_level_name(int level) {
  switch (level) {
    case PBS_LOG_WARN: return "warn";
    case PBS_LOG_INFO: return "info";
    case PBS_LOG_DEBUG: return "debug";
    default: return "error";
  }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Digits only: no sign, no hex, no trailing text. */
static int parse_uint(const char *s, unsigned long max, unsigned long *out) {
  if (!*s) return 0;
  unsigned long value = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return 0;
    value = value * 10 + (unsigned long)(*s - '0');
    if (value > max) return 0;
  }
  *out = value;
  return 1;
}

static int parse_bool(const char *s, int *out) {
  if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "yes") || !strcmp(s, "true")) { *out = 1; return 1; }
  if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "no") || !strcmp(s, "false")) { *out = 0; return 1; }
  return 0;
}

static int token_valid(const char *s) {
  size_t length = strlen(s);
  if (length < PBS_CONFIG_TOKEN_MIN || length > PBS_CONFIG_TOKEN_MAX) return 0;
  for (; *s; s++)
    if ((unsigned char)*s < 0x21 || (unsigned char)*s > 0x7e) return 0;
  return 1;
}

/* Returns an error message (never containing the value) or NULL on success. */
static const char *apply(pbs_config_t *cfg, const char *key, const char *value, char *scratch, size_t size) {
  unsigned long n;
  if (!strcmp(key, "port")) {
    if (!parse_uint(value, 65535, &n) || n < 1) return "invalid value for 'port' (1..65535)";
    cfg->port = (int)n;
  } else if (!strcmp(key, "resolution")) {
    if (!strcmp(value, "720p")) cfg->height = 720;
    else if (!strcmp(value, "1080p")) cfg->height = 1080;
    else if (!strcmp(value, "1440p") || !strcmp(value, "2k")) cfg->height = 1440;
    else if (!strcmp(value, "2160p") || !strcmp(value, "4k")) cfg->height = 2160;
    else return "invalid value for 'resolution' (720p, 1080p, 1440p, 2160p)";
  } else if (!strcmp(key, "bitrate")) {
    if (!parse_uint(value, 100000000ul, &n) || n < 1000000ul) return "invalid value for 'bitrate' (1000000..100000000)";
    cfg->bitrate = (uint32_t)n;
  } else if (!strcmp(key, "minqp")) {
    if (!parse_uint(value, 35, &n) || n < 18) return "invalid value for 'minqp' (18..35)";
    cfg->minqp = (int)n;
  } else if (!strcmp(key, "audio")) {
    if (!parse_bool(value, &cfg->audio)) return "invalid value for 'audio' (on/off)";
  } else if (!strcmp(key, "controller")) {
    if (!parse_bool(value, &cfg->controller)) return "invalid value for 'controller' (on/off)";
  } else if (!strcmp(key, "log_level")) {
    if (!strcmp(value, "error")) cfg->log_level = PBS_LOG_ERROR;
    else if (!strcmp(value, "warn")) cfg->log_level = PBS_LOG_WARN;
    else if (!strcmp(value, "info")) cfg->log_level = PBS_LOG_INFO;
    else if (!strcmp(value, "debug")) cfg->log_level = PBS_LOG_DEBUG;
    else return "invalid value for 'log_level' (error, warn, info, debug)";
  } else if (!strcmp(key, "auth_token")) {
    if (!token_valid(value)) {
      snprintf(scratch, size, "invalid 'auth_token' (%d..%d printable characters, no spaces)",
               PBS_CONFIG_TOKEN_MIN, PBS_CONFIG_TOKEN_MAX);
      return scratch;
    }
    /* A later invalid token never replaces an earlier valid one. */
    memcpy(cfg->auth_token, value, strlen(value) + 1);
    cfg->has_auth_token = 1;
  } else {
    /* The key is not echoed: a bare `secret=` line must not leak. */
    return "unknown key";
  }
  return NULL;
}

int pbs_config_parse(pbs_config_t *cfg, const char *text, size_t length,
                     pbs_config_issue_fn issue, void *user) {
  int rejected = 0;
  unsigned line_number = 0;
  size_t pos = 0;
  while (pos < length) {
    const char *start = text + pos;
    const char *newline = memchr(start, '\n', length - pos);
    size_t line_length = newline ? (size_t)(newline - start) : length - pos;
    pos += line_length + (newline ? 1 : 0);
    line_number++;

    const char *reason = NULL;
    char message[96];
    char line[PBS_CONFIG_MAX_LINE + 1];
    if (line_length > PBS_CONFIG_MAX_LINE) {
      reason = "line too long";
    } else if (memchr(start, '\0', line_length)) {
      reason = "invalid character in line";
    } else {
      memcpy(line, start, line_length);
      line[line_length] = '\0';
      char *begin = line;
      while (is_space(*begin)) begin++;
      char *end = begin + strlen(begin);
      while (end > begin && is_space(end[-1])) end--;
      *end = '\0';
      if (!*begin || *begin == '#') continue;
      char *equals = strchr(begin, '=');
      if (!equals) {
        reason = "missing '='"; /* never echo the line: it may be a bare secret */
      } else {
        char *key_end = equals;
        while (key_end > begin && is_space(key_end[-1])) key_end--;
        *key_end = '\0';
        char *value = equals + 1;
        while (is_space(*value)) value++;
        if (!*begin) reason = "empty key";
        else reason = apply(cfg, begin, value, message, sizeof message);
      }
      /* Do not leave a copy of a possible token on the stack longer than needed. */
      memset(line, 0, sizeof line);
    }
    if (reason) {
      rejected++;
      if (issue) issue(user, line_number, reason);
    }
  }
  return rejected;
}

int pbs_config_load(pbs_config_t *cfg, const char *path,
                    pbs_config_issue_fn issue, void *user) {
  FILE *file = fopen(path, "rb");
  if (!file) return errno == ENOENT ? 1 : -1;
  static char buffer[PBS_CONFIG_MAX_BYTES + 1];
  size_t length = fread(buffer, 1, sizeof buffer, file);
  int failed = ferror(file);
  fclose(file);
  if (failed || length > PBS_CONFIG_MAX_BYTES) return -1;
  pbs_config_parse(cfg, buffer, length, issue, user);
  memset(buffer, 0, length);
  return 0;
}

static void field(char *out, size_t size, size_t *used, const char *name, long value, int unset) {
  if (*used >= size) return;
  int n = unset ? snprintf(out + *used, size - *used, "%s%s=default", *used ? " " : "", name)
                : snprintf(out + *used, size - *used, "%s%s=%ld", *used ? " " : "", name, value);
  if (n > 0) *used += (size_t)n < size - *used ? (size_t)n : size - *used;
}

void pbs_config_describe(const pbs_config_t *cfg, char *out, size_t size) {
  if (!size) return;
  out[0] = '\0';
  size_t used = 0;
  field(out, size, &used, "port", cfg->port, cfg->port == 0);
  field(out, size, &used, "height", cfg->height, cfg->height == 0);
  field(out, size, &used, "bitrate", cfg->bitrate, cfg->bitrate == 0);
  field(out, size, &used, "minqp", cfg->minqp, cfg->minqp == 0);
  field(out, size, &used, "audio", cfg->audio, cfg->audio < 0);
  field(out, size, &used, "controller", cfg->controller, cfg->controller < 0);
  if (used < size) {
    snprintf(out + used, size - used, "%slog_level=%s auth_token=%s", used ? " " : "",
             cfg->log_level < 0 ? "default" : pbs_log_level_name(cfg->log_level),
             cfg->has_auth_token ? "set" : "unset");
  }
}
