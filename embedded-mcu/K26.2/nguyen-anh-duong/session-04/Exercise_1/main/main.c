#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

// GPIO pins connected to the seven-segment display.
#define SEVEN_SEGMENT_A_GPIO GPIO_NUM_4
#define SEVEN_SEGMENT_B_GPIO GPIO_NUM_5
#define SEVEN_SEGMENT_C_GPIO GPIO_NUM_6
#define SEVEN_SEGMENT_D_GPIO GPIO_NUM_7
#define SEVEN_SEGMENT_E_GPIO GPIO_NUM_8
#define SEVEN_SEGMENT_F_GPIO GPIO_NUM_9
#define SEVEN_SEGMENT_G_GPIO GPIO_NUM_10
#define SEVEN_SEGMENT_DP_GPIO GPIO_NUM_11
#define BUTTON_GPIO GPIO_NUM_2

// Button timing settings.
#define DEBOUNCE_MS   50U
#define DOUBLE_CLICK_MS 350U
#define LONG_PRESS_MS 800U
#define REPEAT_PERIOD_MS 500U

typedef struct
{
    int64_t timestamp_us;
    bool is_press;
} btn_event_t;

static QueueHandle_t btn_queue = NULL;
// Common-cathode map: bits b0 to b6 represent segments a to g.
static const uint8_t segment_map[10] = {
  0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
  0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
};

// Display a digit on the seven-segment display.
static void set_display_seven_segment(int value) {
  if (value < 0 || value > 9) value = 0;
     printf("display_number: %d\n", value);
  // Convert the common-cathode pattern for the common-anode display.
  uint8_t segment_pattern = segment_map[value];
  gpio_set_level(SEVEN_SEGMENT_A_GPIO, !(segment_pattern & (1U << 0)));
  gpio_set_level(SEVEN_SEGMENT_B_GPIO, !(segment_pattern & (1U << 1)));
  gpio_set_level(SEVEN_SEGMENT_C_GPIO, !(segment_pattern & (1U << 2)));
  gpio_set_level(SEVEN_SEGMENT_D_GPIO, !(segment_pattern & (1U << 3)));
  gpio_set_level(SEVEN_SEGMENT_E_GPIO, !(segment_pattern & (1U << 4)));
  gpio_set_level(SEVEN_SEGMENT_F_GPIO, !(segment_pattern & (1U << 5)));
  gpio_set_level(SEVEN_SEGMENT_G_GPIO, !(segment_pattern & (1U << 6)));
  gpio_set_level(SEVEN_SEGMENT_DP_GPIO, 1);
}

// Queue debounced button edges for processing in task context.
static void IRAM_ATTR gpio_button_isr_handler(void *arg) {
  static int64_t last_edge_time_us = 0;
  int64_t timestamp_us = esp_timer_get_time();

  if (last_edge_time_us != 0 &&
      timestamp_us - last_edge_time_us < (int64_t)DEBOUNCE_MS * 1000) {
    return;
  }

  btn_event_t event = {
        .timestamp_us = timestamp_us,
        .is_press = (gpio_get_level(BUTTON_GPIO) == 0)
    };

  BaseType_t higher_priority_task_woken = pdFALSE;
  if (xQueueSendFromISR(btn_queue, &event, &higher_priority_task_woken) == pdTRUE) {
    last_edge_time_us = timestamp_us;
    if (higher_priority_task_woken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

static void gpio_init(void) {
  // Configure the seven-segment display and normal LED as outputs.
    gpio_config_t led_config = {};
    led_config.intr_type = GPIO_INTR_DISABLE;
    led_config.mode = GPIO_MODE_OUTPUT;
    led_config.pin_bit_mask =
      (1ULL << SEVEN_SEGMENT_A_GPIO) | 
      (1ULL << SEVEN_SEGMENT_B_GPIO) | 
      (1ULL << SEVEN_SEGMENT_C_GPIO) |
      (1ULL << SEVEN_SEGMENT_D_GPIO) | 
      (1ULL << SEVEN_SEGMENT_E_GPIO) | 
      (1ULL << SEVEN_SEGMENT_F_GPIO) |
      (1ULL << SEVEN_SEGMENT_G_GPIO) | 
      (1ULL << SEVEN_SEGMENT_DP_GPIO);
    led_config.pull_down_en = 0;
    led_config.pull_up_en = 0;
    ESP_ERROR_CHECK(gpio_config(&led_config));

  // Configure the button as an input with an internal pull-up resistor.
  gpio_config_t button_config = {};
  button_config.intr_type = GPIO_INTR_ANYEDGE;       // Detect press and release
  button_config.mode = GPIO_MODE_INPUT;
  button_config.pin_bit_mask = (1ULL << BUTTON_GPIO);
  button_config.pull_down_en = 0;
  button_config.pull_up_en = 1;
  ESP_ERROR_CHECK(gpio_config(&button_config));

  // Install the GPIO interrupt service and button handler.
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(
    BUTTON_GPIO,
    gpio_button_isr_handler,
    NULL
  ));
  ESP_ERROR_CHECK(gpio_intr_enable(BUTTON_GPIO));
}

static void event_task(void *arg)
{
    int display_number = 0;
    bool button_down = false;
    bool long_press_started = false;
    bool click_pending = false;
    bool double_click_candidate = false;
    int64_t press_started_ms = 0;
    int64_t first_release_ms = 0;
    int64_t next_repeat_ms = 0;

    btn_event_t event;
    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t wait_ms = REPEAT_PERIOD_MS;
        if (button_down) {
            int64_t deadline_ms = long_press_started
                ? next_repeat_ms
                : press_started_ms + LONG_PRESS_MS;
            if (deadline_ms - now_ms < wait_ms) {
                wait_ms = deadline_ms - now_ms;
            }
        }
        if (click_pending &&
            first_release_ms + DOUBLE_CLICK_MS - now_ms < wait_ms) {
            wait_ms = first_release_ms + DOUBLE_CLICK_MS - now_ms;
        }
        if (wait_ms < 1) {
            wait_ms = 1;
        }

        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }

        bool event_received =
            xQueueReceive(btn_queue, &event, wait_ticks) == pdTRUE;
        int64_t timer_now_ms = event_received
            ? event.timestamp_us / 1000
            : esp_timer_get_time() / 1000;

        if (click_pending &&
            timer_now_ms - first_release_ms > DOUBLE_CLICK_MS) {
            click_pending = false;
        }

        if (button_down) {
            if (!long_press_started &&
                timer_now_ms - press_started_ms >= LONG_PRESS_MS) {
                display_number = (display_number + 1) % sizeof(segment_map);
                set_display_seven_segment(display_number);
                long_press_started = true;
                next_repeat_ms = press_started_ms + LONG_PRESS_MS + REPEAT_PERIOD_MS;
            }

            while (long_press_started && timer_now_ms >= next_repeat_ms) {
                display_number = (display_number + 1) % sizeof(segment_map);
                set_display_seven_segment(display_number);
                next_repeat_ms += REPEAT_PERIOD_MS;
            }
        }

        if (event_received) {
            int64_t event_time_ms = event.timestamp_us / 1000;

            if (event.is_press) {
                if (!button_down) {
                    if (click_pending) {
                        if (event_time_ms - first_release_ms <= DOUBLE_CLICK_MS) {
                            // Undo the first click's immediate increment.
                            display_number =
                                (display_number + sizeof(segment_map) - 1) % sizeof(segment_map);
                            set_display_seven_segment(display_number);
                            double_click_candidate = true;
                            click_pending = false;
                        } else {
                            click_pending = false;
                            double_click_candidate = false;
                        }
                    }

                    button_down = true;
                    press_started_ms = event_time_ms;
                    long_press_started = false;
                }
            } else if (button_down) {
                bool was_long_press = long_press_started;
                button_down = false;
                long_press_started = false;

                if (double_click_candidate && !was_long_press) {
                    display_number =
                        (display_number + sizeof(segment_map) - 1) % sizeof(segment_map);
                    set_display_seven_segment(display_number);
                    double_click_candidate = false;
                } else if (!was_long_press) {
                    display_number = (display_number + 1) % sizeof(segment_map);
                    set_display_seven_segment(display_number);
                    click_pending = true;
                    first_release_ms = event_time_ms;
                    double_click_candidate = false;
                } else {
                    double_click_candidate = false;
                }
            }
        }
    }
}

// Initialize the queue, GPIO, timer, and event-processing task.
void app_main(void)
{
    btn_queue = xQueueCreate(10, sizeof(btn_event_t));
    ESP_ERROR_CHECK(btn_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    gpio_init();

    set_display_seven_segment(0);

    BaseType_t task_created = xTaskCreate(
        event_task,
        "gpio_event_task",
        2048,
        NULL,
        10,
        NULL
    );

    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
