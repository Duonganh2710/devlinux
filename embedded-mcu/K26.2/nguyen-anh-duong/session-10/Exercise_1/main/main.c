// Exercise 1 - LED dimming with LEDC
// knob on GPIO1 sets the brightness, button on GPIO16 switches to the
// hardware fade (breathe) mode. LED on GPIO15.

#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED_PIN      GPIO_NUM_15
#define BTN_PIN      GPIO_NUM_16

// the S3 only has low speed mode
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_CH      LEDC_CHANNEL_0

// 5 kHz at 13 bit -> the timer needs 5000*8192 = 41 MHz of the 80 MHz APB
// clock, that fits. freq and resolution are tied: max freq = clock / 2^res,
// so 13 bit at 5 MHz is impossible and ledc_timer_config returns an error.
#define LEDC_RES     LEDC_TIMER_13_BIT
#define LEDC_FREQ    5000
#define DUTY_MAX     ((1U << 13) - 1U)

#define FADE_MS      2000

// ADC is 12 bit, raw is 0..4095 which is not the same as DUTY_MAX
#define ADC_MAX      4095
#define NSAMPLES     16

static const char *TAG = "PWM";

static adc_oneshot_unit_handle_t adc;

// ISR sets this, app_main clears it
static volatile bool btn_pressed;
static volatile int64_t last_press;

static void IRAM_ATTR btn_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - last_press > 50000) {      // ignore anything within 50 ms (bounce)
        last_press = now;
        btn_pressed = true;
    }
}

static void init_ledc(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num = LED_PIN,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CH,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));

    ESP_ERROR_CHECK(ledc_fade_func_install(0));
}

static void init_adc(void)
{
    adc_oneshot_unit_init_cfg_t u = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adc));

    adc_oneshot_chan_cfg_t ch = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &ch));
}

static void init_button(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,   // pressed = pin goes low
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_PIN, btn_isr, NULL);
}

// ledc_set_duty only stages the value, update actually pushes it out
static void set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH);
}

static uint32_t read_pot(void)
{
    int raw = 0;
    uint32_t sum = 0;
    for (int i = 0; i < NSAMPLES; i++) {
        adc_oneshot_read(adc, ADC_CHANNEL_0, &raw);
        sum += raw;
    }
    return sum / NSAMPLES;
}

void app_main(void)
{
    init_ledc();
    init_adc();
    init_button();

    bool breathe = false;
    bool up = true;
    ESP_LOGI(TAG, "mode=KNOB");

    while (1) {
        if (btn_pressed) {
            btn_pressed = false;
            breathe = !breathe;

            if (breathe) {
                up = true;
            } else {
                ESP_LOGI(TAG, "mode=KNOB");
            }
        }

        if (breathe) {
            // hand the whole ramp to the LEDC peripheral. WAIT_DONE blocks this
            // task (it sleeps, CPU idle) until the fade finishes, so we never
            // compute intermediate duties ourselves. A press during the fade is
            // kept in btn_pressed and picked up on the next loop.
            uint32_t target = up ? DUTY_MAX : 0;
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CH, target, FADE_MS);
            ESP_LOGI(TAG, "mode=BREATHE  fade %s -> %u over %d ms",
                     up ? "up  " : "down", (unsigned)target, FADE_MS);
            ledc_fade_start(LEDC_MODE, LEDC_CH, LEDC_FADE_WAIT_DONE);
            up = !up;
        } else {
            uint32_t raw = read_pot();
            uint32_t duty = (uint32_t)((uint64_t)raw * DUTY_MAX / ADC_MAX);
            set_duty(duty);
            ESP_LOGI(TAG, "mode=KNOB  raw=%u  duty=%u", (unsigned)raw, (unsigned)duty);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
}
