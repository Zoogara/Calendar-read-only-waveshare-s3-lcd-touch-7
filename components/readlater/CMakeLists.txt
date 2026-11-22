idf_component_register(
    SRCS "link_url.c" "readlater_store.c" "readlater.c"
    INCLUDE_DIRS "include"
    REQUIRES app_common nvs_flash esp_timer heap
)
