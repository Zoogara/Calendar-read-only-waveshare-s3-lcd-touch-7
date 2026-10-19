#include "fake_http.h"

#include <stdlib.h>
#include <string.h>

/* The same shape as ics_client.c's struct http_resp_buf, which the firmware's shared
 * event handler fills in - perform() below does that job here. */
struct resp_buf {
    char *data;
    size_t len, cap;
};

static const char *s_body = "";
static int s_status = 200;
static esp_err_t s_result = 0;
static void *s_user;
static int s_dummy;

void fake_http_serve(const char *body, int status, esp_err_t result)
{
    s_body = body;
    s_status = status;
    s_result = result;
}

esp_http_client_handle_t fake_http_client(void) { return (esp_http_client_handle_t)&s_dummy; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) { (void)c; (void)url; return 0; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) { (void)c; (void)m; return 0; }
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *d) { (void)c; s_user = d; return 0; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return s_status; }

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    (void)c;
    if (s_result != 0) {
        return s_result;
    }
    struct resp_buf *r = s_user;
    size_t n = strlen(s_body);
    if (n + 1 > r->cap) {
        r->data = realloc(r->data, n + 1);
        r->cap = n + 1;
    }
    memcpy(r->data, s_body, n + 1);
    r->len = n;
    return 0;
}
