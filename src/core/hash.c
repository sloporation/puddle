#include "hash.h"

#include <openssl/evp.h>

void puddle_hex_encode(const unsigned char *bytes, unsigned len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < len; i++) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[2 * len] = '\0';
}

void puddle_sha256_hex(const void *data, size_t len, char out[65])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL);
    puddle_hex_encode(md, mdlen, out);
}
