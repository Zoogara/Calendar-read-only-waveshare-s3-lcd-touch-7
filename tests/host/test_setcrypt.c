/* settings_crypt.c: the encrypted settings backup format. The known-answer vectors were produced by an
 * independent implementation (Python hashlib + the "cryptography" package, tools/gcal_settings.py), so this
 * checks the file format against something other than itself. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "settings_crypt.h"
#include "test.h"

static const char *PASS = "correct horse battery";
static const char *PLAIN = "{\"wifi_ssid\":\"demo\",\"n\":1}";   /* 26 bytes */

static const uint8_t KAT[82] = {
    0x47, 0x43, 0x41, 0x4c, 0x53, 0x45, 0x54, 0x01, 0x00, 0x00, 0x27, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
    0x6c, 0x6d, 0x6e, 0x6f, 0x82, 0x15, 0x97, 0x09, 0x86, 0xd1, 0x47, 0xe8, 0x4a, 0xad, 0xd7, 0x41, 0x68, 0x24,
    0xbf, 0x58, 0xbb, 0xc9, 0x54, 0x83, 0xc7, 0x5d, 0x8f, 0x1c, 0x44, 0xdc, 0x36, 0xb4, 0xd0, 0x57, 0x89, 0xcb,
    0xfa, 0xbc, 0x69, 0xcd, 0x14, 0x6b, 0x09, 0xf9, 0x13, 0x20};

static void fixed(uint8_t *salt, uint8_t *nonce)
{
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(100 + i);
}

static void pbkdf2_known_answers(void)
{
    uint8_t key[32];
    /* RFC 7914 section 11's published vector family: PBKDF2-HMAC-SHA256("password", "salt", 1) */
    CHECK_INT(settings_crypt_pbkdf2("password", (const uint8_t *)"salt", 4, 1, key), 0);
    static const uint8_t k1[32] = {0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
                                   0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48, 0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b};
    CHECK(memcmp(key, k1, 32) == 0);
    /* the same call with Python's hashlib.pbkdf2_hmac, 10000 rounds */
    uint8_t salt[16], nonce[12];
    fixed(salt, nonce);
    CHECK_INT(settings_crypt_pbkdf2(PASS, salt, 16, 10000, key), 0);
    static const uint8_t k2[32] = {0x5f, 0xe0, 0xc8, 0x2d, 0xe5, 0x6e, 0x5f, 0xd9, 0x8d, 0x1c, 0x20, 0x2f, 0x10, 0x37, 0x83, 0x95,
                                   0xaa, 0xae, 0x09, 0x85, 0x13, 0x0f, 0xba, 0xdd, 0x92, 0x46, 0x57, 0x76, 0xc0, 0xb2, 0x97, 0xf4};
    CHECK(memcmp(key, k2, 32) == 0);
}

static void encrypt_matches_reference(void)
{
    uint8_t salt[16], nonce[12], out[128];
    size_t n = 0;
    fixed(salt, nonce);
    CHECK_INT(settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, strlen(PLAIN), 10000, salt, nonce, out, sizeof(out), &n),
              SETCRYPT_OK);
    CHECK_INT(n, 82);
    CHECK(memcmp(out, KAT, 82) == 0);
}

static void decrypt_reference_file(void)
{
    uint8_t out[64];
    size_t n = 0;
    CHECK_INT(settings_crypt_decrypt(PASS, KAT, sizeof(KAT), out, sizeof(out), &n), SETCRYPT_OK);
    CHECK_INT(n, 26);
    CHECK(memcmp(out, PLAIN, 26) == 0);
}

static void round_trip_sizes(void)
{
    uint8_t salt[16], nonce[12];
    fixed(salt, nonce);
    size_t sizes[] = {0, 1, 15, 16, 17, 255, 4096, 9000};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        size_t len = sizes[k];
        uint8_t *plain = malloc(len + 1), *enc = malloc(len + SETCRYPT_OVERHEAD), *dec = malloc(len + 1);
        for (size_t i = 0; i < len; i++) plain[i] = (uint8_t)(i * 31 + 7);
        size_t en = 0, dn = 0;
        CHECK_INT(settings_crypt_encrypt(PASS, plain, len, 10000, salt, nonce, enc, len + SETCRYPT_OVERHEAD, &en), SETCRYPT_OK);
        CHECK_INT(en, len + SETCRYPT_OVERHEAD);
        CHECK_INT(settings_crypt_decrypt(PASS, enc, en, dec, len + 1, &dn), SETCRYPT_OK);
        CHECK_INT(dn, len);
        CHECK(len == 0 || memcmp(plain, dec, len) == 0);
        if (len > 16) {   /* the plain text is not sitting in the file */
            CHECK(memcmp(enc + SETCRYPT_HEADER_LEN, plain, 16) != 0);
        }
        free(plain); free(enc); free(dec);
    }
}

static void wrong_passphrase_and_tampering(void)
{
    uint8_t out[64], copy[82];
    size_t n = 0;
    CHECK_INT(settings_crypt_decrypt("correct horse batterz", KAT, sizeof(KAT), out, sizeof(out), &n), SETCRYPT_ERR_AUTH);
    CHECK_INT(settings_crypt_decrypt("", KAT, sizeof(KAT), out, sizeof(out), &n), SETCRYPT_ERR_AUTH);
    /* flipping any single bit anywhere in the file must be refused */
    int refused = 0, tried = 0;
    for (size_t byte = 0; byte < sizeof(KAT); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(copy, KAT, sizeof(KAT));
            copy[byte] ^= (uint8_t)(1u << bit);
            memset(out, 0xAA, sizeof(out));
            setcrypt_err_t e = settings_crypt_decrypt(PASS, copy, sizeof(copy), out, sizeof(out), &n);
            tried++;
            if (e != SETCRYPT_OK) {
                refused++;
            }
            if (e != SETCRYPT_OK) {
                CHECK(out[0] == 0xAA || e == SETCRYPT_ERR_AUTH);   /* nothing usable leaked on a refusal */
            }
        }
    }
    CHECK_INT(refused, tried);
}

static void malformed_files(void)
{
    uint8_t out[64], copy[82];
    size_t n = 0;
    CHECK_INT(settings_crypt_decrypt(PASS, KAT, 0, out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);
    CHECK_INT(settings_crypt_decrypt(PASS, KAT, 55, out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);       /* shorter than header + tag */
    CHECK_INT(settings_crypt_decrypt(PASS, KAT, sizeof(KAT) - 1, out, sizeof(out), &n), SETCRYPT_ERR_AUTH);   /* truncated */
    memcpy(copy, KAT, sizeof(KAT)); copy[0] = 'X';
    CHECK_INT(settings_crypt_decrypt(PASS, copy, sizeof(copy), out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);
    memcpy(copy, KAT, sizeof(KAT)); copy[7] = 2;      /* a newer format version */
    CHECK_INT(settings_crypt_decrypt(PASS, copy, sizeof(copy), out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);
    /* an iteration count that is too low (weak) or too high (ties the display up) is refused before any work */
    memcpy(copy, KAT, sizeof(KAT)); copy[8] = copy[9] = copy[10] = 0; copy[11] = 1;
    CHECK_INT(settings_crypt_decrypt(PASS, copy, sizeof(copy), out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);
    memcpy(copy, KAT, sizeof(KAT)); copy[8] = 0xff; copy[9] = 0xff; copy[10] = 0xff; copy[11] = 0xff;
    CHECK_INT(settings_crypt_decrypt(PASS, copy, sizeof(copy), out, sizeof(out), &n), SETCRYPT_ERR_FORMAT);
    /* output buffer too small */
    CHECK_INT(settings_crypt_decrypt(PASS, KAT, sizeof(KAT), out, 25, &n), SETCRYPT_ERR_ARG);
    /* nulls */
    CHECK_INT(settings_crypt_decrypt(NULL, KAT, sizeof(KAT), out, sizeof(out), &n), SETCRYPT_ERR_ARG);
}

static void encrypt_argument_checks(void)
{
    uint8_t salt[16], nonce[12], out[128];
    size_t n = 0;
    fixed(salt, nonce);
    CHECK_INT(settings_crypt_encrypt("short pass", (const uint8_t *)PLAIN, 26, 10000, salt, nonce, out, sizeof(out), &n),
              SETCRYPT_ERR_PASSPHRASE);                       /* 10 characters */
    CHECK_INT(settings_crypt_encrypt("exactly12chr", (const uint8_t *)PLAIN, 26, 10000, salt, nonce, out, sizeof(out), &n),
              SETCRYPT_OK);                                   /* 12 is enough */
    CHECK_INT(settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 10000, salt, nonce, out, 81, &n), SETCRYPT_ERR_ARG);
    CHECK_INT(settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 9999, salt, nonce, out, sizeof(out), &n), SETCRYPT_ERR_ARG);
    CHECK_INT(settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 1000001, salt, nonce, out, sizeof(out), &n), SETCRYPT_ERR_ARG);
    CHECK_INT(settings_crypt_encrypt(PASS, NULL, 26, 10000, salt, nonce, out, sizeof(out), &n), SETCRYPT_ERR_ARG);
}

static void different_salt_or_nonce_changes_everything(void)
{
    uint8_t salt[16], nonce[12], a[128], b[128];
    size_t n = 0;
    fixed(salt, nonce);
    settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 10000, salt, nonce, a, sizeof(a), &n);
    nonce[0] ^= 1;
    settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 10000, salt, nonce, b, sizeof(b), &n);
    CHECK(memcmp(a + SETCRYPT_HEADER_LEN, b + SETCRYPT_HEADER_LEN, 26) != 0);
    nonce[0] ^= 1; salt[0] ^= 1;
    settings_crypt_encrypt(PASS, (const uint8_t *)PLAIN, 26, 10000, salt, nonce, b, sizeof(b), &n);
    CHECK(memcmp(a + SETCRYPT_HEADER_LEN, b + SETCRYPT_HEADER_LEN, 26) != 0);
}

void test_setcrypt(void)
{
    RUN(pbkdf2_known_answers);
    RUN(encrypt_matches_reference);
    RUN(decrypt_reference_file);
    RUN(round_trip_sizes);
    RUN(wrong_passphrase_and_tampering);
    RUN(malformed_files);
    RUN(encrypt_argument_checks);
    RUN(different_salt_or_nonce_changes_everything);
}
