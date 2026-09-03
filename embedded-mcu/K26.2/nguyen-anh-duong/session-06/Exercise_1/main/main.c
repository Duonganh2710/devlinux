//#include <corecrt_search.h>
#include <stdio.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "hal/gpio_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "images/image1.c"

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
#define LCD_NO_PARAMETER (-1)

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

/* Startup-bar heights: three near-equal thirds of the 320-row panel. */
#define BAR_H_RED      (LCD_V_RES / 3U)                /* 106 */
#define BAR_H_GREEN    ((LCD_V_RES - BAR_H_RED) / 2U)  /* 107 */
#define BAR_H_BLUE     (LCD_V_RES - BAR_H_RED - BAR_H_GREEN) /* 107 */

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
        SPI_DMA_CH_AUTO     // Enable the DMA feature
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

void static gpio_init(void)
{
    gpio_config_t io_config = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << PIN_RS) |
                        (1ULL << PIN_RST) |
                        (1ULL << PIN_BK_LIGHT)
    };

    ESP_ERROR_CHECK(gpio_config(&io_config));
}

void lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &cmd
    };
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 0));
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handler, &transaction));
}

void lcd_write_data(const uint8_t *buf, size_t len)
{
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = buf
    };
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 1));
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handler, &transaction));
}

void lcd_reset(void)
{
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* Initialize the 3.5-inch IPS SPI display with the ST7796 controller. */
void lcd_init(void)
{
    /* Reset the controller, then leave sleep mode. */
    lcd_reset();
    lcd_write_cmd(CMD_SLPOUT);

    /* Allow the display oscillator and internal circuits to start. */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Memory access control: set display orientation and RGB/BGR order. */
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

    /* Power control 1: source and gate driver voltage settings. */
    const uint8_t pwr1[] = { 0xC0, 0x00 };
    lcd_write_cmd(CMD_PWR1);
    lcd_write_data(pwr1, sizeof(pwr1));

    /* Power control 2. */
    const uint8_t pwr2[] = { 0x13 };
    lcd_write_cmd(CMD_PWR2);
    lcd_write_data(pwr2, sizeof(pwr2));

    /* Power control 3. */
    const uint8_t pwr3[] = { 0xA7 };
    lcd_write_cmd(CMD_PWR3);
    lcd_write_data(pwr3, sizeof(pwr3));

    /* VCOM control. */
    const uint8_t vcom[] = { 0x21 };
    lcd_write_cmd(CMD_VCOM);
    lcd_write_data(vcom, sizeof(vcom));

    /* Display timing and frame-rate control. */
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

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
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

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
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


void lcd_show_image(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                   const uint16_t *image)
{
    const uint32_t pixel_count = (uint32_t)width * height;
    if (width == 0 || height == 0 || image == NULL) {
        return;
    }

    lcd_set_window(x, y, x + width - 1, y + height - 1);

    uint32_t pixels_sent = 0;
    while (pixels_sent < pixel_count) {
        uint32_t chunk_pixels = pixel_count - pixels_sent;
        if (chunk_pixels > LCD_FILL_CHUNK_PIXELS) {
            chunk_pixels = LCD_FILL_CHUNK_PIXELS;
        }

        lcd_write_data((const uint8_t *)&image[pixels_sent], chunk_pixels * 2);
        pixels_sent += chunk_pixels;
    }
}

void app_main(void)
{
    spi_init();
    gpio_init();
    lcd_init();
    ESP_ERROR_CHECK(gpio_set_level(PIN_BK_LIGHT, 1));

    /* Draw horizontal startup bars: red top, green middle, blue bottom. */
    lcd_fill_rect(0, 0, LCD_H_RES, BAR_H_RED, COLOUR_RED);
    lcd_fill_rect(0, BAR_H_RED, LCD_H_RES, BAR_H_GREEN, COLOUR_GREEN);
    lcd_fill_rect(0, BAR_H_RED + BAR_H_GREEN, LCD_H_RES, BAR_H_BLUE, COLOUR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    const uint16_t colours[] = {
        COLOUR_RED,   /* red */
        COLOUR_GREEN, /* green */
        COLOUR_BLUE,  /* blue */
        COLOUR_WHITE, /* white */
        COLOUR_BLACK  /* black */
    };

    while (1) {
        for (size_t index = 0; index < sizeof(colours) / sizeof(colours[0]); index++) {
            lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, colours[index]);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
