#include "esp.h"
#include <string.h>
#include <stdio.h>

void esp_send_command(USART_TypeDef *port, const char *cmd) {
    UART_SendString(port, cmd);
    UART_SendString(port, "\r\n");
}

uint8_t esp_wait_response(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *expected, uint32_t timeout_ms) {
    char buffer[128] = {0};
    uint8_t index = 0;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            if (index < sizeof(buffer) - 1) { // leave room for the null terminator
                buffer[index++] = c;
                buffer[index] = '\0'; // keep the buffer a valid string after every char
            }

            if (strstr(buffer, expected) != NULL) {
                tim_port->CR1 &= ~(0b1U);
                return 1; // found it
            }
        }

        if (tim_port->CNT >= timeout_ms * 1000U) { // convert ms to us, since timer is 1MHz
            tim_port->CR1 &= ~(0b1U);
            return 0; // timed out
        }
    }
}

uint8_t esp_connect_wifi(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *ssid, const char *password) {
    esp_send_command(port, "AT+CWMODE=1"); // Set station mode
    if (!esp_wait_response(port, tim_port, "OK", ESP_TIMEOUT_MS)) {
        return 0; // failed to set station mode
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password); // Format string for ESP

    esp_send_command(port, cmd);
    return esp_wait_response(port, tim_port, "OK", ESP_WIFI_TIMEOUT_MS);
}

uint8_t esp_send_reading(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, float temp, uint8_t door, uint32_t timestamp) {
    char cmd[64];
    char body[128];
    char request[384];

    char formattedTemp[16];

    format_float(formattedTemp, sizeof(formattedTemp), temp, 2);

    // Build the JSON body first, since we need its length for the HTTP header
    snprintf(body, sizeof(body), "{\"temp\":%s,\"door\":%u,\"timestamp\":%lu}",
            formattedTemp, door, (unsigned long)timestamp);

    // Build the full HTTP request text, embedding the body and its length
    int request_len = snprintf(request, sizeof(request),
        "POST /api/reading HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        host, (unsigned)strlen(body), body);

    // Open the TCP connection
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
    esp_send_command(port, cmd);
    if (!esp_wait_response(port, tim_port, "OK", ESP_TIMEOUT_MS)) {
        return 0; // connection failed
    }

    // Tell the ESP how many bytes are coming
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", request_len);
    esp_send_command(port, cmd);
    if (!esp_wait_response(port, tim_port, ">", ESP_TIMEOUT_MS)) {
        return 0; // ESP didn't give the send prompt
    }

    // Send the actual HTTP request (no extra \r\n appended here -
    // esp_send_command() would add one, and the request already ends correctly)
    UART_SendString(port, request);

    return esp_wait_response(port, tim_port, "SEND OK", ESP_TIMEOUT_MS);
}

// Formats a float as a decimal string
int format_float(char *buf, size_t buf_size, float val, uint8_t decimals) {

    // Handle sign
    uint8_t negative = (val < 0.0f);
    if (negative) val = -val;

    // Scale into int
    float scale = 1.0f;
    for (uint8_t i = 0; i < decimals; i++) scale *= 10.0f;

    uint32_t scaled = (uint32_t)(val * scale + 0.5f); // round-half up

    // Split into whole and fractional parts
    uint32_t divisor  = (uint32_t)scale; // 100 for decimals=2
    uint32_t int_part  = scaled / divisor;
    uint32_t frac_part = scaled % divisor;

    // Add leading zeros to frac_part
    char frac_str[8];
    if (decimals >= sizeof(frac_str)) decimals = sizeof(frac_str) - 1; // safety
    for (int8_t i = decimals - 1; i >= 0; i--) {
        frac_str[i] = (char)('0' + (frac_part % 10));
        frac_part /= 10;
    }
    frac_str[decimals] = '\0';

    // Return final string
    return snprintf(buf, buf_size, "%s%lu.%s", negative ? "-" : "", (unsigned long)int_part, frac_str);
}
