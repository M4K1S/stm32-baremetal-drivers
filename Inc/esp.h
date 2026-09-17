#ifndef INC_ESP_H_
#define INC_ESP_H_

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "uart.h"
#include "tim.h"

// How long to wait for a plain AT/OK-style response
#define ESP_TIMEOUT_MS      5000
// AT+CWJAP can take much longer than a normal command
#define ESP_WIFI_TIMEOUT_MS 15000

void esp_send_command(USART_TypeDef *port, const char *cmd);
uint8_t esp_wait_response(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *expected, uint32_t timeout_ms);
uint8_t esp_connect_wifi(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *ssid, const char *password);
uint8_t esp_send_reading(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, float temp, uint8_t door, uint32_t timestamp);
int format_float(char *buf, size_t buf_size, float val, uint8_t decimals);

#endif
