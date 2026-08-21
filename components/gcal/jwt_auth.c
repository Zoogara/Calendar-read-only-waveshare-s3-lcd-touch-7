#include "jwt_auth.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "jwt_auth";

#define TOKEN_URL "https://oauth2.googleapis.com/token"
#define CALENDAR_SCOPE "https://www.googleapis.com/auth/calendar.readonly"

/* base64url({"alg":"RS256","typ":"JWT"}) - constant for every request. */
static const char *JWT_HEADER_B64 = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9";

static char s_cached_token[1024];
static time_t s_cached_expiry;
static SemaphoreHandle_t s_lock;

/* esp_http_client_perform() can return ESP_ERR_HTTP_EAGAIN even for a
 * blocking (non-async) client if a header/data read times out mid-transfer
 * - ESP-IDF's own esp_http_client.c comments say the caller should just
 * call perform() again on the same handle to pick up where it left off.
 * Seen in practice on a Wi-Fi link that occasionally drops/reconnects. */
static esp_err_t http_perform_with_retry(esp_http_client_handle_t client)
{
    esp_err_t err;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return err;
}

static esp_err_t b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_sz)
{
    size_t olen = 0;
    /* mbedtls_base64_encode returns MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL and
     * sets olen to the required size (which can exceed out_sz) rather than
     * writing anything - must check the return value before touching
     * out[olen], or an undersized out buffer becomes a stack overflow. */
    int ret = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, in, in_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "base64 encode failed (needed %d bytes, have %d): mbedtls -0x%04x",
                 (int)olen, (int)out_sz, (unsigned int)-ret);
        return ESP_ERR_INVALID_SIZE;
    }
    out[olen] = '\0';
    for (size_t i = 0; i < olen; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    /* strip padding */
    while (olen > 0 && out[olen - 1] == '=') {
        out[--olen] = '\0';
    }
    return ESP_OK;
}

/* Response buffer for the HTTP client's event-based fetch. */
struct http_resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    struct http_resp_buf *buf = (struct http_resp_buf *)evt->user_data;
    /* evt->data is already de-chunked payload during ON_DATA regardless of
     * transfer encoding when using esp_http_client_perform() - excluding
     * chunked responses here (as some ESP-IDF examples do for a different,
     * manual-esp_http_client_read() flow that doesn't apply when using
     * perform()) meant Google's chunked-encoded token responses were
     * silently dropped: HTTP 200 came back but the body buffer stayed
     * empty, which looked like an auth failure with no real diagnostic. */
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf->len + evt->data_len + 1 > buf->cap) {
            size_t new_cap = (buf->len + evt->data_len + 1) * 2;
            char *grown = realloc(buf->data, new_cap);
            if (grown == NULL) {
                return ESP_FAIL;
            }
            buf->data = grown;
            buf->cap = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, evt->data_len);
        buf->len += evt->data_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t build_signed_jwt(const app_settings_t *cfg, char *jwt_out, size_t jwt_out_sz)
{
    time_t now;
    time(&now);

    char claims_json[384];
    int claims_n = snprintf(claims_json, sizeof(claims_json),
             "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
             cfg->sa_client_email, CALENDAR_SCOPE, TOKEN_URL,
             (long long)now, (long long)(now + 3600));
    if (claims_n < 0 || (size_t)claims_n >= sizeof(claims_json)) {
        ESP_LOGE(TAG, "claims_json buffer too small (needed %d, have %d) - "
                      "is the service account email unusually long?",
                 claims_n, (int)sizeof(claims_json));
        return ESP_ERR_INVALID_SIZE;
    }

    char claims_b64[512];
    if (b64url_encode((const uint8_t *)claims_json, strlen(claims_json), claims_b64, sizeof(claims_b64)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    char signing_input[600];
    int n = snprintf(signing_input, sizeof(signing_input), "%s.%s", JWT_HEADER_B64, claims_b64);
    if (n < 0 || (size_t)n >= sizeof(signing_input)) {
        ESP_LOGE(TAG, "signing_input buffer too small (needed %d, have %d) - "
                      "is the service account email unusually long?",
                 n, (int)sizeof(signing_input));
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t *)signing_input, strlen(signing_input), digest);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    esp_err_t ret = ESP_FAIL;
    const char *pers = "gcal_jwt";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed");
        goto out;
    }

    /* +1 to include the PEM's NUL terminator, which mbedtls_pk_parse_key
     * requires for PEM (vs. DER) input. */
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)cfg->sa_private_key_pem,
                              strlen(cfg->sa_private_key_pem) + 1, NULL, 0,
                              mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "failed to parse service-account private key - check it was "
                      "pasted with the BEGIN/END PRIVATE KEY lines intact");
        goto out;
    }

    uint8_t sig[MBEDTLS_MPI_MAX_SIZE];
    size_t sig_len = 0;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                         sig, sizeof(sig), &sig_len,
                         mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        ESP_LOGE(TAG, "RSA sign failed");
        goto out;
    }

    char sig_b64[700]; /* fits signatures up to a 4096-bit RSA key
                           (512-byte sig -> ~683 base64 chars); Google's
                           default 2048-bit keys only need ~344 */
    if (b64url_encode(sig, sig_len, sig_b64, sizeof(sig_b64)) != ESP_OK) {
        goto out;
    }

    n = snprintf(jwt_out, jwt_out_sz, "%s.%s", signing_input, sig_b64);
    if (n < 0 || (size_t)n >= jwt_out_sz) {
        ESP_LOGE(TAG, "final JWT buffer too small (needed %d, have %d)",
                 n, (int)jwt_out_sz);
        goto out;
    }
    ret = ESP_OK;

out:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

static esp_err_t exchange_jwt_for_token(const char *jwt)
{
    /* "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion="
     * with the grant_type value percent-encoded; the JWT itself is
     * unpadded base64url, which is already URL-safe. */
    static const char *grant_type_encoded =
        "urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer";

    size_t body_sz = strlen(grant_type_encoded) + strlen(jwt) + 64;
    char *body = malloc(body_sz);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_sz, "grant_type=%s&assertion=%s", grant_type_encoded, jwt);

    struct http_resp_buf resp = {.data = malloc(512), .len = 0, .cap = 512};
    if (resp.data == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url = TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        /* Default is only 512 bytes (DEFAULT_HTTP_BUF_SIZE) - worked so far
         * but fragile; see the matching buffer_size comment in
         * gcal_client.c for what happens when response headers don't fit
         * (silent hang until timeout, not a clean error). */
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = http_perform_with_retry(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "token request transport error: %s", esp_err_to_name(err));
        free(resp.data);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "token request HTTP %d: %s", status, resp.data);
        free(resp.data);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.data);
    if (root == NULL) {
        ESP_LOGE(TAG, "token response HTTP 200 but not valid JSON: %.200s", resp.data);
        free(resp.data);
        return ESP_FAIL;
    }
    free(resp.data);
    cJSON *tok = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    if (!cJSON_IsString(tok) || tok->valuestring == NULL) {
        char *dump = cJSON_PrintUnformatted(root);
        ESP_LOGE(TAG, "token response HTTP 200 but no access_token field: %.200s",
                 dump ? dump : "(couldn't re-serialize)");
        free(dump);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strncpy(s_cached_token, tok->valuestring, sizeof(s_cached_token) - 1);
    time_t now;
    time(&now);
    long expires_in = cJSON_IsNumber(exp) ? (long)exp->valuedouble : 3600;
    s_cached_expiry = now + expires_in - 60; /* 60s safety margin */
    cJSON_Delete(root);

    ESP_LOGI(TAG, "obtained access token, valid ~%lds", expires_in);
    return ESP_OK;
}

esp_err_t jwt_auth_get_token(const app_settings_t *cfg, char *out_token, size_t out_sz)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    time_t now;
    time(&now);
    esp_err_t err = ESP_OK;
    if (s_cached_token[0] == '\0' || now >= s_cached_expiry) {
        char jwt[1400]; /* header + claims + signature; see sig_b64 comment
                            in build_signed_jwt for the 4096-bit-key sizing */
        err = build_signed_jwt(cfg, jwt, sizeof(jwt));
        if (err == ESP_OK) {
            err = exchange_jwt_for_token(jwt);
        }
    }
    if (err == ESP_OK) {
        strncpy(out_token, s_cached_token, out_sz - 1);
        out_token[out_sz - 1] = '\0';
    }

    xSemaphoreGive(s_lock);
    return err;
}
