idf_component_register(SRCS "main.c"
    REQUIRES espressif__esp_hosted
    PRIV_REQUIRES nvs_flash esp_wifi esp_event)
