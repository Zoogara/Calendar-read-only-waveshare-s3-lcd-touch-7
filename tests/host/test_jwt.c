/* jwt_build.c: base64url, the claims JSON and the signing input. (Signing itself needs mbedtls,
 * which isn't available to this host build, so it is not covered here.) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_settings.h"
#include "jwt_build.h"
#include "test.h"

#define EMAIL "reader@demo-project.iam.gserviceaccount.invalid"

/* A decoder to round-trip against, written independently of the encoder. */
static int b64url_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int b64url_decode(const char *s, uint8_t *out)
{
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *s; s++) {
        int v = b64url_val(*s);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return n;
}

static void b64url_rfc4648_vectors(void)
{
    static const struct { const char *in, *want; } V[] = {
        {"", ""}, {"f", "Zg"}, {"fo", "Zm8"}, {"foo", "Zm9v"}, {"foob", "Zm9vYg"},
        {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"},
    };
    char out[16];
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        CHECK(jwt_b64url_encode((const uint8_t *)V[i].in, strlen(V[i].in), out, sizeof(out)));
        CHECK_STR(out, V[i].want);
        CHECK(strchr(out, '=') == NULL);               /* never padded */
    }
}

static void b64url_alphabet(void)
{
    char out[16];
    /* standard base64 of these three bytes is "+//+" */
    static const uint8_t b[] = {0xfb, 0xff, 0xfe};
    CHECK(jwt_b64url_encode(b, 3, out, sizeof(out)));
    CHECK_STR(out, "-__-");
    /* and none of the output of any byte value is outside the URL-safe alphabet */
    uint8_t all[256];
    char big[400];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    CHECK(jwt_b64url_encode(all, sizeof(all), big, sizeof(big)));
    for (const char *p = big; *p; p++) {
        CHECK(b64url_val(*p) >= 0);
    }
}

static void b64url_round_trip_and_length(void)
{
    uint8_t in[64], back[64];
    char out[128];
    for (size_t n = 0; n <= 40; n++) {
        for (size_t i = 0; i < n; i++) {
            in[i] = (uint8_t)(i * 37 + n * 11 + 5);
        }
        CHECK(jwt_b64url_encode(in, n, out, sizeof(out)));
        CHECK_INT(strlen(out), JWT_B64URL_LEN(n));
        CHECK_INT(b64url_decode(out, back), (int)n);
        CHECK(memcmp(in, back, n) == 0);
    }
}

static void b64url_exact_buffer_limit(void)
{
    /* Guard bytes on both sides of a buffer: nothing may be written past out_sz. */
    uint8_t in[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (size_t n = 0; n <= sizeof(in); n++) {
        size_t need = JWT_B64URL_LEN(n) + 1;
        char mem[64];
        memset(mem, 0x5A, sizeof(mem));
        char *out = mem + 8;
        CHECK(jwt_b64url_encode(in, n, out, need));                  /* exactly enough */
        CHECK_INT(strlen(out), need - 1);
        CHECK(mem[8 + need] == 0x5A);

        memset(mem, 0x5A, sizeof(mem));
        CHECK(!jwt_b64url_encode(in, n, out, need - 1));             /* one byte short */
        for (size_t i = 0; i < 8 + need; i++) {
            CHECK(mem[i] == 0x5A);                                   /* nothing was written */
        }
    }
    char one[1];
    CHECK(!jwt_b64url_encode(in, 1, one, sizeof(one)));
}

static void header_constant(void)
{
    char out[64];
    CHECK(jwt_b64url_encode((const uint8_t *)JWT_HEADER_JSON, strlen(JWT_HEADER_JSON), out, sizeof(out)));
    CHECK_STR(out, JWT_HEADER_B64);
    CHECK_STR(JWT_HEADER_JSON, "{\"alg\":\"RS256\",\"typ\":\"JWT\"}");
}

static long long field_num(const char *json, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    long long v = -1;
    if (p) {
        sscanf(p + strlen(pat), "%lld", &v);
    }
    return v;
}

static void claims_content(void)
{
    char json[384];
    int n = jwt_build_claims(json, sizeof(json), EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL);
    CHECK(n > 0);
    CHECK_INT(n, strlen(json));
    CHECK(strstr(json, "\"iss\":\"" EMAIL "\"") != NULL);
    CHECK(strstr(json, "\"scope\":\"https://www.googleapis.com/auth/calendar.readonly\"") != NULL);
    CHECK(strstr(json, "\"aud\":\"https://oauth2.googleapis.com/token\"") != NULL);
    CHECK_INT(field_num(json, "iat"), 1760000000LL);
    CHECK_INT(field_num(json, "exp"), 1760003600LL);
    CHECK_INT(field_num(json, "exp") - field_num(json, "iat"), 3600);
    CHECK(json[0] == '{' && json[n - 1] == '}');

    /* a time past 2038 must not wrap */
    n = jwt_build_claims(json, sizeof(json), EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 4102444800LL);
    CHECK(n > 0);
    CHECK_INT(field_num(json, "exp") - field_num(json, "iat"), 3600);
    CHECK_INT(field_num(json, "iat"), 4102444800LL);
}

static void claims_buffer_limits(void)
{
    char json[384];
    int n = jwt_build_claims(json, sizeof(json), EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL);
    /* exactly n + 1 bytes fits; one less does not, and nothing is written past the end */
    char mem[512];
    memset(mem, 0x5A, sizeof(mem));
    CHECK_INT(jwt_build_claims(mem, (size_t)n + 1, EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL), n);
    CHECK(mem[n + 1] == 0x5A);
    memset(mem, 0x5A, sizeof(mem));
    CHECK_INT(jwt_build_claims(mem, (size_t)n, EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL), -1);
    CHECK(mem[n + 1] == 0x5A);
    CHECK_INT(jwt_build_claims(mem, 0, EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL), -1);
}

/* The firmware's own buffers (see build_signed_jwt in jwt_auth.c). */
static void longest_allowed_email_fits_the_firmwares_buffers(void)
{
    char email[APP_SETTINGS_MAX_STR];
    memset(email, 'e', sizeof(email) - 1);
    email[sizeof(email) - 1] = '\0';

    char claims[384], input[600];
    int n = jwt_build_claims(claims, sizeof(claims), email, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL);
    CHECK(n > 0);
    CHECK(jwt_signing_input(input, sizeof(input), claims));
    CHECK(strlen(input) < sizeof(input));

    /* and with a signature up to a 4096-bit key, the 700-byte buffer the firmware uses */
    uint8_t sig[512];
    char sig_b64[700];
    memset(sig, 0xA5, sizeof(sig));
    CHECK(jwt_b64url_encode(sig, sizeof(sig), sig_b64, sizeof(sig_b64)));
    CHECK_INT(strlen(sig_b64), 683);
    CHECK(strlen(input) + 1 + strlen(sig_b64) < 1200);   /* the caller's jwt buffer */
}

static void oversized_email_is_refused_not_overrun(void)
{
    char email[500], claims[384], input[600];
    for (size_t len = 1; len < sizeof(email); len += 7) {
        memset(email, 'e', len);
        email[len] = '\0';
        memset(claims, 0x5A, sizeof(claims));
        int n = jwt_build_claims(claims, sizeof(claims), email, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL);
        if (n < 0) {
            CHECK(len > 150);                       /* only an unusually long address is refused */
            continue;
        }
        CHECK_INT(n, strlen(claims));
        CHECK(jwt_signing_input(input, sizeof(input), claims));
    }
}

static void signing_input_structure(void)
{
    char claims[384], input[600], back[400];
    int n = jwt_build_claims(claims, sizeof(claims), EMAIL, JWT_CALENDAR_SCOPE, JWT_TOKEN_URL, 1760000000LL);
    CHECK(jwt_signing_input(input, sizeof(input), claims));

    const char *dot = strchr(input, '.');
    CHECK(dot != NULL && strchr(dot + 1, '.') == NULL);          /* header.claims, one dot */
    CHECK_INT(dot - input, strlen(JWT_HEADER_B64));
    CHECK(memcmp(input, JWT_HEADER_B64, strlen(JWT_HEADER_B64)) == 0);
    CHECK(strchr(input, '=') == NULL && strchr(input, '+') == NULL && strchr(input, '/') == NULL);
    int m = b64url_decode(dot + 1, (uint8_t *)back);
    CHECK_INT(m, n);
    back[m > 0 ? m : 0] = '\0';
    CHECK_STR(back, claims);

    /* exact fit, then one byte short */
    size_t need = strlen(input) + 1;
    char mem[700];
    memset(mem, 0x5A, sizeof(mem));
    CHECK(jwt_signing_input(mem, need, claims));
    CHECK(mem[need] == 0x5A);
    memset(mem, 0x5A, sizeof(mem));
    CHECK(!jwt_signing_input(mem, need - 1, claims));
    CHECK(mem[need - 1] == 0x5A);
}

void test_jwt(void)
{
    RUN(b64url_rfc4648_vectors);
    RUN(b64url_alphabet);
    RUN(b64url_round_trip_and_length);
    RUN(b64url_exact_buffer_limit);
    RUN(header_constant);
    RUN(claims_content);
    RUN(claims_buffer_limits);
    RUN(longest_allowed_email_fits_the_firmwares_buffers);
    RUN(oversized_email_is_refused_not_overrun);
    RUN(signing_input_structure);
}
