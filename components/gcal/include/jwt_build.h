#pragma once
/*
 * The text-building half of the service-account JWT (RFC 7515/7519): base64url, the claims
 * JSON and the "header.claims" signing input. Pure C with no ESP-IDF or mbedtls calls, so it
 * is unit-tested on a computer (tests/host); jwt_auth.c signs the result and does the HTTP
 * exchange.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JWT_TOKEN_URL      "https://oauth2.googleapis.com/token"
#define JWT_CALENDAR_SCOPE "https://www.googleapis.com/auth/calendar.readonly"
#define JWT_LIFETIME_S     3600

/* {"alg":"RS256","typ":"JWT"} and its base64url form - the same for every request. */
#define JWT_HEADER_JSON "{\"alg\":\"RS256\",\"typ\":\"JWT\"}"
extern const char JWT_HEADER_B64[];

/* Length of the unpadded base64url form of n bytes (without the NUL). */
#define JWT_B64URL_LEN(n) (((n) * 4 + 2) / 3)

/* base64url, no padding ('-' and '_' for '+' and '/'), NUL-terminated. Returns false, writing
 * nothing, if JWT_B64URL_LEN(n) + 1 bytes don't fit in out_sz. */
bool jwt_b64url_encode(const uint8_t *in, size_t n, char *out, size_t out_sz);

/* {"iss":...,"scope":...,"aud":...,"iat":iat,"exp":iat+JWT_LIFETIME_S}. Returns the length,
 * or -1 if it (and its NUL) doesn't fit in out_sz. The values are put in as they are, so
 * they must not contain '"' or '\\'. */
int jwt_build_claims(char *out, size_t out_sz, const char *iss, const char *scope, const char *aud,
                     int64_t iat);

/* "<header>.<base64url(claims_json)>" - the bytes that get signed. False if it doesn't fit. */
bool jwt_signing_input(char *out, size_t out_sz, const char *claims_json);

#ifdef __cplusplus
}
#endif
