idf_component_register(
    SRCS "prov_store.c" "prov_web.c" "wifi_sta.c" "config_web.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server json app_common mbedtls board_bsp
)
