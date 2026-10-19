#include "jwt_build.h"

#include <stdio.h>
#include <string.h>

const char JWT_HEADER_B64[] = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9";

bool jwt_b64url_encode(const uint8_t *in, size_t n, char *out, size_t out_sz)
{
    static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (JWT_B64URL_LEN(n) + 1 > out_sz) {
        return false;
    }
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = ALPHABET[(v >> 6) & 63];
        out[o++] = ALPHABET[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = ALPHABET[(v >> 6) & 63];
    }
    out[o] = '\0';
    return true;
}

int jwt_build_claims(char *out, size_t out_sz, const char *iss, const char *scope, const char *aud,
                     int64_t iat)
{
    int n = snprintf(out, out_sz, "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
                     iss, scope, aud, (long long)iat, (long long)(iat + JWT_LIFETIME_S));
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return n;
}

bool jwt_signing_input(char *out, size_t out_sz, const char *claims_json)
{
    size_t hl = strlen(JWT_HEADER_B64);
    size_t cl = strlen(claims_json);
    if (hl + 1 + JWT_B64URL_LEN(cl) + 1 > out_sz) {
        return false;
    }
    memcpy(out, JWT_HEADER_B64, hl);
    out[hl] = '.';
    return jwt_b64url_encode((const uint8_t *)claims_json, cl, out + hl + 1, out_sz - hl - 1);
}
