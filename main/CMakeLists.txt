idf_component_register(
    SRCS "app_main.c" "camera.c" "http_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp32-camera esp_event esp_http_server heap esp_netif esp_wifi nvs_flash spi_flash
)
