idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    REQUIRES nvs_flash esp_http_server driver esp_wifi esp_driver_rmt
)
