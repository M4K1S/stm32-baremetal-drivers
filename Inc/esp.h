#ifndef INC_ESP_H_
#define INC_ESP_H_

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "uart.h"
#include "tim.h"
#include "ds3231.h"

#define ESP_TIMEOUT_MS         5000  // plain AT reply
#define ESP_WIFI_TIMEOUT_MS    15000 // AT+CWJAP
#define ESP_CONNECT_TIMEOUT_MS 10000 // AT+CIPSTART (DNS + TCP)

// Hook called about every 25 ms from the blocking waits in esp.c; NULL disables it.
// It must not call esp_* functions or touch the timer passed to them.
typedef void (*esp_idle_tick_fn)(void);
void esp_set_idle_tick(esp_idle_tick_fn fn);

void esp_send_command(USART_TypeDef *port, const char *cmd);
uint8_t esp_wait_response(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *expected, uint32_t timeout_ms);
uint8_t esp_wifi_associated(USART_TypeDef *port, TIM_TypeDef *tim_port);
uint8_t esp_reset(USART_TypeDef *port, TIM_TypeDef *tim_port);
uint8_t esp_connect_wifi(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *ssid, const char *password);
uint8_t esp_send_reading(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, float temp, uint8_t door, uint32_t timestamp);
uint8_t esp_sync_time(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, RTC_Time *time, int8_t *tz_offset_hours, float *alarm_threshold_c, uint32_t *alarm_delay_min, uint32_t *server_last_key);
uint8_t esp_send_hourly(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, uint32_t key, int16_t min_x100, int16_t max_x100, int16_t avg_x100, uint16_t door_samples, uint16_t samples);
int format_float(char *buf, size_t buf_size, float val, uint8_t decimals);

#endif
