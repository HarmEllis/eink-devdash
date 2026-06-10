#include "boot_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BOOT_POLL_INTERVAL_MS   50
#define BOOT_FORCE_PROV_MAGIC   0xB001F0F0u
#define BOOT_FACTORY_RESET_MAGIC 0xFAC70123u
/* Ignore falling edges within this window of the last accepted one: longer than
 * tactile contact bounce (~5-20 ms), shorter than a human double-tap interval
 * (~150-300 ms), so a real 1x/2x tap is distinguished from bounce. */
#define BOOT_TAP_DEBOUNCE_US    60000

static const char *TAG = "boot_button";

static RTC_NOINIT_ATTR uint32_t s_force_prov_magic;
static RTC_NOINIT_ATTR uint32_t s_factory_reset_magic;
static bool s_initialised   = false;
static bool s_monitor_alive = false;

static portMUX_TYPE s_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int     s_tap_count;
static volatile int64_t s_tap_last_edge_us;
static bool s_tap_isr_service_installed = false;

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

static void IRAM_ATTR boot_button_tap_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();   /* ISR/IRAM-safe */
    portENTER_CRITICAL_ISR(&s_tap_mux);
    if (now - s_tap_last_edge_us >= BOOT_TAP_DEBOUNCE_US) {
        s_tap_last_edge_us = now;
        s_tap_count++;
    }
    portEXIT_CRITICAL_ISR(&s_tap_mux);
}

void boot_button_press_counter_arm(void)
{
    /* The ISR needs the pin configured (input + pull-up, active-low); don't rely
     * on an earlier level-read having done it. */
    if (!s_initialised) boot_button_init();

    portENTER_CRITICAL(&s_tap_mux);
    s_tap_count = 0;
    s_tap_last_edge_us = 0;
    portEXIT_CRITICAL(&s_tap_mux);

    if (!s_tap_isr_service_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "tap ISR service install failed: %s — taps will be "
                     "missed, reset gesture degrades to cancel", esp_err_to_name(err));
            return;
        }
        s_tap_isr_service_installed = true;
    }

    /* Set the edge type before registering the handler so it is never briefly
     * live with the GPIO_INTR_DISABLE type left by boot_button_init(). Failures
     * are logged, not fatal: ESP_ERROR_CHECK would abort() the whole device for a
     * non-critical UI gesture, and take() returning 0 already degrades safely. */
    esp_err_t err = gpio_set_intr_type(BOOT_BUTTON_GPIO, GPIO_INTR_NEGEDGE);
    if (err == ESP_OK) err = gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_button_tap_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tap ISR arm failed: %s — reset gesture degrades to cancel",
                 esp_err_to_name(err));
        gpio_set_intr_type(BOOT_BUTTON_GPIO, GPIO_INTR_DISABLE);
    }
}

int boot_button_press_counter_take(void)
{
    int c;
    portENTER_CRITICAL(&s_tap_mux);
    c = s_tap_count;
    s_tap_count = 0;
    portEXIT_CRITICAL(&s_tap_mux);
    return c;
}

void boot_button_press_counter_disarm(void)
{
    gpio_isr_handler_remove(BOOT_BUTTON_GPIO);
    gpio_set_intr_type(BOOT_BUTTON_GPIO, GPIO_INTR_DISABLE);
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
