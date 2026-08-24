#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "freertos/task.h"
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
#define TASK_DELAY_MS 25U
#define DOUBLE_CLICK_MS 350U
#define LONG_PRESS_MS 800U
#define REPEAT_PERIOD_MS 500U

static volatile bool button_status = false;

// Common-cathode map: bits b0 to b6 represent segments a to g.
static const uint8_t segment_map[10] = {
  0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
  0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
};

// Display a digit on the seven-segment display.
static void set_display_seven_segment(int value) {
  if (value < 0 || value > 9) value = 0;
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

// Send a button event to the queue when the button is pressed.
static void IRAM_ATTR gpio_button_isr_handler(void *arg) {
  button_status = (gpio_get_level(BUTTON_GPIO) == 0);
}

static void event_task(void *arg)
{
    int display_number = 0;

    int64_t press_time_ms = 0;
    int64_t first_click_time_ms = 0;

    bool waiting_double_click = false;
    bool handled_hold = false;

    while (1)
    {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (button_status)
        {
            // Button vừa được nhấn
            if (press_time_ms == 0)
            {
                press_time_ms = now_ms;
                handled_hold = false;
            }

            int64_t hold_time_ms = now_ms - press_time_ms;

            // Long press
            if (hold_time_ms >= LONG_PRESS_MS && !handled_hold)
            {
                handled_hold = true;

                while (button_status)
                {
                    display_number =
                        (display_number + 1) % sizeof(segment_map);

                    set_display_seven_segment(display_number);

                    vTaskDelay(pdMS_TO_TICKS(REPEAT_PERIOD_MS));
                }

                press_time_ms = 0;
                waiting_double_click = false;
            }
        }
        else
        {
            // Button vừa được nhả
            if (press_time_ms != 0)
            {
                int64_t hold_time_ms = now_ms - press_time_ms;

                press_time_ms = 0;

                // Không phải long press
                if (!handled_hold && hold_time_ms < LONG_PRESS_MS)
                {
                    if (waiting_double_click &&
                        (now_ms - first_click_time_ms) <= DOUBLE_CLICK_MS)
                    {
                        // DOUBLE CLICK → -1
                        display_number =
                            (display_number + sizeof(segment_map) - 1)
                            % sizeof(segment_map);

                        set_display_seven_segment(display_number);

                        waiting_double_click = false;
                    }
                    else
                    {
                        // Click lần 1 → chờ click thứ 2
                        first_click_time_ms = now_ms;
                        waiting_double_click = true;
                    }
                }
            }
        }

        // Hết thời gian chờ double click
        if (waiting_double_click &&
            (now_ms - first_click_time_ms) > DOUBLE_CLICK_MS)
        {
            // SINGLE CLICK → +1
            display_number =
                (display_number + 1) % sizeof(segment_map);

            set_display_seven_segment(display_number);

            waiting_double_click = false;
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
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

// Initialize the queue, GPIO, timer, and event-processing task.
void app_main(void) {
  // Start the task that processes queue events.
  BaseType_t task_created = xTaskCreate(
    event_task,
    "gpio_event_task",
    2048,
    NULL,
    10,
    NULL
  );
  ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

  // Initialize GPIO pins and interrupts.
  gpio_init();

  // Set the initial display states.
  set_display_seven_segment(0);
}
