idf_component_register(SRCS "main.c" "ui/ui.c" "lcd/lcd.c"
                            "lvgl_port/lvgl_display.c" "lvgl_port/st7796_panel.c" "lvgl_app/lvgl_app.c"
                    INCLUDE_DIRS "." "ui" "lcd" "lvgl_port" "lvgl_app"
                    REQUIRES esp_lcd driver esp_timer)