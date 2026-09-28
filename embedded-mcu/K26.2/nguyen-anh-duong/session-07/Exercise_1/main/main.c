/* Session 07 - exercise 1
 *
 * FT6336U touch panel on I2C, ST7796 display still on SPI.
 * The display half is the session 06 code, left alone.
 */

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TOUCH";

/* ---------- display, SPI (session 06, untouched) ---------- */
#define LCD_HOST      SPI2_HOST
#define PIN_SCK       GPIO_NUM_12
#define PIN_MOSI      GPIO_NUM_11
#define PIN_MISO      GPIO_NUM_13
#define PIN_CS        GPIO_NUM_10
#define PIN_RS        GPIO_NUM_9
#define PIN_RST       GPIO_NUM_14
#define PIN_BK_LIGHT  GPIO_NUM_2

#define LCD_H_RES     (480U) /* landscape: width  */
#define LCD_V_RES     (320U) /* landscape: height */
#define LCD_CLK_HZ    (20 * 1000 * 1000) /* start here; raise it once it works */

/* ST7796 MADCTL bit definitions. */
#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_ML       0x10
#define MADCTL_BGR      0x08
#define MADCTL_MH       0x04

/* Pixel format / colour-depth value for COLMOD. */
#define PIXFMT_RGB565  (0x55U)

/* ST7796 command bytes (named constants). */
#define CMD_SLPOUT     (0x11U)
#define CMD_INVON      (0x21U)
#define CMD_DISPON     (0x29U)
#define CMD_CASET      (0x2AU)
#define CMD_RASET      (0x2BU)
#define CMD_RAMWR      (0x2CU)
#define CMD_MADCTL     (0x36U)
#define CMD_COLMOD     (0x3AU)
#define CMD_CMDCTL     (0xF0U) /* command-set control (enable/lock ext. cmds) */
#define CMD_PORCH      (0xB4U) /* porch / inversion control */
#define CMD_GATE       (0xB7U) /* gate control */
#define CMD_PWR1       (0xC0U) /* power control 1 */
#define CMD_PWR2       (0xC1U) /* power control 2 */
#define CMD_PWR3       (0xC2U) /* power control 3 */
#define CMD_VCOM       (0xC5U) /* VCOM control */
#define CMD_FRCTRL     (0xE8U) /* display timing / frame-rate control */
#define CMD_GAMMAP     (0xE0U) /* positive voltage gamma correction */
#define CMD_GAMMAN     (0xE1U) /* negative voltage gamma correction */

/* RGB565 colours. */
#define COLOUR_RED     (0xF800U)
#define COLOUR_GREEN   (0x07E0U)
#define COLOUR_BLUE    (0x001FU)
#define COLOUR_WHITE   (0xFFFFU)
#define COLOUR_BLACK   (0x0000U)

#define LCD_FILL_CHUNK_PIXELS 80

static spi_device_handle_t spi_handler;

static void spi_init(void)
{
    spi_bus_config_t spi_bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_FILL_CHUNK_PIXELS * sizeof(uint16_t)
    };
    ESP_ERROR_CHECK(spi_bus_initialize(
        LCD_HOST,
        &spi_bus,
        SPI_DMA_CH_AUTO     /* Enable the DMA feature */
    ));

    spi_device_interface_config_t spi_dev_config = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(
        LCD_HOST,
        &spi_dev_config,
        &spi_handler));
}

static void lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &cmd
    };
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 0));
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handler, &transaction));
}

static void lcd_write_data(const uint8_t *buf, size_t len)
{
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = buf
    };
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 1));
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handler, &transaction));
}

static void lcd_reset(void)
{
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* Initialize the 3.5-inch IPS SPI display with the ST7796 controller. */
static void lcd_init(void)
{
    /* Reset the controller, then leave sleep mode. */
    lcd_reset();
    lcd_write_cmd(CMD_SLPOUT);

    /* Allow the display oscillator and internal circuits to start. */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Memory access control: landscape orientation and BGR order. */
    const uint8_t madctl = MADCTL_MV | MADCTL_BGR;
    lcd_write_cmd(CMD_MADCTL);
    lcd_write_data(&madctl, sizeof(madctl));

    /* Pixel format: 16 bits per pixel (RGB565). */
    const uint8_t pixfmt = PIXFMT_RGB565;
    lcd_write_cmd(CMD_COLMOD);
    lcd_write_data(&pixfmt, sizeof(pixfmt));

    /* Enable manufacturer command set. */
    const uint8_t ext_mfr[] = { 0xC3 };
    lcd_write_cmd(CMD_CMDCTL);
    lcd_write_data(ext_mfr, sizeof(ext_mfr));

    /* Enable extended command access. */
    const uint8_t ext_access[] = { 0x96 };
    lcd_write_cmd(CMD_CMDCTL);
    lcd_write_data(ext_access, sizeof(ext_access));

    /* Display inversion and porch settings. */
    const uint8_t porch[] = { 0x02 };
    lcd_write_cmd(CMD_PORCH);
    lcd_write_data(porch, sizeof(porch));

    /* Entry mode and gate control settings. */
    const uint8_t gate[] = { 0xC6 };
    lcd_write_cmd(CMD_GATE);
    lcd_write_data(gate, sizeof(gate));

    /* Power and VCOM settings. */
    const uint8_t pwr1[] = { 0xC0, 0x00 };
    lcd_write_cmd(CMD_PWR1);
    lcd_write_data(pwr1, sizeof(pwr1));

    const uint8_t pwr2[] = { 0x13 };
    lcd_write_cmd(CMD_PWR2);
    lcd_write_data(pwr2, sizeof(pwr2));

    const uint8_t pwr3[] = { 0xA7 };
    lcd_write_cmd(CMD_PWR3);
    lcd_write_data(pwr3, sizeof(pwr3));

    const uint8_t vcom[] = { 0x21 };
    lcd_write_cmd(CMD_VCOM);
    lcd_write_data(vcom, sizeof(vcom));

    /* Frame rate / timing. */
    const uint8_t frctrl[] = { 0x40, 0x8A, 0x1B, 0x1B, 0x23, 0x0A, 0xAC, 0x33 };
    lcd_write_cmd(CMD_FRCTRL);
    lcd_write_data(frctrl, sizeof(frctrl));

    /* Positive voltage gamma correction. */
    const uint8_t gammap[] = {
        0xD2, 0x05, 0x08, 0x06, 0x05, 0x02, 0x2A, 0x44,
        0x46, 0x39, 0x15, 0x15, 0x2D, 0x32
    };
    lcd_write_cmd(CMD_GAMMAP);
    lcd_write_data(gammap, sizeof(gammap));

    /* Negative voltage gamma correction. */
    const uint8_t gamman[] = {
        0x96, 0x08, 0x0C, 0x09, 0x09, 0x25, 0x2E, 0x43,
        0x42, 0x35, 0x11, 0x11, 0x28, 0x2E
    };
    lcd_write_cmd(CMD_GAMMAN);
    lcd_write_data(gamman, sizeof(gamman));

    /* Lock the extended command set. */
    const uint8_t ext_lock1[] = { 0x3C };
    lcd_write_cmd(CMD_CMDCTL);
    lcd_write_data(ext_lock1, sizeof(ext_lock1));

    /* Lock the manufacturer command set. */
    const uint8_t ext_lock2[] = { 0x69 };
    lcd_write_cmd(CMD_CMDCTL);
    lcd_write_data(ext_lock2, sizeof(ext_lock2));

    /* Enable display inversion, wait for the panel, then turn it on. */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_write_cmd(CMD_INVON);
    lcd_write_cmd(CMD_DISPON);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* Column address set: X start and end, high byte first. */
    const uint8_t caset[] = {
        (uint8_t)(x0 >> 8), (uint8_t)x0,
        (uint8_t)(x1 >> 8), (uint8_t)x1
    };
    lcd_write_cmd(CMD_CASET);
    lcd_write_data(caset, sizeof(caset));

    /* Row address set: Y start and end, high byte first. */
    const uint8_t raset[] = {
        (uint8_t)(y0 >> 8), (uint8_t)y0,
        (uint8_t)(y1 >> 8), (uint8_t)y1
    };
    lcd_write_cmd(CMD_RASET);
    lcd_write_data(raset, sizeof(raset));

    /* Start writing pixel data to the selected window. */
    lcd_write_cmd(CMD_RAMWR);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                          uint16_t colour)
{
    static uint8_t pixel_buffer[LCD_FILL_CHUNK_PIXELS * 2];
    const uint32_t pixel_count = (uint32_t)width * height;
    const uint8_t colour_high = (uint8_t)(colour >> 8);
    const uint8_t colour_low = (uint8_t)colour;

    if (width == 0 || height == 0) {
        return;
    }

    lcd_set_window(x, y, x + width - 1, y + height - 1);

    uint32_t pixels_sent = 0;
    while (pixels_sent < pixel_count) {
        uint32_t chunk_pixels = pixel_count - pixels_sent;
        if (chunk_pixels > LCD_FILL_CHUNK_PIXELS) {
            chunk_pixels = LCD_FILL_CHUNK_PIXELS;
        }

        for (uint32_t index = 0; index < chunk_pixels; index++) {
            pixel_buffer[index * 2] = colour_high;
            pixel_buffer[index * 2 + 1] = colour_low;
        }

        lcd_write_data(pixel_buffer, chunk_pixels * 2);
        pixels_sent += chunk_pixels;
    }
}

/* ---------- touch controller, FT6336U on I2C ----------
 *
 * extra wires for this one:
 *   SDA=GPIO4  SCL=GPIO5  RST=GPIO6  INT=GPIO7
 * INT is wired up but we're polling, so it just sits there.
 */

#define PIN_TOUCH_SDA    GPIO_NUM_4
#define PIN_TOUCH_SCL    GPIO_NUM_5
#define PIN_TOUCH_RST    GPIO_NUM_6
#define PIN_TOUCH_INT    GPIO_NUM_7

#define TOUCH_I2C_PORT   I2C_NUM_0
#define TOUCH_I2C_ADDR   (0x38U)
#define I2C_CLK_HZ       (400000U)
#define I2C_TIMEOUT_MS   (100)   /* ms, polling so no point waiting longer */
#define POLL_PERIOD_MS   (50U)   /* ~20 polls per second */

/* FT6336U registers. */
#define REG_TD_STATUS    (0x02U) /* low nibble = number of touch points */
#define REG_P1_XH        (0x03U) /* then XL, YH, YL in the next three registers */
#define REG_CHIP_ID      (0xA3U)
#define REG_VENDOR_ID    (0xA8U)

#define TOUCH_COORD_MASK (0x0FU) /* only the low 4 bits of the high byte are coordinate */
#define TOUCH_POINT_MASK (0x0FU) /* finger count lives in the low nibble */

#define TOUCH_READY_DELAY_MS (50U) /* controller boot time after reset */

#define TOUCH_SQUARE_SIZE (20U)

/* flip these only if the square lands mirrored on your panel */
#define TOUCH_INVERT_X   (0U)
#define TOUCH_INVERT_Y   (0U)

/* grab len bytes from reg, one write-then-read transaction */
static esp_err_t touch_read(i2c_master_dev_handle_t dev, uint8_t reg,
                            uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1U, buf, len, I2C_TIMEOUT_MS);
}

/* reset the controller before we talk to it */
static void touch_reset(void)
{
    ESP_ERROR_CHECK(gpio_set_level(PIN_TOUCH_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(gpio_set_level(PIN_TOUCH_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(TOUCH_READY_DELAY_MS));
}

static i2c_master_dev_handle_t touch_init(void)
{
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_dev_handle_t dev_handle = NULL;

    touch_reset();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = TOUCH_I2C_PORT,
        .sda_io_num                   = PIN_TOUCH_SDA,
        .scl_io_num                   = PIN_TOUCH_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_I2C_ADDR,
        .scl_speed_hz    = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &touch_cfg, &dev_handle));

    return dev_handle;
}

/* read both ids once so a dead bus shows up here and not in the touch loop */
static void touch_log_ids(i2c_master_dev_handle_t dev)
{
    uint8_t chip_id = 0;
    uint8_t vendor_id = 0;

    esp_err_t chip_err = touch_read(dev, REG_CHIP_ID, &chip_id, 1U);
    esp_err_t vendor_err = touch_read(dev, REG_VENDOR_ID, &vendor_id, 1U);

    if (chip_err != ESP_OK || vendor_err != ESP_OK) {
        ESP_LOGW(TAG, "ID read failed (chip=%s vendor=%s) - check wiring/pull-ups",
                 esp_err_to_name(chip_err), esp_err_to_name(vendor_err));
        return;
    }

    ESP_LOGI(TAG, "chip_id=0x%02X vendor_id=0x%02X", chip_id, vendor_id);

    /* 0x00 / 0xFF means nothing is actually answering on the bus */
    if (chip_id == 0x00 || chip_id == 0xFF || vendor_id == 0x00 || vendor_id == 0xFF) {
        ESP_LOGW(TAG, "ID values look bogus - verify SDA/SCL and pull-ups");
    }
}

static esp_err_t touch_read_point_count(i2c_master_dev_handle_t dev, uint8_t *count)
{
    uint8_t status = 0;
    esp_err_t err = touch_read(dev, REG_TD_STATUS, &status, 1U);
    if (err == ESP_OK) {
        *count = status & TOUCH_POINT_MASK;
    }
    return err;
}

/* point 1: P1_XH, P1_XL, P1_YH, P1_YL, all four in one transaction */
static esp_err_t touch_read_point(i2c_master_dev_handle_t dev,
                                  uint16_t *x, uint16_t *y)
{
    uint8_t raw[4] = { 0 };

    esp_err_t err = touch_read(dev, REG_P1_XH, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    /* only the low nibble of the high byte belongs to the coordinate */
    *x = (uint16_t)(((raw[0] & TOUCH_COORD_MASK) << 8) | raw[1]);
    *y = (uint16_t)(((raw[2] & TOUCH_COORD_MASK) << 8) | raw[3]);
    return ESP_OK;
}

static void touch_to_screen(uint16_t raw_x, uint16_t raw_y,
                            uint16_t *screen_x, uint16_t *screen_y)
{
    int32_t x = raw_y;
    int32_t y = raw_x;

    if (TOUCH_INVERT_X) {
        x = (int32_t)LCD_H_RES - 1 - x;
    }
    if (TOUCH_INVERT_Y) {
        y = (int32_t)LCD_V_RES - 1 - y;
    }

    *screen_x = (uint16_t)x;
    *screen_y = (uint16_t)y;
}

static void draw_touch_square(uint16_t x, uint16_t y)
{
    const int32_t half = TOUCH_SQUARE_SIZE / 2;
    int32_t left = (int32_t)x - half;
    int32_t top = (int32_t)y - half;

    /* clamp so the square never runs off the panel edge */
    if (left < 0) {
        left = 0;
    } else if (left > (int32_t)LCD_H_RES - (int32_t)TOUCH_SQUARE_SIZE) {
        left = (int32_t)LCD_H_RES - (int32_t)TOUCH_SQUARE_SIZE;
    }

    if (top < 0) {
        top = 0;
    } else if (top > (int32_t)LCD_V_RES - (int32_t)TOUCH_SQUARE_SIZE) {
        top = (int32_t)LCD_V_RES - (int32_t)TOUCH_SQUARE_SIZE;
    }

    lcd_fill_rect((uint16_t)left, (uint16_t)top,
                  TOUCH_SQUARE_SIZE, TOUCH_SQUARE_SIZE, COLOUR_WHITE);
}

/* ---------- gpio + main ---------- */

static void gpio_init(void)
{
    /* RS, both resets and the backlight are outputs */
    gpio_config_t output_config = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << PIN_RS) |
                        (1ULL << PIN_RST) |
                        (1ULL << PIN_BK_LIGHT) |
                        (1ULL << PIN_TOUCH_RST)
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    /* INT is unused, just configured as an input for now */
    gpio_config_t input_config = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT)
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));
}

void app_main(void)
{
    /* display first, same order as session 06 */
    spi_init();
    gpio_init();
    lcd_init();
    ESP_ERROR_CHECK(gpio_set_level(PIN_BK_LIGHT, 1));
    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOUR_BLACK);

    /* now the touch controller, on its own bus */
    i2c_master_dev_handle_t touch = touch_init();
    touch_log_ids(touch);

    ESP_LOGI(TAG, "display ready, waiting for touch");

    while (true) {
        uint8_t point_count = 0;

        /* plain warn-and-continue rather than ESP_ERROR_CHECK, we don't want
         * a single bad read taking the board down */
        esp_err_t err = touch_read_point_count(touch, &point_count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TD_STATUS read failed: %s", esp_err_to_name(err));
        } else if (point_count > 0U) {
            uint16_t raw_x = 0;
            uint16_t raw_y = 0;

            err = touch_read_point(touch, &raw_x, &raw_y);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "coordinate read failed: %s", esp_err_to_name(err));
            } else {
                uint16_t x = 0;
                uint16_t y = 0;

                touch_to_screen(raw_x, raw_y, &x, &y);
                draw_touch_square(x, y);
                ESP_LOGI(TAG, "touch @ x=%u y=%u", x, y);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}
