idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_common provisioning gcal calendar_ui board_bsp sd_card presence_sensor nvs_flash esp_netif lwip
)
