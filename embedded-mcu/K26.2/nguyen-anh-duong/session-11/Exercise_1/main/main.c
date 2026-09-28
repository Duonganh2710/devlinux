/* Session 11 - Exercise 1
   Light sleep + deep sleep on the ESP32-S3, both with two wake sources
   (timer and button). Compare an RTC variable against a normal one. */

#include <stdbool.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"

#define TAG           "SLEEP"
#define BTN_PIN       GPIO_NUM_16     // RTC-capable, other leg to GND
#define PIN_BK_LIGHT  GPIO_NUM_2      // display backlight, high = on

#define LIGHT_SLEEP_US  5000000ULL    // 5 s
#define DEEP_SLEEP_US   10000000ULL   // 10 s

#define BTN_WAKE_LEVEL  0             // pull-up, so a press pulls it low

// this one lives in RTC slow memory, stays powered through deep sleep
RTC_DATA_ATTR static uint32_t rtc_boot_count = 0;

// normal RAM, deep sleep cuts it and the chip resets from scratch
static uint32_t ram_boot_count = 0;

static const char *cause_name(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return "TIMER";
    case ESP_SLEEP_WAKEUP_EXT0:
        return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
        return "EXT1";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "GPIO";
    default:
        return "POWER_ON / RESET";
    }
}

static void backlight(bool on)
{
    gpio_set_level(PIN_BK_LIGHT, on ? 1 : 0);
}

static void btn_init(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BTN_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

void app_main(void)
{
    // bump both counters and report what woke us up this time
    rtc_boot_count++;
    ram_boot_count++;

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "=== boot: cause=%s  rtc_boots=%" PRIu32 "  ram_boots=%" PRIu32 " ===",
             cause_name(cause), rtc_boot_count, ram_boot_count);

    btn_init();
    gpio_set_direction(PIN_BK_LIGHT, GPIO_MODE_OUTPUT);
    backlight(true);

    // ---- light sleep ----
    backlight(false);   // no point leaving the backlight on while sleeping
    esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_US);

    // light sleep: enable the pin for wake-up, then enable the GPIO wake source
    // miss either one and it only ever wakes on the timer, not the button
    gpio_wakeup_enable(BTN_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    ESP_LOGI(TAG, "entering light sleep (5 s or button)");
    esp_light_sleep_start();

    // the call above returns, so code carries on from here instead of resetting
    backlight(true);
    ESP_LOGI(TAG, "resumed from light sleep, cause=%s",
             cause_name(esp_sleep_get_wakeup_cause()));

    // ---- deep sleep ----
    backlight(false);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_US);

    // no GPIO peripheral left in deep sleep, only the RTC_IO block, so this
    // has to be ext0 - the light sleep way above does nothing here
    esp_sleep_enable_ext0_wakeup(BTN_PIN, BTN_WAKE_LEVEL);

    ESP_LOGI(TAG, "entering deep sleep - see you in app_main()");
    esp_deep_sleep_start();

    // never reached: deep sleep resets the chip, so app_main() runs again
    // from the top
}




