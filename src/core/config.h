#ifndef PUDDLE_CONFIG_H
#define PUDDLE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PATH_MAX 1024

typedef struct {
    char address[64];          /* IPv4 or IPv6 literal; "::" = all interfaces */
    uint16_t port;
    char dir[PUDDLE_PATH_MAX]; /* directory of the config file; TLS material lives here */
} puddle_config;

/* ~/.config/puddle/puddle.conf on macOS/Linux, %APPDATA%\puddle\puddle.conf on Windows. */
int puddle_config_default_path(char *out, size_t n);

/* Load `path` (NULL = default path). A missing file is created with defaults.
 * Returns 0 on success, -1 with a message in `err` otherwise. */
int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
