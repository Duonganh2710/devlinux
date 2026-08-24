#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
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

//static volatile bool button_status = false;
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

// Send a button event to the queue when the button is pressed.
static void IRAM_ATTR gpio_button_isr_handler(void *arg) {
  //button_status = (gpio_get_level(BUTTON_GPIO) == 0);
  btn_event_t event = {
        .timestamp_us = esp_timer_get_time(),
        .is_press = (gpio_get_level(BUTTON_GPIO) == 0)
    };

    xQueueSend(btn_queue, &event, 0);
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
    int64_t previous_time_ms = 0;
    int64_t current_time_ms = 0;

    int64_t current_time_count_long_click_ms = 0;
    int64_t previous_time_count_long_click_ms = 0;
    int64_t first_time_count_long_click_ms = 0;

    int display_number = 0;
    bool long_click = false; 
    bool single_click = false; 

    btn_event_t event;
    while(1)
    {
        if (xQueueReceive(btn_queue, &event, pdMS_TO_TICKS(DEBOUNCE_MS)) == pdTRUE)
        {

            if (event.is_press)
            {
                previous_time_ms = current_time_ms;
                current_time_ms = event.timestamp_us / 1000;
                long_click = true;

                current_time_count_long_click_ms = 0;
                previous_time_count_long_click_ms = 0;
                first_time_count_long_click_ms = 0;
            }
            else 
            {
                long_click = false;
                current_time_count_long_click_ms = 0;
                previous_time_count_long_click_ms = 0;
                first_time_count_long_click_ms = 0;

                if(current_time_ms != 0 && previous_time_ms == 0) 
                {
                    if(event.timestamp_us / 1000 - current_time_ms >= DOUBLE_CLICK_MS){
                        // SINGLE CLICK → +1
                        display_number = (display_number + 1) % sizeof(segment_map);
                        set_display_seven_segment(display_number);

                        current_time_ms = 0;
                        previous_time_ms = 0;
                        single_click = false;
                    }else{
                            single_click = true;
                    }
                    
                }
                else if(current_time_ms != 0 && previous_time_ms != 0){
                    if(((event.timestamp_us / 1000) - previous_time_ms) <= DOUBLE_CLICK_MS)
                    {
                        //printf("value: %lld\n", event.timestamp_us / 1000 -  previous_time_ms );
                        // DOUBLE CLICK → -1
                        display_number =
                            (display_number + sizeof(segment_map) - 1)
                            % sizeof(segment_map);

                        set_display_seven_segment(display_number);

                        current_time_ms = 0;
                        previous_time_ms = 0;

                    }  
                }
            }
        }

        if(long_click)
        {
            if(first_time_count_long_click_ms == 0)
            {
                first_time_count_long_click_ms = esp_timer_get_time() / 1000;
            }
            current_time_count_long_click_ms = esp_timer_get_time() / 1000;

            if(current_time_count_long_click_ms -  first_time_count_long_click_ms >= LONG_PRESS_MS && 
                current_time_count_long_click_ms - previous_time_count_long_click_ms >= REPEAT_PERIOD_MS)
            {
                display_number = (display_number + 1) % sizeof(segment_map);
                set_display_seven_segment(display_number);

                previous_time_count_long_click_ms = current_time_count_long_click_ms;
            }
        }

        if(single_click)
        {
            if(esp_timer_get_time() / 1000 - current_time_ms > DOUBLE_CLICK_MS 
            &&  esp_timer_get_time() / 1000 - current_time_ms < LONG_PRESS_MS)
            {
                display_number = (display_number + 1) % sizeof(segment_map);
                    set_display_seven_segment(display_number);

                single_click = false;
                current_time_ms = 0;
                previous_time_ms = 0;
            }

            printf("value: %lld\n", esp_timer_get_time() / 1000 - current_time_ms );
        }

        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
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
