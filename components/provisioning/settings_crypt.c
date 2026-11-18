#include "settings_crypt.h"
#include <string.h>
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

static void (*s_yield)(void);

void settings_crypt_set_yield(void (*fn)(void))
{
    s_yield = fn;
}

int settings_crypt_pbkdf2(const char *passphrase, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t key[32])
{
    /* One block (32 bytes = SHA-256's output), so PBKDF2 is U1 = HMAC(pw, salt || 00000001),
     * Ui = HMAC(pw, Ui-1), result = U1 xor U2 xor ... xor Un. The HMAC is set up once and reset
     * each round, which is half the work of calling a one-shot HMAC every time. */
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL || passphrase == NULL || salt == NULL || iterations == 0 || key == NULL) {
        return -1;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    uint8_t u[32], acc[32];
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) {
        rc = mbedtls_md_hmac_starts(&ctx, (const unsigned char *)passphrase, strlen(passphrase));
    }
    if (rc == 0) {
        static const uint8_t one[4] = {0, 0, 0, 1};
        rc = mbedtls_md_hmac_update(&ctx, salt, salt_len);
        if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, one, 4);
        if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, u);
    }
    if (rc == 0) {
        memcpy(acc, u, 32);
        for (uint32_t i = 1; i < iterations && rc == 0; i++) {
            rc = mbedtls_md_hmac_reset(&ctx);
            if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, u, 32);
            if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, u);
            for (int j = 0; j < 32; j++) {
                acc[j] ^= u[j];
            }
            if (s_yield != NULL && i % 10000 == 0) {
                s_yield();
            }
        }
    }
    if (rc == 0) {
        memcpy(key, acc, 32);
    }
    mbedtls_md_free(&ctx);
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(acc, sizeof(acc));
    return rc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

setcrypt_err_t settings_crypt_encrypt(const char *passphrase, const uint8_t *plain, size_t plain_len,
                                      uint32_t iterations, const uint8_t salt[SETCRYPT_SALT_LEN],
                                      const uint8_t nonce[SETCRYPT_NONCE_LEN],
                                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (passphrase == NULL || plain == NULL || salt == NULL || nonce == NULL || out == NULL ||
        out_len == NULL || plain_len > SIZE_MAX - SETCRYPT_OVERHEAD ||
        out_cap < plain_len + SETCRYPT_OVERHEAD ||
        iterations < SETCRYPT_MIN_ITERATIONS || iterations > SETCRYPT_MAX_ITERATIONS) {
        return SETCRYPT_ERR_ARG;
    }
    if (strlen(passphrase) < SETCRYPT_MIN_PASSPHRASE) {
        return SETCRYPT_ERR_PASSPHRASE;
    }
    memcpy(out, SETCRYPT_MAGIC, 7);
    out[7] = SETCRYPT_VERSION;
    put_be32(out + 8, iterations);
    memcpy(out + 12, salt, SETCRYPT_SALT_LEN);
    memcpy(out + 28, nonce, SETCRYPT_NONCE_LEN);

    uint8_t key[32];
    if (settings_crypt_pbkdf2(passphrase, salt, SETCRYPT_SALT_LEN, iterations, key) != 0) {
        return SETCRYPT_ERR_CRYPTO;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain_len, nonce, SETCRYPT_NONCE_LEN,
                                       out, SETCRYPT_HEADER_LEN, plain, out + SETCRYPT_HEADER_LEN,
                                       SETCRYPT_TAG_LEN, out + SETCRYPT_HEADER_LEN + plain_len);
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (rc != 0) {
        return SETCRYPT_ERR_CRYPTO;
    }
    *out_len = plain_len + SETCRYPT_OVERHEAD;
    return SETCRYPT_OK;
}

setcrypt_err_t settings_crypt_decrypt(const char *passphrase, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (passphrase == NULL || in == NULL || out == NULL || out_len == NULL) {
        return SETCRYPT_ERR_ARG;
    }
    if (in_len < SETCRYPT_OVERHEAD || memcmp(in, SETCRYPT_MAGIC, 7) != 0 || in[7] != SETCRYPT_VERSION) {
        return SETCRYPT_ERR_FORMAT;
    }
    uint32_t iterations = get_be32(in + 8);
    if (iterations < SETCRYPT_MIN_ITERATIONS || iterations > SETCRYPT_MAX_ITERATIONS) {
        return SETCRYPT_ERR_FORMAT;
    }
    size_t plain_len = in_len - SETCRYPT_OVERHEAD;
    if (out_cap < plain_len) {
        return SETCRYPT_ERR_ARG;
    }
    uint8_t key[32];
    if (settings_crypt_pbkdf2(passphrase, in + 12, SETCRYPT_SALT_LEN, iterations, key) != 0) {
        return SETCRYPT_ERR_CRYPTO;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&gcm, plain_len, in + 28, SETCRYPT_NONCE_LEN, in, SETCRYPT_HEADER_LEN,
                                      in + SETCRYPT_HEADER_LEN + plain_len, SETCRYPT_TAG_LEN,
                                      in + SETCRYPT_HEADER_LEN, out);
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        return SETCRYPT_ERR_AUTH;
    }
    if (rc != 0) {
        return SETCRYPT_ERR_CRYPTO;
    }
    *out_len = plain_len;
    return SETCRYPT_OK;
}
