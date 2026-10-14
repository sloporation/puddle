#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char dir[512];

static void write_conf(const char *text)
{
    char path[600];
    snprintf(path, sizeof path, "%s/puddle.conf", dir);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/puddle-conf-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(dir))
        return 2;
    char path[600], err[256];
    snprintf(path, sizeof path, "%s/puddle.conf", dir);
    puddle_config c;

    /* missing file: created with defaults */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.address, "127.0.0.1") == 0 && c.port == 8443);
    CHECK(access(path, F_OK) == 0);
    CHECK(strcmp(c.dir, dir) == 0);
    char def[PUDDLE_PATH_MAX];
    CHECK(puddle_config_default_data_dir(def, sizeof def) == 0);
    CHECK(strcmp(c.data_dir, def) == 0);

    /* the file it wrote loads back */
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);

    write_conf("address = [::1]\nport = 9000\ndata_dir = /x/y\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.address, "::1") == 0 && c.port == 9000 && strcmp(c.data_dir, "/x/y") == 0);

    setenv("HOME", "/home/test", 1);
    write_conf("data_dir = ~/puddle-data\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == 0);
    CHECK(strcmp(c.data_dir, "/home/test/puddle-data") == 0);

    write_conf("prot = 1\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1 && strstr(err, "unknown key"));
    write_conf("port = 70000\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);
    write_conf("data_dir =\n");
    CHECK(puddle_config_load(&c, path, err, sizeof err) == -1);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    return failures ? 1 : 0;
}
