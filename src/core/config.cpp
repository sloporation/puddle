#include "config.h"

#include "yamltree.h"

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

static const char DEFAULT_CONFIG[] =
    "# Puddle configuration\n"
    "\n"
    "# IPv4 or IPv6 address to listen on. \"::\" listens on every interface (IPv4 and IPv6);\n"
    "# \"127.0.0.1\" and \"::1\" are local-only. Put IPv6 addresses in quotes.\n"
    "address: " DEFAULT_ADDRESS "\n"
    "\n"
    "# TCP port for the HTTPS listener.\n"
    "port: 8443\n"
    "\n"
    "# The address other people use to reach you, for your link: \"203.0.113.7:8443\", \"[2001:db8::1]:8443\"\n"
    "# or \"home.example.org:8443\". Needed when address above is \"::\" or 0.0.0.0; otherwise it defaults to\n"
    "# address and port.\n"
    "# advertise: 203.0.113.7:8443\n"
    "\n"
    "# Where chats, keys and profile images are stored. Uncomment to change the platform default.\n"
    "# data_dir: /path/to/puddle-data\n"
    "\n"
    "# Your profiles. One file, or a list of files; each file can hold several profiles.\n"
    "# Relative paths are relative to this file.\n"
    "#   profiles:\n"
    "#     - profiles.yaml\n"
    "#     - work-profiles.yaml\n"
    "profiles: profiles.yaml\n";

static const char DEFAULT_PROFILES_HEAD[] =
    "# Your profiles. Give each friend (each stream) the one that suits them.\n"
    "# The first profile is used for anyone you have not assigned one to.\n"
    "#\n"
    "# Fields (only id and display_name are required):\n"
    "#   id:           a short name only you see (letters, digits, - and _)\n"
    "#   display_name: what others see, up to 64 characters\n"
    "#   avatar:       profile picture: path to a png, jpg, gif or webp\n"
    "#   blurb:        a short line, up to 160 characters\n"
    "#   about:        a longer description, up to 2000 characters\n"
    "#   pronouns:     up to 32 characters\n"
    "#   color:        \"#3366ff\", or a gradient (see below)\n"
    "#   banner:       banner image path\n"
    "#   background:   profile background image path\n"
    "# Image paths are relative to this file, or start with ~/ for your home folder.\n"
    "\n"
    "profiles:\n"
    "  - id: default\n";

static const char DEFAULT_PROFILES_TAIL[] =
    "    # blurb: Hello!\n"
    "    # pronouns: they/them\n"
    "    # color: \"#3366ff\"\n"
    "    # color:\n"
    "    #   gradient: [\"#ff7a18\", \"#af002d\"]\n"
    "    #   angle: 135\n"
    "    # about: |\n"
    "    #   A longer description\n"
    "    #   over several lines.\n";

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
    w = snprintf(out, n, "%s\\puddle\\puddle.yaml", base);
#else
    const char *home = home_dir();
    if (!home)
        return -1;
    w = snprintf(out, n, "%s/.config/puddle/puddle.yaml", home);
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

int puddle_resolve_path(const char *base_dir, const char *value, char *out, size_t n)
{
    int w;
#ifndef _WIN32
    if (value[0] == '~' && (value[1] == '/' || value[1] == '\0')) {
        const char *home = home_dir();
        if (!home)
            return -1;
        w = snprintf(out, n, "%s%s", home, value + 1);
    } else if (value[0] == '/') {
        w = snprintf(out, n, "%s", value);
    } else {
        w = snprintf(out, n, "%s/%s", base_dir, value);
    }
#else
    if ((value[0] && value[1] == ':') || value[0] == '\\' || value[0] == '/')
        w = snprintf(out, n, "%s", value);
    else
        w = snprintf(out, n, "%s\\%s", base_dir, value);
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

void puddle_login_name(char *out, size_t n)
{
    const char *name = NULL;
#ifdef _WIN32
    name = getenv("USERNAME");
#else
    struct passwd *pw = getpwuid(getuid());
    if (pw)
        name = pw->pw_name;
    if (!name || !*name)
        name = getenv("USER");
#endif
    snprintf(out, n, "%s", (name && *name) ? name : "Me");
}

/* a YAML double-quoted string for `s` */
static void yaml_quote(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (n < 3) {
        if (n)
            out[0] = '\0';
        return;
    }
    out[o++] = '"';
    for (; *s && o + 3 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            out[o++] = '\\';
        if (c >= 0x20)
            out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = '\0';
}

static int write_defaults(const char *path, const char *dir, char *err, size_t errlen)
{
    if (make_parents(path, err, errlen) != 0)
        return -1;
    FILE *fp = fopen(path, "wx");
    if (!fp) {
        if (errno == EEXIST)
            return 0; /* lost a race with another process; use theirs */
        seterr(err, errlen, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    fputs(DEFAULT_CONFIG, fp);
    fclose(fp);

    char ppath[PUDDLE_PATH_MAX];
    if (puddle_resolve_path(dir, "profiles.yaml", ppath, sizeof ppath) == 0) {
        fp = fopen(ppath, "wx");
        if (fp) {
            char name[64], quoted[160];
            puddle_login_name(name, sizeof name);
            yaml_quote(name, quoted, sizeof quoted);
            fputs(DEFAULT_PROFILES_HEAD, fp);
            fprintf(fp, "    display_name: %s\n", quoted);
            fputs(DEFAULT_PROFILES_TAIL, fp);
            fclose(fp);
        }
    }
    return 0;
}

/* "[::1]" and "::" are not valid YAML unquoted; say so rather than leaving a cryptic parser message */
static void add_ipv6_hint(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "address:", 8) != 0)
            continue;
        const char *v = line + 8;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v != '"' && *v != '\'' && (*v == '[' || strchr(v, ':'))) {
            size_t used = strlen(err);
            if (used < errlen)
                snprintf(err + used, errlen - used, " (put IPv6 addresses in quotes, e.g. address: \"::1\")");
        }
        break;
    }
    fclose(f);
}

static int bad(char *err, size_t errlen, const char *path, const ynode *n, const char *what)
{
    seterr(err, errlen, "%s:%d: %s", path, ynode_line(n), what);
    return -1;
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

    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno != ENOENT) {
            seterr(err, errlen, "cannot read %s: %s", path, strerror(errno));
            return -1;
        }
        if (write_defaults(path, cfg->dir, err, errlen) != 0)
            return -1;
    }

    ynode *root = puddle_yaml_parse_file(path, err, errlen);
    if (!root) {
        add_ipv6_hint(path, err, errlen);
        return -1;
    }
    if (ynode_kind(root) != Y_MAP) {
        bad(err, errlen, path, root, "expected settings like \"port: 8443\"");
        goto fail;
    }

    for (size_t i = 0; i < ynode_count(root); i++) {
        const char *key = ynode_key(root, i);
        const ynode *v = ynode_item(root, i);
        const char *s = ynode_scalar(v);
        char what[160];

        if (strcmp(key, "address") == 0) {
            if (ynode_kind(v) == Y_SEQ) {
                bad(err, errlen, path, v, "address is a list; put IPv6 addresses in quotes, e.g. address: \"::1\"");
                goto fail;
            }
            if (!s || !*s || strlen(s) >= sizeof cfg->address) {
                bad(err, errlen, path, v, "address must be an IPv4 or IPv6 address");
                goto fail;
            }
            snprintf(cfg->address, sizeof cfg->address, "%s", s);
        } else if (strcmp(key, "port") == 0) {
            char *end = NULL;
            long p = s ? strtol(s, &end, 10) : 0;
            if (!s || !*s || *end != '\0' || p < 1 || p > 65535) {
                bad(err, errlen, path, v, "port must be a number from 1 to 65535");
                goto fail;
            }
            cfg->port = (uint16_t)p;
        } else if (strcmp(key, "advertise") == 0) {
            if (ynode_is_null(v))
                continue;
            if (!s || !*s || strlen(s) >= sizeof cfg->advertise || strpbrk(s, " \t/@?#")) {
                bad(err, errlen, path, v, "advertise must look like host:port, e.g. \"203.0.113.7:8443\"");
                goto fail;
            }
            snprintf(cfg->advertise, sizeof cfg->advertise, "%s", s);
        } else if (strcmp(key, "data_dir") == 0) {
            if (!s || !*s || puddle_resolve_path(cfg->dir, s, cfg->data_dir, sizeof cfg->data_dir) != 0) {
                bad(err, errlen, path, v, "data_dir must be a folder path");
                goto fail;
            }
            if (strlen(cfg->data_dir) > PUDDLE_DATA_DIR_MAX) {
                bad(err, errlen, path, v, "data_dir path is too long");
                goto fail;
            }
        } else if (strcmp(key, "profiles") == 0) {
            if (ynode_is_null(v))
                continue;
            size_t count = ynode_kind(v) == Y_SEQ ? ynode_count(v) : 1;
            if (count > PUDDLE_MAX_PROFILE_FILES) {
                snprintf(what, sizeof what, "at most %d profile files", PUDDLE_MAX_PROFILE_FILES);
                bad(err, errlen, path, v, what);
                goto fail;
            }
            for (size_t j = 0; j < count; j++) {
                const ynode *item = ynode_kind(v) == Y_SEQ ? ynode_item(v, j) : v;
                const char *f = ynode_scalar(item);
                if (!f || !*f ||
                    puddle_resolve_path(cfg->dir, f, cfg->profile_files[cfg->nprofile_files],
                                        sizeof cfg->profile_files[0]) != 0) {
                    bad(err, errlen, path, item, "profiles must be a file path, or a list of file paths");
                    goto fail;
                }
                cfg->nprofile_files++;
            }
        } else {
            snprintf(what, sizeof what, "unknown setting '%.60s'", key);
            bad(err, errlen, path, v, what);
            goto fail;
        }
    }
    puddle_yaml_free(root);
    return 0;
fail:
    puddle_yaml_free(root);
    return -1;
}
