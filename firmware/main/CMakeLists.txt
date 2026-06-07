idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "wifi_prov.c"
        "wifi_roam.c"
        "captive_dns.c"
        "api_client.c"
        "storage.c"
        "timekeep.c"
        "boot_button.c"
        "ota_client.c"
        "ota_version.c"
        "runtime_policy.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_app_format
        esp_wifi
        esp_http_client
        esp_http_server
        esp_https_ota
        app_update
        esp-tls
        esp_timer
        nvs_flash
        driver
        vfs
        json
        eink_weact29
        espressif__qrcode
)

# Catch large automatic stack frames in this component before they hit
# hardware. The SoftAP provisioning crash on ESP32-S3 was a 7+ KB
# dash_config_v2_t local in the HTTP request handler — see
# .co-develop/SOFTAP_DHCP_CONTEXT_SWITCH_CRASH_BRIEF.md. 2048 bytes is a
# generous warn threshold; tighten if it ever fires.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wframe-larger-than=2048)
