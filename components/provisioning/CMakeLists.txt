idf_component_register(
    SRCS "prov_store.c" "prov_web.c" "wifi_sta.c" "config_web.c" "auth_guard.c" "url_decode.c" "settings_crypt.c"
    INCLUDE_DIRS "include"
    REQUIRES readlater nvs_flash esp_wifi esp_netif lwip esp_timer esp_event esp_http_server json app_common mbedtls board_bsp ota_update app_update esp_app_format
)
