#ifndef INC_SSD1306_H_
#define INC_SSD1306_H_

#include <stdint.h>
#include "stm32f4xx.h"
#include "i2c.h"
#include "tim.h"
#include "oled_fonts.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_FB_SIZE ((OLED_WIDTH * OLED_HEIGHT) / 8)

#define SSD1306_ADDR      0x3C
#define SSD1306_CMD_MODE  0x00
#define SSD1306_DATA_MODE 0x40

uint8_t ssd1306_write_command(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t cmd);
uint8_t ssd1306_write_data(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t *data, uint8_t len);
uint8_t ssd1306_init(I2C_TypeDef *port, TIM_TypeDef *tim_port);

void oled_set_pixel(int x, int y, uint8_t state);
uint8_t oled_get_pixel(int x, int y);
void oled_clear(void);
uint8_t oled_update(I2C_TypeDef *port, TIM_TypeDef *tim_port);

void oled_draw_icon(int x, int y, const uint8_t *icon);
void oled_draw_small_time(int x, int y, uint8_t hh, uint8_t mm);
void oled_draw_status_screen(uint8_t wifi_connected, uint8_t sd_ready,
                              uint8_t have_refresh, uint8_t refresh_hh, uint8_t refresh_mm,
                              uint8_t have_temp, int32_t temp_deci,
                              uint8_t muted, uint8_t mute_hh, uint8_t mute_mm);

#endif
