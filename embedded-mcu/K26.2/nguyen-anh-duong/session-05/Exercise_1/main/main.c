#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <string.h>

/* DevKitC-1 v1.1, onboard addressable RGB LED on GPIO38. */
#define UART_PORT_NUM_0   UART_NUM_0
#define UART_TX_PIN_43    GPIO_NUM_43
#define UART_RX_PIN_44    GPIO_NUM_44
#define UART_BAUD_RATE    115200UL
#define UART_BUF_SIZE     1024U
#define UART_QUEUE_SIZE   10U
#define CMD_BUF_SIZE      64U
#define UART_TASK_STACK_SIZE 4096U
#define UART_TASK_PRIORITY   10U
#define UART_TASK_CORE       0
#define RGB_LED_PIN       GPIO_NUM_38
#define RGB_LED_COUNT     1U
#define RGB_LED_INDEX     0U

#define CMD_LED_ON        "LED_ON"
#define CMD_LED_OFF       "LED_OFF"
#define CMD_RED           "RED"
#define CMD_GREEN         "GREEN"
#define CMD_BLUE          "BLUE"
#define UART_TAG          "UART_CONSOLE"
#define MSG_NEWLINE       "\n"
#define MSG_BACKSPACE     "\b \b"
#define MSG_UNKNOWN       "Unknown command\r\n"
#define MSG_TOO_LONG      "Command is oversize\r\n"
#define MSG_UART_OVERFLOW "UART overflow, input flushed\r\n"

// UART event queue and RGB LED driver handle.
static QueueHandle_t uart_queue;
static led_strip_handle_t rgb_led;

static void rgb_led_init(void)
{
    // Configure one addressable RGB LED connected to GPIO 38.
    const led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = RGB_LED_COUNT,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = true,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));
}

static void uart_interrupt_init(void)
{
    // Configure UART0 for 115200 baud and 8N1 communication.
    const uart_config_t uart_config_0 = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(
        UART_PORT_NUM_0, 
        UART_BUF_SIZE * 2, 
        UART_BUF_SIZE * 2,
        UART_QUEUE_SIZE, 
        &uart_queue, 
        0));
    ESP_ERROR_CHECK(uart_param_config(
        UART_PORT_NUM_0, 
        &uart_config_0));
    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT_NUM_0, 
        UART_TX_PIN_43, 
        UART_RX_PIN_44, 
        UART_PIN_NO_CHANGE, 
        UART_PIN_NO_CHANGE));
}

static void command_handler(const char *cmd_buf)
{
    // Compare the completed text command and control the RGB LED.
    ESP_LOGI(UART_TAG, "Received command: \"%s\"", cmd_buf);
    if (strcmp(cmd_buf, CMD_LED_ON) == 0) {
        ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, RGB_LED_INDEX, 32, 32, 32));
        ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
        ESP_LOGI(UART_TAG, "LED -> WHITE");
    } else if (strcmp(cmd_buf, CMD_RED) == 0) {
        ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, RGB_LED_INDEX, 32, 0, 0));
        ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
        ESP_LOGI(UART_TAG, "LED -> RED");
    } else if (strcmp(cmd_buf, CMD_GREEN) == 0) {
        ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, RGB_LED_INDEX, 0, 32, 0));
        ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
        ESP_LOGI(UART_TAG, "LED -> GREEN");
    } else if (strcmp(cmd_buf, CMD_BLUE) == 0) {
        ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, RGB_LED_INDEX, 0, 0, 32));
        ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
        ESP_LOGI(UART_TAG, "LED -> BLUE");
    } else if (strcmp(cmd_buf, CMD_LED_OFF) == 0) {
        ESP_ERROR_CHECK(led_strip_clear(rgb_led));
        ESP_LOGI(UART_TAG, "LED -> OFF");
    } else {
        uart_write_bytes(UART_PORT_NUM_0, MSG_UNKNOWN, sizeof(MSG_UNKNOWN) - 1);
        ESP_LOGW(UART_TAG, "Unknown command: \"%s\"", cmd_buf);
    }
}

static void uart_event_task(void *task_parameter)
{
    (void) task_parameter;
    uart_event_t event;
    char cmd_buf[CMD_BUF_SIZE];
    uint8_t data[UART_BUF_SIZE];
    int cmd_length = 0;
    bool line_too_long = false;

    // Wait for UART events and process the received line.
    while (1) {
        if (xQueueReceive(
                uart_queue,
                &event,
                portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case UART_DATA:
                {
                    // Read received bytes from the UART RX buffer.
                    int len = uart_read_bytes(UART_PORT_NUM_0, data, event.size, pdMS_TO_TICKS(20));
                    if (len > 0) 
                    {
                        for (int index = 0; index < len; index++) 
                        {
                            char c = (char) data[index];
                            if (c == '\r' || c == '\n') {
                                // Enter submits the current command.
                                cmd_buf[cmd_length] = '\0';
                                uart_write_bytes(UART_PORT_NUM_0, MSG_NEWLINE, sizeof(MSG_NEWLINE) - 1);
                                if (cmd_buf[0] != '\0' && !line_too_long) {
                                    command_handler(cmd_buf);
                                }
                                cmd_length = 0;
                                line_too_long = false;
                            } else if (c == '\b' || c == 0x7F) {
                                // Backspace and DEL remove the last character.
                                if (cmd_length > 0) {
                                    cmd_length--;
                                    uart_write_bytes(UART_PORT_NUM_0, MSG_BACKSPACE, sizeof(MSG_BACKSPACE) - 1);
                                }
                            } else if (!line_too_long && cmd_length < CMD_BUF_SIZE - 1) {
                                // Store normal characters until the line is complete.
                                cmd_buf[cmd_length] = c;
                                cmd_length++;
                                uart_write_bytes(UART_PORT_NUM_0, &c, 1);
                            } else if (!line_too_long) {
                                // Reject an input line that is too long.
                                uart_write_bytes(UART_PORT_NUM_0, MSG_TOO_LONG, sizeof(MSG_TOO_LONG) - 1);
                                ESP_LOGW(UART_TAG, "Command is oversize");
                                line_too_long = true;
                            }

                            
                        }
                    }
                    break;
                }
                case UART_BUFFER_FULL:
                case UART_FIFO_OVF:
                {
                    // Discard corrupted or overflowing input and reset events.
                    uart_flush_input(UART_PORT_NUM_0);
                    xQueueReset(uart_queue);
                    cmd_length = 0;
                    line_too_long = false;

                    // Report the overflow to the terminal and log.
                    uart_write_bytes(UART_PORT_NUM_0, MSG_UART_OVERFLOW, sizeof(MSG_UART_OVERFLOW) - 1);
                    ESP_LOGW(UART_TAG, "UART overflow, input flushed");
                    break;
                }
                default:    
                    ESP_LOGW(UART_TAG, "Unhandled UART event: %d", event.type);
                    break;
            }
        }
    }
}

void app_main(void)
{
    BaseType_t task_result;

    uart_interrupt_init();
    rgb_led_init();
    // Keep UART work in its own task.
    task_result = xTaskCreatePinnedToCore(
        uart_event_task,
        "uart_event_task",
        UART_TASK_STACK_SIZE,
        NULL,
        UART_TASK_PRIORITY,
        NULL,
        UART_TASK_CORE);
    configASSERT(task_result == pdPASS);
    ESP_LOGI(UART_TAG, "Console ready on UART0, 115200-8-N-1");
}
