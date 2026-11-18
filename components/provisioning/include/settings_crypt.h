#pragma once
/*
 * Password-encrypted settings backup: the format and the crypto, with no ESP-IDF in it (so the host tests
 * run the real code). config_web.c supplies the random bytes and the settings JSON.
 *
 *   offset  size  what
 *        0     7  magic "GCALSET"
 *        7     1  format version (1)
 *        8     4  PBKDF2 iteration count, big endian
 *       12    16  salt
 *       28    12  AES-GCM nonce
 *       40     n  ciphertext (the settings JSON, same length as the plain text)
 *     40+n    16  GCM tag
 *
 * Key: PBKDF2-HMAC-SHA256(passphrase, salt, iterations) -> 32 bytes -> AES-256-GCM. The first 40 bytes
 * are the associated data, so changing the version, iteration count, salt or nonce fails the tag check
 * just as changing the ciphertext does. Anything that doesn't decrypt and authenticate yields an error and
 * no output: a wrong passphrase and a damaged file are deliberately indistinguishable.
 *
 * The file can be read without the display: tools/gcal_settings.py decrypts and encrypts the same format.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETCRYPT_MAGIC          "GCALSET"
#define SETCRYPT_VERSION        1
#define SETCRYPT_SALT_LEN       16
#define SETCRYPT_NONCE_LEN      12
#define SETCRYPT_TAG_LEN        16
#define SETCRYPT_HEADER_LEN     40
#define SETCRYPT_OVERHEAD       (SETCRYPT_HEADER_LEN + SETCRYPT_TAG_LEN)

#define SETCRYPT_ITERATIONS     100000u      /* what a backup is written with */
#define SETCRYPT_MIN_ITERATIONS 10000u       /* a file claiming fewer or more is refused: */
#define SETCRYPT_MAX_ITERATIONS 1000000u     /* too weak, or a way to tie the display up */
#define SETCRYPT_MIN_PASSPHRASE 12           /* characters */

typedef enum {
    SETCRYPT_OK = 0,
    SETCRYPT_ERR_ARG,             /* a null pointer or too small an output buffer */
    SETCRYPT_ERR_PASSPHRASE,      /* encrypting: passphrase shorter than SETCRYPT_MIN_PASSPHRASE */
    SETCRYPT_ERR_FORMAT,          /* not one of our files: magic, version, length or iteration count */
    SETCRYPT_ERR_AUTH,            /* wrong passphrase, or the file was changed */
    SETCRYPT_ERR_CRYPTO,          /* the crypto library failed (out of memory) */
} setcrypt_err_t;

/* Called every 10000 PBKDF2 rounds (a backup takes a second or two of solid CPU): the firmware gives it a
 * task delay so the idle task and its watchdog get to run. NULL (the default) does nothing. */
void settings_crypt_set_yield(void (*fn)(void));

/* PBKDF2-HMAC-SHA256, 32 bytes out. Exposed so the tests can check it against known answers. */
int settings_crypt_pbkdf2(const char *passphrase, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t key[32]);

/* out must hold plain_len + SETCRYPT_OVERHEAD bytes. salt and nonce must be fresh random bytes
 * every time (the caller's job - the tests pass fixed ones). */
setcrypt_err_t settings_crypt_encrypt(const char *passphrase, const uint8_t *plain, size_t plain_len,
                                      uint32_t iterations, const uint8_t salt[SETCRYPT_SALT_LEN],
                                      const uint8_t nonce[SETCRYPT_NONCE_LEN],
                                      uint8_t *out, size_t out_cap, size_t *out_len);

/* out must hold in_len - SETCRYPT_OVERHEAD bytes (add one for a terminating NUL if you want a string).
 * Nothing is written to out unless the whole file authenticates. */
setcrypt_err_t settings_crypt_decrypt(const char *passphrase, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
