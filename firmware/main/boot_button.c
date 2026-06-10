#include "boot_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BOOT_POLL_INTERVAL_MS   50
#define BOOT_FORCE_PROV_MAGIC   0xB001F0F0u
#define BOOT_FACTORY_RESET_MAGIC 0xFAC70123u

static const char *TAG = "boot_button";

static RTC_NOINIT_ATTR uint32_t s_force_prov_magic;
static RTC_NOINIT_ATTR uint32_t s_factory_reset_magic;
static bool s_initialised   = false;
static bool s_monitor_alive = false;

void boot_button_init(void)
{
    if (s_initialised) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    // First read after configuring the pull-up needs a settle delay.
    vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
    s_initialised = true;
}

bool boot_button_is_pressed(void)
{
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

bool boot_button_wait_longpress(uint32_t hold_ms)
{
    uint32_t elapsed = 0;
    while (boot_button_is_pressed()) {
        if (elapsed >= hold_ms) return true;
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
        elapsed += BOOT_POLL_INTERVAL_MS;
    }
    return false;
}

void boot_button_wait_release(void)
{
    while (boot_button_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
    }
    // Tiny debounce after release before any subsequent reset, to avoid
    // catching contact bounce as a fresh strap-pin assertion.
    vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
}

void boot_button_force_prov_mark(void)
{
    s_force_prov_magic = BOOT_FORCE_PROV_MAGIC;
}

bool boot_button_force_prov_active(void)
{
    return s_force_prov_magic == BOOT_FORCE_PROV_MAGIC;
}

void boot_button_force_prov_clear(void)
{
    s_force_prov_magic = 0;
}

void boot_button_request_factory_reset(void)
{
    s_factory_reset_magic = BOOT_FACTORY_RESET_MAGIC;
}

bool boot_button_factory_reset_pending(void)
{
    return s_factory_reset_magic == BOOT_FACTORY_RESET_MAGIC;
}

void boot_button_factory_reset_clear(void)
{
    s_factory_reset_magic = 0;
}

static void boot_button_monitor_task(void *arg)
{
    (void)arg;
    const uint32_t hold_ms = CONFIG_DEVDASH_BOOT_LONGPRESS_MS;
    ESP_LOGI(TAG, "Monitor running (long-press = %u ms)", (unsigned)hold_ms);

    for (;;) {
        if (!boot_button_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "BOOT pressed; hold %u ms to enter setup", (unsigned)hold_ms);
        if (!boot_button_wait_longpress(hold_ms)) continue;

        ESP_LOGW(TAG, "Long-press confirmed; release BOOT to reboot into setup");
        boot_button_wait_release();

        boot_button_force_prov_mark();
        ESP_LOGW(TAG, "Restarting into provisioning portal");
        esp_restart();
    }
}

void boot_button_monitor_start(void)
{
    if (s_monitor_alive) return;
    if (!s_initialised) boot_button_init();
    BaseType_t ok = xTaskCreate(boot_button_monitor_task, "boot_btn",
                                2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start monitor task");
        return;
    }
    s_monitor_alive = true;
}
