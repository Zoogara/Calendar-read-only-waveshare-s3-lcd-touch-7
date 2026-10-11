idf_component_register(
    SRCS "remote_log.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf lwip heap log
)
