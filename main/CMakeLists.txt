idf_component_register(
    SRCS "esp-ble-gpio-control.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        esp_driver_gpio
        mdns
)
