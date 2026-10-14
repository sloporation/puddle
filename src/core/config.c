#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP '\\'
#define make_dir(p) _mkdir(p)
#else
#include <pwd.h>
#include <unistd.h>
#define PATH_SEP '/'
#define make_dir(p) mkdir((p), 0700)
#endif

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8443

static const char DEFAULT_FILE[] =
    "# Puddle configuration\n"
    "\n"
    "# IPv4 or IPv6 address to listen on. \"::\" listens on every interface\n"
    "# (IPv4 and IPv6); \"127.0.0.1\" / \"::1\" are local-only.\n"
    "address = " DEFAULT_ADDRESS "\n"
    "\n"
    "# TCP port for the HTTPS listener.\n"
    "port = 8443\n"
    "\n"
    "# Where chats, keys and media are stored. Uncomment to change from the platform default.\n"
    "# data_dir = /path/to/puddle-data\n";

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

#ifndef _WIN32
static const char *home_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    return (home && *home) ? home : NULL;
}
#endif

int puddle_config_default_path(char *out, size_t n)
{
    int w;
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !*base)
        return -1;
    w = snprintf(out, n, "%s\\puddle\\puddle.conf", base);
#else
    const char *home = home_dir();
    if (!home)
        return -1;
    w = snprintf(out, n, "%s/.config/puddle/puddle.conf", home);
#endif
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int puddle_config_default_data_dir(char *out, size_t n)
{
    int w;
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base)
        return -1;
    w = snprintf(out, n, "%s\\puddle", base);
#elif defined(__APPLE__)
    const char *home = home_dir();
    if (!home)
        return -1;
    w = snprintf(out, n, "%s/Library/Application Support/Puddle", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = home_dir();
    if (xdg && xdg[0] == '/')
        w = snprintf(out, n, "%s/puddle", xdg);
    else if (home)
        w = snprintf(out, n, "%s/.local/share/puddle", home);
    else
        return -1;
#endif
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

/* mkdir -p for the parent of `file`. */
static int make_parents(const char *file, char *err, size_t errlen)
{
    char buf[PUDDLE_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", file);
    for (char *p = buf + 1; *p; p++) {
        if (*p != PATH_SEP)
            continue;
        *p = '\0';
        if (make_dir(buf) != 0 && errno != EEXIST) {
            seterr(err, errlen, "cannot create %s: %s", buf, strerror(errno));
            return -1;
        }
        *p = PATH_SEP;
    }
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

int puddle_config_load(puddle_config *cfg, const char *path, char *err, size_t errlen)
{
    char defpath[PUDDLE_PATH_MAX];
    if (!path) {
        if (puddle_config_default_path(defpath, sizeof defpath) != 0) {
            seterr(err, errlen, "cannot determine config directory (HOME/APPDATA unset)");
            return -1;
        }
        path = defpath;
    }

    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->address, sizeof cfg->address, "%s", DEFAULT_ADDRESS);
    cfg->port = DEFAULT_PORT;
    if (puddle_config_default_data_dir(cfg->data_dir, sizeof cfg->data_dir) != 0) {
        seterr(err, errlen, "cannot determine data directory (HOME/LOCALAPPDATA unset)");
        return -1;
    }

    /* directory = everything before the last separator */
    snprintf(cfg->dir, sizeof cfg->dir, "%s", path);
    char *slash = strrchr(cfg->dir, PATH_SEP);
    if (slash)
        *slash = '\0';
    else
        snprintf(cfg->dir, sizeof cfg->dir, ".");

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            seterr(err, errlen, "cannot read %s: %s", path, strerror(errno));
            return -1;
        }
        if (make_parents(path, err, errlen) != 0)
            return -1;
        fp = fopen(path, "wx");
        if (!fp) {
            seterr(err, errlen, "cannot create %s: %s", path, strerror(errno));
            return -1;
        }
        fputs(DEFAULT_FILE, fp);
        fclose(fp);
        return 0; /* defaults already set */
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            seterr(err, errlen, "%s:%d: expected key = value", path, lineno);
            goto bad;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "address") == 0) {
            size_t vl = strlen(val);
            if (vl >= 2 && val[0] == '[' && val[vl - 1] == ']') { /* [::1] form */
                val[vl - 1] = '\0';
                val++;
            }
            if (!*val || strlen(val) >= sizeof cfg->address) {
                seterr(err, errlen, "%s:%d: invalid address", path, lineno);
                goto bad;
            }
            snprintf(cfg->address, sizeof cfg->address, "%s", val);
        } else if (strcmp(key, "port") == 0) {
            char *end;
            long p = strtol(val, &end, 10);
            if (*val == '\0' || *end != '\0' || p < 1 || p > 65535) {
                seterr(err, errlen, "%s:%d: port must be 1-65535", path, lineno);
                goto bad;
            }
            cfg->port = (uint16_t)p;
        } else if (strcmp(key, "data_dir") == 0) {
            if (!*val || strlen(val) >= sizeof cfg->data_dir) {
                seterr(err, errlen, "%s:%d: invalid data_dir", path, lineno);
                goto bad;
            }
#ifndef _WIN32
            const char *home = home_dir();
            if (val[0] == '~' && val[1] == '/' && home)
                snprintf(cfg->data_dir, sizeof cfg->data_dir, "%s/%s", home, val + 2);
            else
#endif
                snprintf(cfg->data_dir, sizeof cfg->data_dir, "%s", val);
        } else {
            seterr(err, errlen, "%s:%d: unknown key '%s'", path, lineno, key);
            goto bad;
        }
    }
    fclose(fp);
    return 0;
bad:
    fclose(fp);
    return -1;
}
