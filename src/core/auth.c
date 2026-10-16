#include "auth.h"

#include "config.h"
#include "encoding.h"
#include "fsutil.h"
#include "hash.h"
#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#define TOKEN_HEX 64

struct puddle_auth {
    char dir[PUDDLE_PATH_MAX];
    char path[PUDDLE_PATH_MAX];
    char token[TOKEN_HEX + 1];
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int valid_token(const char *t)
{
    unsigned char raw[32];
    return strlen(t) == TOKEN_HEX && puddle_hex_decode(t, raw, 32) == 0;
}

puddle_auth *puddle_auth_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_auth *a = calloc(1, sizeof *a);
    if (!a) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(a->dir, sizeof a->dir, "%s/state", data_dir);
    snprintf(a->path, sizeof a->path, "%s/owner-token", a->dir);
    if (puddle_mkdir_p(a->dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", a->dir, strerror(errno));
        goto fail;
    }

    FILE *f = fopen(a->path, "r");
    int existed = f != NULL;
    if (f) {
        char line[128] = "";
        if (fgets(line, sizeof line, f))
            line[strcspn(line, "\r\n")] = '\0';
        fclose(f);
        if (valid_token(line)) {
            memcpy(a->token, line, TOKEN_HEX + 1);
            memset(line, 0, sizeof line);
            return a;
        }
        memset(line, 0, sizeof line);
    }

    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof raw) != 1) {
        seterr(err, errlen, "cannot generate a token");
        goto fail;
    }
    puddle_hex_encode(raw, sizeof raw, a->token);
    memset(raw, 0, sizeof raw);
    char text[TOKEN_HEX + 2];
    snprintf(text, sizeof text, "%s\n", a->token);
    if (puddle_write_file_atomic(a->dir, a->path, text, TOKEN_HEX + 1) != 0) {
        seterr(err, errlen, "cannot write %s: %s", a->path, strerror(errno));
        goto fail;
    }
    memset(text, 0, sizeof text);
    puddle_log(existed ? PUDDLE_LOG_WARN : PUDDLE_LOG_INFO, "%s owner token in %s",
               existed ? "replaced a damaged" : "created an", a->path);
    return a;
fail:
    puddle_auth_close(a);
    return NULL;
}

void puddle_auth_close(puddle_auth *a)
{
    if (!a)
        return;
    memset(a->token, 0, sizeof a->token);
    free(a);
}

int puddle_auth_check_bearer(const puddle_auth *a, const char *authorization)
{
    if (!authorization || strncasecmp(authorization, "Bearer ", 7) != 0)
        return 0;
    const char *t = authorization + 7;
    if (strlen(t) != TOKEN_HEX)
        return 0;
    return CRYPTO_memcmp(t, a->token, TOKEN_HEX) == 0;
}

const char *puddle_auth_token_path(const puddle_auth *a) { return a->path; }
const char *puddle_auth_token(const puddle_auth *a) { return a->token; }
