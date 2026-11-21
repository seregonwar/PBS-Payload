// SPDX-License-Identifier: GPL-3.0-or-later
/* Host regression for the configuration parser. No console, no network. */
#include "common/config/config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SECRET "s3cr3t-TOKEN-value"

typedef struct { unsigned lines[32]; char messages[32][128]; int count; } issues_t;

static void collect(void *user, unsigned line, const char *message) {
  issues_t *issues = user;
  assert(issues->count < 32);
  issues->lines[issues->count] = line;
  snprintf(issues->messages[issues->count], sizeof issues->messages[0], "%s", message);
  issues->count++;
}

static int parse(pbs_config_t *cfg, issues_t *issues, const char *text) {
  pbs_config_init(cfg);
  memset(issues, 0, sizeof *issues);
  return pbs_config_parse(cfg, text, strlen(text), collect, issues);
}

static void verify_defaults(void) {
  pbs_config_t cfg;
  pbs_config_init(&cfg);
  assert(cfg.port == 0 && cfg.height == 0 && cfg.bitrate == 0 && cfg.minqp == 0);
  assert(cfg.audio == -1 && cfg.controller == -1 && cfg.log_level == -1);
  assert(!cfg.has_auth_token);
  issues_t issues;
  assert(parse(&cfg, &issues, "") == 0);
  assert(parse(&cfg, &issues, "\n\n# only a comment\n   \n") == 0);
  assert(cfg.port == 0 && issues.count == 0);
}

static void verify_valid_values(void) {
  pbs_config_t cfg; issues_t issues;
  int rejected = parse(&cfg, &issues,
    "# pbs.conf\n"
    "port = 8080\n"
    "resolution=1080p\r\n"
    "  bitrate=30000000  \n"
    "minqp=22\n"
    "audio=off\n"
    "controller=yes\n"
    "log_level=debug\n"
    "auth_token=" SECRET);          /* last line without a newline */
  assert(rejected == 0 && issues.count == 0);
  assert(cfg.port == 8080 && cfg.height == 1080 && cfg.bitrate == 30000000u);
  assert(cfg.minqp == 22 && cfg.audio == 0 && cfg.controller == 1);
  assert(cfg.log_level == PBS_LOG_DEBUG);
  assert(cfg.has_auth_token && !strcmp(cfg.auth_token, SECRET));
}

static void verify_resolution_aliases(void) {
  pbs_config_t cfg; issues_t issues;
  assert(!parse(&cfg, &issues, "resolution=720p\n") && cfg.height == 720);
  assert(!parse(&cfg, &issues, "resolution=2k\n") && cfg.height == 1440);
  assert(!parse(&cfg, &issues, "resolution=4k\n") && cfg.height == 2160);
  assert(parse(&cfg, &issues, "resolution=480p\n") == 1 && cfg.height == 0);
}

static void verify_ranges(void) {
  pbs_config_t cfg; issues_t issues;
  /* Same bounds the command line enforces today. */
  assert(!parse(&cfg, &issues, "port=1\n") && cfg.port == 1);
  assert(!parse(&cfg, &issues, "port=65535\n") && cfg.port == 65535);
  assert(parse(&cfg, &issues, "port=0\n") == 1 && cfg.port == 0);
  assert(parse(&cfg, &issues, "port=65536\n") == 1 && cfg.port == 0);
  assert(!parse(&cfg, &issues, "bitrate=1000000\n") && cfg.bitrate == 1000000u);
  assert(!parse(&cfg, &issues, "bitrate=100000000\n") && cfg.bitrate == 100000000u);
  assert(parse(&cfg, &issues, "bitrate=999999\n") == 1 && cfg.bitrate == 0);
  assert(parse(&cfg, &issues, "bitrate=100000001\n") == 1 && cfg.bitrate == 0);
  assert(!parse(&cfg, &issues, "minqp=18\n") && cfg.minqp == 18);
  assert(!parse(&cfg, &issues, "minqp=35\n") && cfg.minqp == 35);
  assert(parse(&cfg, &issues, "minqp=17\n") == 1 && cfg.minqp == 0);
  assert(parse(&cfg, &issues, "minqp=36\n") == 1 && cfg.minqp == 0);
}

static void verify_malformed_numbers(void) {
  pbs_config_t cfg; issues_t issues;
  assert(parse(&cfg, &issues, "port=80x\n") == 1);
  assert(parse(&cfg, &issues, "port=-1\n") == 1);
  assert(parse(&cfg, &issues, "port=0x50\n") == 1);
  assert(parse(&cfg, &issues, "port=\n") == 1);
  assert(parse(&cfg, &issues, "port=99999999999999999999\n") == 1);
  assert(cfg.port == 0);
}

static void verify_booleans_and_levels(void) {
  pbs_config_t cfg; issues_t issues;
  assert(!parse(&cfg, &issues, "audio=true\ncontroller=0\n") && cfg.audio == 1 && cfg.controller == 0);
  assert(parse(&cfg, &issues, "audio=maybe\n") == 1 && cfg.audio == -1);
  assert(!parse(&cfg, &issues, "log_level=warn\n") && cfg.log_level == PBS_LOG_WARN);
  assert(parse(&cfg, &issues, "log_level=trace\n") == 1 && cfg.log_level == -1);
  assert(!strcmp(pbs_log_level_name(PBS_LOG_INFO), "info"));
  assert(!strcmp(pbs_log_level_name(99), "error"));
}

static void verify_bad_lines_are_skipped_and_numbered(void) {
  pbs_config_t cfg; issues_t issues;
  int rejected = parse(&cfg, &issues,
    "port=9000\n"            /* 1 ok */
    "no equals sign\n"       /* 2 */
    "=value\n"               /* 3 */
    "mystery=1\n"            /* 4 */
    "minqp=99\n"             /* 5 */
    "bitrate=40000000\n");   /* 6 ok: parsing continues after errors */
  assert(rejected == 4 && issues.count == 4);
  assert(issues.lines[0] == 2 && issues.lines[1] == 3 && issues.lines[2] == 4 && issues.lines[3] == 5);
  assert(cfg.port == 9000 && cfg.bitrate == 40000000u && cfg.minqp == 0);
  assert(strstr(issues.messages[3], "minqp"));
  pbs_config_init(&cfg);
  assert(pbs_config_parse(&cfg, "port=abc\n", 9, NULL, NULL) == 1);   /* NULL callback is tolerated */
}

static void verify_later_duplicate_wins(void) {
  pbs_config_t cfg; issues_t issues;
  assert(!parse(&cfg, &issues, "port=8000\nport=8001\n") && cfg.port == 8001);
  /* A later invalid value does not erase an earlier valid one. */
  assert(parse(&cfg, &issues, "port=8000\nport=bad\n") == 1 && cfg.port == 8000);
}

static void verify_line_limits(void) {
  pbs_config_t cfg; issues_t issues;
  char text[600];
  memset(text, 'a', sizeof text);
  memcpy(text, "port=", 5);
  text[PBS_CONFIG_MAX_LINE + 1] = '\n';        /* 256-byte line */
  text[PBS_CONFIG_MAX_LINE + 2] = '\0';
  assert(parse(&cfg, &issues, text) == 1 && !strcmp(issues.messages[0], "line too long"));
  assert(cfg.port == 0);
  /* An embedded NUL must not truncate silently into a valid-looking line. */
  pbs_config_init(&cfg);
  assert(pbs_config_parse(&cfg, "port=80\0" "90\n", 10, NULL, NULL) == 1 && cfg.port == 0);
}

static void verify_token_rules(void) {
  pbs_config_t cfg; issues_t issues;
  assert(!parse(&cfg, &issues, "auth_token=12345678\n") && cfg.has_auth_token);
  assert(!parse(&cfg, &issues, "auth_token=ab#cd=ef==\n") && cfg.has_auth_token);  /* '#' and '=' are allowed */
  assert(parse(&cfg, &issues, "auth_token=short\n") == 1 && !cfg.has_auth_token);
  assert(parse(&cfg, &issues, "auth_token=has space inside\n") == 1 && !cfg.has_auth_token);
  char long_token[80] = "auth_token=";
  memset(long_token + 11, 'x', 65); long_token[76] = '\n'; long_token[77] = '\0';
  assert(parse(&cfg, &issues, long_token) == 1 && !cfg.has_auth_token);
  /* An invalid replacement keeps the earlier valid token. */
  assert(parse(&cfg, &issues, "auth_token=" SECRET "\nauth_token=bad\n") == 1);
  assert(cfg.has_auth_token && !strcmp(cfg.auth_token, SECRET));
}

/* The token must reach neither an issue message nor the describe() summary,
 * even when it sits in a malformed or unknown line. */
static void verify_token_never_leaks(void) {
  static const char *const hostile[] = {
    "auth_token=" SECRET "\n",
    "auth_token=" SECRET " trailing space\n",
    SECRET "\n",                               /* bare secret, no '=' */
    SECRET "==\n",                             /* base64-looking key, empty value */
    SECRET "=1\n",                             /* secret used as an unknown key */
    "port=" SECRET "\n",                       /* secret pasted into another field */
    "log_level=" SECRET "\n",
    "resolution=" SECRET "\n",
  };
  for (size_t i = 0; i < sizeof hostile / sizeof *hostile; i++) {
    pbs_config_t cfg; issues_t issues;
    parse(&cfg, &issues, hostile[i]);
    for (int j = 0; j < issues.count; j++) assert(!strstr(issues.messages[j], SECRET));
    char summary[256];
    pbs_config_describe(&cfg, summary, sizeof summary);
    assert(!strstr(summary, SECRET));
  }
  pbs_config_t cfg; issues_t issues;
  parse(&cfg, &issues, "auth_token=" SECRET "\nport=8080\n");
  char summary[256];
  pbs_config_describe(&cfg, summary, sizeof summary);
  assert(strstr(summary, "auth_token=set") && strstr(summary, "port=8080"));
  assert(!strstr(summary, SECRET));
  pbs_config_init(&cfg);
  pbs_config_describe(&cfg, summary, sizeof summary);
  assert(strstr(summary, "auth_token=unset") && strstr(summary, "port=default"));
  char tiny[8];                                  /* truncation must stay NUL-terminated */
  pbs_config_describe(&cfg, tiny, sizeof tiny);
  assert(strlen(tiny) < sizeof tiny);
}

static void write_file(const char *path, const void *data, size_t length) {
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(data, 1, length, f) == length);
  fclose(f);
}

static void verify_load(void) {
  const char *path = "pbs_config_test.tmp";   /* written in the working directory, removed below */
  pbs_config_t cfg; issues_t issues;

  memset(&issues, 0, sizeof issues);
  pbs_config_init(&cfg);
  const char good[] = "port=7000\nbogus\nauth_token=" SECRET "\n";
  write_file(path, good, sizeof good - 1);
  assert(pbs_config_load(&cfg, path, collect, &issues) == 0);
  assert(cfg.port == 7000 && cfg.has_auth_token && issues.count == 1 && issues.lines[0] == 2);

  /* Exactly the limit loads; one byte more is refused. */
  static char big[PBS_CONFIG_MAX_BYTES + 1];
  memset(big, '#', sizeof big);
  write_file(path, big, PBS_CONFIG_MAX_BYTES);
  pbs_config_init(&cfg);
  assert(pbs_config_load(&cfg, path, NULL, NULL) == 0);
  write_file(path, big, PBS_CONFIG_MAX_BYTES + 1);
  assert(pbs_config_load(&cfg, path, NULL, NULL) == -1);

  assert(remove(path) == 0);
  pbs_config_init(&cfg);
  assert(pbs_config_load(&cfg, path, NULL, NULL) == 1);   /* missing file is not an error */
  assert(cfg.port == 0 && !cfg.has_auth_token);
  assert(pbs_config_load(&cfg, ".", NULL, NULL) == -1);   /* a directory is a read error */
}

int main(void) {
  verify_defaults();
  verify_valid_values();
  verify_resolution_aliases();
  verify_ranges();
  verify_malformed_numbers();
  verify_booleans_and_levels();
  verify_bad_lines_are_skipped_and_numbered();
  verify_later_duplicate_wins();
  verify_line_limits();
  verify_token_rules();
  verify_token_never_leaks();
  verify_load();
  puts("config tests passed");
  return 0;
}
