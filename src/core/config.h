#ifndef PUDDLE_CONFIG_H
#define PUDDLE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PATH_MAX 1024
#define PUDDLE_DATA_DIR_MAX (PUDDLE_PATH_MAX - 128) /* leaves room for the names Puddle creates under it */
#define PUDDLE_MAX_PROFILE_FILES 8

typedef struct {
    char address[64];          /* IPv4 or IPv6 literal; "::" = all interfaces */
    uint16_t port;
    char advertise[128];       /* host:port others should use to reach this Puddle; "" = work it out from address/port */
    char dir[PUDDLE_PATH_MAX]; /* directory of the config file; TLS material lives here */
    char data_dir[PUDDLE_PATH_MAX]; /* chats, keys, profile images, media (docs/storage.md) */
    char profile_files[PUDDLE_MAX_PROFILE_FILES][PUDDLE_PATH_MAX]; /* absolute or config-relative, resolved */
    size_t nprofile_files;
} puddle_config;

/* ~/.config/puddle/puddle.yaml on macOS/Linux, %APPDATA%\puddle\puddle.yaml on Windows. */
int puddle_config_default_path(char *out, size_t n);

/* ~/Library/Application Support/Puddle (macOS), $XDG_DATA_HOME or ~/.local/share/puddle (Linux),
 * %LOCALAPPDATA%\puddle (Windows). */
int puddle_config_default_data_dir(char *out, size_t n);

/* Load `path` (NULL = default path). A missing file is created with defaults, along with a starter
 * profiles.yaml next to it. Returns 0 on success, -1 with a message in `err` otherwise. */
int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen);

/* The OS account name (for the starter profile); "Me" if it can't be found. */
void puddle_login_name(char *out, size_t n);

/* Turn a path from a config or profile file into an absolute one: "~/x" is the home directory,
 * absolute paths are kept, anything else is relative to `base_dir`. 0 or -1 if it does not fit. */
int puddle_resolve_path(const char *base_dir, const char *value, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
