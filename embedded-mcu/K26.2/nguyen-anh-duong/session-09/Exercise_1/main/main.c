#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "ADC";

#define LCD_HOST          SPI2_HOST
#define PIN_LCD_SCK       GPIO_NUM_12
#define PIN_LCD_MOSI      GPIO_NUM_11
#define PIN_LCD_MISO      GPIO_NUM_13
#define PIN_LCD_CS        GPIO_NUM_10
#define PIN_LCD_RS        GPIO_NUM_9
#define PIN_LCD_RST       GPIO_NUM_14
#define PIN_LCD_BL        GPIO_NUM_2

#define LCD_H_RES         (480U)
#define LCD_V_RES         (320U)
#define LCD_CLK_HZ        (20 * 1000 * 1000)

/* 480 * 16 pixels per transfer = 15360 bytes, same as max_transfer_sz */
#define LCD_CHUNK_LINES   (16U)
#define LCD_CHUNK_PIXELS  (LCD_H_RES * LCD_CHUNK_LINES)
#define LCD_CHUNK_BYTES   (LCD_CHUNK_PIXELS * 2U)

#define CMD_SWRESET       (0x01U)
#define CMD_SLPOUT        (0x11U)
#define CMD_INVON         (0x21U)
#define CMD_DISPON        (0x29U)
#define CMD_CASET         (0x2AU)
#define CMD_RASET         (0x2BU)
#define CMD_RAMWR         (0x2CU)
#define CMD_MADCTL        (0x36U)
#define CMD_COLMOD        (0x3AU)

#define COLMOD_RGB565     (0x55U)

#define MADCTL_MV         (0x20U)
#define MADCTL_BGR        (0x08U)
#define MADCTL_LANDSCAPE  (MADCTL_MV | MADCTL_BGR)

#define COLOUR_BG         (0x0000U)
#define COLOUR_BAR        (0x07E0U)
#define COLOUR_FRAME      (0xFFFFU)

#define BAR_X             (40U)
#define BAR_Y             (140U)
#define BAR_MAX_W         (400U)
#define BAR_H             (40U)
#define BAR_FRAME         (1U)

#define ADC_UNIT_USED     ADC_UNIT_1
#define ADC_CHANNEL_POT   ADC_CHANNEL_0   /* GPIO1 */

/* 12 dB attenuation covers about 0 - 3.1 V, so the 3.3 V rail at the top of
   the pot is slightly out of range and the knob reads a bit under 3300 mV. */
#define ADC_ATTEN_USED    ADC_ATTEN_DB_12
#define ADC_BITWIDTH_USED ADC_BITWIDTH_DEFAULT

/* The knob stops around raw 10 at one end and around 4050 at the other, so
   neither end of 0..4095 is ever reached. */
#define ADC_RAW_FULL_SCALE (4095)
#define SAMPLE_COUNT      (16U)
#define UPDATE_PERIOD_MS  (200U)
#define VOLTAGE_MAX_MV    (3300)

static spi_device_handle_t lcd_spi;
static uint16_t *lcd_tx_buf;

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle;

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

/* vendor sequence, from TFT_eSPI TFT_Drivers/ST7796_Init.h */
static const lcd_init_cmd_t lcd_init_cmds[] = {
    {CMD_SWRESET, {0}, 0, 120},
    {CMD_SLPOUT,  {0}, 0, 120},
    {0xF0, {0xC3}, 1, 0},
    {0xF0, {0x96}, 1, 0},
    {CMD_MADCTL, {MADCTL_LANDSCAPE}, 1, 0},
    {CMD_COLMOD, {COLMOD_RGB565}, 1, 0},
    {0xB4, {0x01}, 1, 0},
    {0xB6, {0x80, 0x02, 0x3B}, 3, 0},
    {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xC1, {0x06}, 1, 0},
    {0xC2, {0xA7}, 1, 0},
    {0xC5, {0x18}, 1, 120},
    {0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F,
            0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 14, 0},
    {0xE1, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B,
            0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 14, 120},
    {0xF0, {0x3C}, 1, 0},
    {0xF0, {0x69}, 1, 0},
    {CMD_INVON, {0}, 0, 0},
    {CMD_DISPON, {0}, 0, 120},
};

static void lcd_write_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_RS, 0);

    spi_transaction_t t = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {cmd},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

static void lcd_write_data(const uint8_t *data, size_t len)
{
    if (len == 0U) {
        return;
    }

    gpio_set_level(PIN_LCD_RS, 1);

    spi_transaction_t t = {0};
    t.length = len * 8;

    if (len <= sizeof(t.tx_data)) {
        memcpy(t.tx_data, data, len);
        t.flags = SPI_TRANS_USE_TXDATA;
    } else {
        t.tx_buffer = data;
    }

    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

static void lcd_write_cmd_data(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    lcd_write_cmd(cmd);
    lcd_write_data(data, len);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t caset[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                              (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t raset[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                              (uint8_t)(y1 >> 8), (uint8_t)y1};

    lcd_write_cmd(CMD_CASET);
    lcd_write_data(caset, sizeof(caset));
    lcd_write_cmd(CMD_RASET);
    lcd_write_data(raset, sizeof(raset));
    lcd_write_cmd(CMD_RAMWR);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour)
{
    if ((w == 0U) || (h == 0U)) {
        return;
    }

    lcd_set_window(x, y, x + w - 1U, y + h - 1U);

    const uint16_t be_colour = (uint16_t)((colour >> 8) | (colour << 8));
    for (size_t i = 0; i < LCD_CHUNK_PIXELS; i++) {
        lcd_tx_buf[i] = be_colour;
    }

    size_t left = (size_t)w * h;
    while (left > 0U) {
        size_t px = (left > LCD_CHUNK_PIXELS) ? LCD_CHUNK_PIXELS : left;
        lcd_write_data((const uint8_t *)lcd_tx_buf, px * 2U);
        left -= px;
    }
}

static void lcd_draw_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t thickness, uint16_t colour)
{
    lcd_fill_rect(x, y, w, thickness, colour);
    lcd_fill_rect(x, y + h - thickness, w, thickness, colour);
    lcd_fill_rect(x, y, thickness, h, colour);
    lcd_fill_rect(x + w - thickness, y, thickness, h, colour);
}

static void lcd_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_RS) | (1ULL << PIN_LCD_RST) | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void lcd_spi_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev_cfg, &lcd_spi));

    lcd_tx_buf = heap_caps_malloc(LCD_CHUNK_BYTES, MALLOC_CAP_DMA);
    assert(lcd_tx_buf != NULL);
}

static void lcd_init(void)
{
    lcd_gpio_init();
    lcd_spi_init();

    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        const lcd_init_cmd_t *c = &lcd_init_cmds[i];
        lcd_write_cmd_data(c->cmd, c->data, c->len);
        if (c->delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_BL, 1));
}

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_POT, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_USED,
        .chan = ADC_CHANNEL_POT,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_USED,
    };

    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no curve fitting data (%s), mV values will be an uncalibrated estimate",
                 esp_err_to_name(err));
        cali_handle = NULL;
    }
}

static int adc_read_average(void)
{
    uint32_t sum = 0;
    int raw = 0;

    for (unsigned int i = 0; i < SAMPLE_COUNT; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_POT, &raw));
        sum += (uint32_t)raw;
    }

    return (int)(sum / SAMPLE_COUNT);
}

static int raw_to_mv(int raw)
{
    int mv = 0;

    if (cali_handle != NULL) {
        if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) == ESP_OK) {
            return mv;
        }
        ESP_LOGW(TAG, "calibration failed on raw=%d", raw);
    }

    return raw * VOLTAGE_MAX_MV / ADC_RAW_FULL_SCALE;
}

void app_main(void)
{
    lcd_init();
    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOUR_BG);
    lcd_draw_frame(BAR_X - BAR_FRAME, BAR_Y - BAR_FRAME,
                   BAR_MAX_W + 2U * BAR_FRAME, BAR_H + 2U * BAR_FRAME,
                   BAR_FRAME, COLOUR_FRAME);

    adc_init();

    uint16_t old_w = 0;

    while (true) {
        int raw = adc_read_average();
        int mv = raw_to_mv(raw);
        uint16_t new_w = (uint16_t)((uint32_t)mv * BAR_MAX_W / VOLTAGE_MAX_MV);

        if (new_w > BAR_MAX_W) {
            new_w = BAR_MAX_W;
        }

        ESP_LOGI(TAG, "raw=%5d -> %5d mV", raw, mv);

        if (new_w > old_w) {
            lcd_fill_rect(BAR_X + old_w, BAR_Y, new_w - old_w, BAR_H, COLOUR_BAR);
        } else if (new_w < old_w) {
            lcd_fill_rect(BAR_X + new_w, BAR_Y, old_w - new_w, BAR_H, COLOUR_BG);
        }
        old_w = new_w;

        vTaskDelay(pdMS_TO_TICKS(UPDATE_PERIOD_MS));
    }
}