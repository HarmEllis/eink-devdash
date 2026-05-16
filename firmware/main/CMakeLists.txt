idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "wifi_prov.c"
        "api_client.c"
        "storage.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_client
        nvs_flash
        wifi_provisioning
        json
        eink_weact29
)
