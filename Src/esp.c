#include "esp.h"
#include <string.h>
#include <stdio.h>

static esp_idle_tick_fn idle_tick_fn = 0;

void esp_set_idle_tick(esp_idle_tick_fn fn) {
    idle_tick_fn = fn;
}

// Calls the idle hook about every 25 ms of tim_port's 1 MHz count.
static void esp_service_idle(TIM_TypeDef *tim_port, uint32_t *next_idle_us) {
    if (!idle_tick_fn) return;
    if (tim_port->CNT < *next_idle_us) return;
    *next_idle_us += 25000U;
    idle_tick_fn();
}

// Constant-cost tail match; a strstr per byte overruns the UART at 115200.
static uint8_t esp_ends_with(const char *buf, size_t len, const char *pat) {
    size_t m = strlen(pat);
    if (len < m) return 0;
    return memcmp(buf + len - m, pat, m) == 0;
}

// Appends c, sliding out the older half when full so a long reply keeps its tail.
static void esp_buf_push(char *buf, size_t size, uint8_t *index, char c) {
    if (*index >= size - 1) {
        size_t keep = size / 2;
        memmove(buf, buf + (*index - keep), keep);
        *index = (uint8_t)keep;
    }
    buf[(*index)++] = c;
    buf[*index] = '\0';
}

void esp_send_command(USART_TypeDef *port, const char *cmd) {
    // drop leftover bytes from the previous reply
    while (UART_DataAvailable(port)) UART_ReceiveChar(port);
    UART_SendString(port, cmd);
    UART_SendString(port, "\r\n");
}

uint8_t esp_wait_response(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *expected, uint32_t timeout_ms) {
    char buffer[128] = {0};
    uint8_t index = 0;
    uint32_t next_idle_us = 25000U;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            esp_buf_push(buffer, sizeof(buffer), &index, c);

            if (esp_ends_with(buffer, index, expected)) {
                tim_port->CR1 &= ~(0b1U);
                return 1; // found it
            }
        }

        esp_service_idle(tim_port, &next_idle_us);

        if (tim_port->CNT >= timeout_ms * 1000U) { // convert ms to us, since timer is 1MHz
            tim_port->CR1 &= ~(0b1U);
            return 0; // timed out
        }
    }
}

// Soft reset; recovers an ESP stuck mid-command.
uint8_t esp_reset(USART_TypeDef *port, TIM_TypeDef *tim_port) {
    esp_send_command(port, "AT+RST");
    uint8_t ok = esp_wait_response(port, tim_port, "ready", 8000);
    TIM_Delay_ms(tim_port, 500);
    while (UART_DataAvailable(port)) UART_ReceiveChar(port);
    return ok;
}

static uint8_t esp_capture_until(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *terminator, char *buf, size_t buf_size, uint32_t timeout_ms);

// True if the ESP is still joined to an AP.
uint8_t esp_wifi_associated(USART_TypeDef *port, TIM_TypeDef *tim_port) {
    char buf[200];
    esp_send_command(port, "AT+CWJAP?");
    if (!esp_capture_until(port, tim_port, "OK", buf, sizeof(buf), ESP_TIMEOUT_MS)) return 0;
    return strstr(buf, "+CWJAP:") != NULL;
}

// Waits for CWJAP's OK or FAIL; skips the echo, which contains the password.
static uint8_t esp_wait_join(USART_TypeDef *port, TIM_TypeDef *tim_port, uint32_t timeout_ms) {
    char buffer[160] = {0};
    uint8_t index = 0;
    uint8_t seen_lf = 0;
    uint32_t next_idle_us = 25000U;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U);
    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            esp_buf_push(buffer, sizeof(buffer), &index, c);
            if (c == '\n') seen_lf = 1;

            if (seen_lf && (esp_ends_with(buffer, index, "OK") || esp_ends_with(buffer, index, "FAIL"))) {
                tim_port->CR1 &= ~(0b1U);
                return esp_ends_with(buffer, index, "OK");
            }
        }
        esp_service_idle(tim_port, &next_idle_us);
        if (tim_port->CNT >= timeout_ms * 1000U) {
            tim_port->CR1 &= ~(0b1U);
            return 0;
        }
    }
}

uint8_t esp_connect_wifi(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *ssid, const char *password) {
    esp_send_command(port, "AT+CWMODE=1"); // station mode
    if (!esp_wait_response(port, tim_port, "OK", ESP_TIMEOUT_MS)) {
        return 0;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);

    esp_send_command(port, cmd);
    return esp_wait_join(port, tim_port, ESP_WIFI_TIMEOUT_MS);
}

// Like esp_wait_response, but also copies the reply into buf.
static uint8_t esp_capture_until(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *terminator, char *buf, size_t buf_size, uint32_t timeout_ms) {
    size_t index = 0;
    char tail[8] = {0}; // last bytes seen, so the terminator matches past a full buf
    size_t tlen = strlen(terminator);
    uint32_t next_idle_us = 25000U;
    buf[0] = '\0';

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U);

    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            if (index < buf_size - 1) {
                buf[index++] = c;
                buf[index] = '\0';
            }
            memmove(tail, tail + 1, sizeof(tail) - 1);
            tail[sizeof(tail) - 1] = c;
            if (tlen <= sizeof(tail) && memcmp(tail + sizeof(tail) - tlen, terminator, tlen) == 0) {
                tim_port->CR1 &= ~(0b1U);
                return 1;
            }
        }
        esp_service_idle(tim_port, &next_idle_us);
        if (tim_port->CNT >= timeout_ms * 1000U) {
            tim_port->CR1 &= ~(0b1U);
            return 0;
        }
    }
}

// Closes any open socket; returns on OK or ERROR, 300 ms at most.
static void esp_close_connection(USART_TypeDef *port, TIM_TypeDef *tim_port) {
    char buf[32] = {0};
    uint8_t index = 0;
    uint32_t next_idle_us = 25000U;

    esp_send_command(port, "AT+CIPCLOSE");

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U);
    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            esp_buf_push(buf, sizeof(buf), &index, c);
            if (esp_ends_with(buf, index, "OK") || esp_ends_with(buf, index, "ERROR")) break;
        }
        esp_service_idle(tim_port, &next_idle_us);
        if (tim_port->CNT >= 300U * 1000U) break;
    }
    tim_port->CR1 &= ~(0b1U);
}

// Waits for CIPSTART's OK; fails fast on ERROR instead of waiting out the timeout.
static uint8_t esp_wait_connect_result(USART_TypeDef *port, TIM_TypeDef *tim_port, uint32_t timeout_ms) {
    char buffer[160] = {0}; // holds the echoed command plus CONNECT and OK
    uint8_t index = 0;
    uint32_t next_idle_us = 25000U;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U);
    while (1) {
        if (UART_DataAvailable(port)) {
            char c = UART_ReceiveChar(port);
            esp_buf_push(buffer, sizeof(buffer), &index, c);
            if (esp_ends_with(buffer, index, "OK")) {
                tim_port->CR1 &= ~(0b1U);
                return 1;
            }
            if (esp_ends_with(buffer, index, "ERROR")) {
                tim_port->CR1 &= ~(0b1U);
                return 0;
            }
        }
        esp_service_idle(tim_port, &next_idle_us);
        if (tim_port->CNT >= timeout_ms * 1000U) {
            tim_port->CR1 &= ~(0b1U);
            return 0;
        }
    }
}

// HEAD request; captures the response headers until the server closes.
static uint8_t esp_http_head(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, const char *path, char *buf, size_t buf_size) {
    char cmd[64];
    char request[128];

    int request_len = snprintf(request, sizeof(request),
        "HEAD %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    // a new host can fail the first connect, so retry with a pause
    uint8_t connected = 0;
    for (uint8_t attempt = 0; attempt < 5 && !connected; attempt++) {
        if (attempt > 0) TIM_Delay_ms(tim_port, 1000);
        esp_close_connection(port, tim_port);
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
        esp_send_command(port, cmd);
        connected = esp_wait_connect_result(port, tim_port, ESP_CONNECT_TIMEOUT_MS);
    }
    if (!connected) {
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", request_len);
    esp_send_command(port, cmd);
    if (!esp_wait_response(port, tim_port, ">", ESP_TIMEOUT_MS)) {
        return 0;
    }

    UART_SendString(port, request);

    // wait for CLOSED; OK would match inside SEND OK before the response arrives
    esp_capture_until(port, tim_port, "CLOSED", buf, buf_size, ESP_TIMEOUT_MS);
    return buf[0] != '\0';
}

static uint8_t month_from_abbr(const char *abbr) {
    static const char *names[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    for (uint8_t i = 0; i < 12; i++) {
        if (abbr[0] == names[i][0] && abbr[1] == names[i][1] && abbr[2] == names[i][2]) return i + 1;
    }
    return 0;
}

// RFC 1123 Date header: "Www, dd Mmm yyyy HH:MM:SS GMT"
static uint8_t parse_http_date(const char *s, RTC_Time *out) {
    s += 5; // skip "Www, "

    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return 0;
    uint8_t date = (uint8_t)((s[0]-'0')*10 + (s[1]-'0'));
    s += 3; // skip "dd "

    uint8_t month = month_from_abbr(s);
    if (month == 0) return 0;
    s += 4; // skip "Mmm "

    uint16_t year = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        year = (uint16_t)(year * 10 + (s[i] - '0'));
    }
    if (year < 2020) return 0;
    s += 5; // skip "yyyy "

    if (!(s[0]>='0'&&s[0]<='9') || !(s[1]>='0'&&s[1]<='9') || s[2] != ':') return 0;
    uint8_t hours = (uint8_t)((s[0]-'0')*10 + (s[1]-'0'));
    s += 3;
    if (!(s[0]>='0'&&s[0]<='9') || !(s[1]>='0'&&s[1]<='9') || s[2] != ':') return 0;
    uint8_t minutes = (uint8_t)((s[0]-'0')*10 + (s[1]-'0'));
    s += 3;
    if (!(s[0]>='0'&&s[0]<='9') || !(s[1]>='0'&&s[1]<='9')) return 0;
    uint8_t seconds = (uint8_t)((s[0]-'0')*10 + (s[1]-'0'));

    out->seconds = seconds;
    out->minutes = minutes;
    out->hours   = hours;
    out->day     = 1; // unused, see RTC_Time.day
    out->date    = date;
    out->month   = month;
    out->year    = (uint8_t)(year % 100);
    return 1;
}

// Signed integer, e.g. -4 (X-Tz-Offset).
static uint8_t parse_int8(const char *s, int8_t *out) {
    uint8_t negative = 0;
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') { s++; }
    if (*s < '0' || *s > '9') return 0;

    int16_t value = 0;
    while (*s >= '0' && *s <= '9') { value = (int16_t)(value * 10 + (*s - '0')); s++; }
    *out = (int8_t)(negative ? -value : value);
    return 1;
}

// Signed decimal, e.g. -15.0 (X-Alarm-Threshold).
static uint8_t parse_float(const char *s, float *out) {
    uint8_t negative = 0;
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') { s++; }
    if ((*s < '0' || *s > '9') && *s != '.') return 0;

    float value = 0.0f;
    while (*s >= '0' && *s <= '9') { value = value * 10.0f + (float)(*s - '0'); s++; }
    if (*s == '.') {
        s++;
        float scale = 0.1f;
        while (*s >= '0' && *s <= '9') { value += (float)(*s - '0') * scale; scale *= 0.1f; s++; }
    }
    *out = negative ? -value : value;
    return 1;
}

// Unsigned integer, e.g. 26100913 (X-Last-Hour).
static uint8_t parse_u32(const char *s, uint32_t *out) {
    if (*s < '0' || *s > '9') return 0;
    uint32_t value = 0;
    while (*s >= '0' && *s <= '9') { value = value * 10U + (uint32_t)(*s - '0'); s++; }
    *out = value;
    return 1;
}

// Value of the header `name` (e.g. "X-Alarm-Threshold: "), or NULL if it is missing
// or the capture was cut off before the end of its line. A half-read number would
// otherwise parse as a different, wrong one (a threshold of -1 for -15.0).
static const char *header_value(const char *buf, const char *name) {
    const char *tag = strstr(buf, name);
    if (!tag) return NULL;
    const char *value = tag + strlen(name);
    return strchr(value, '\r') ? value : NULL;
}

// Time sync from the server's Date header; also reads its X-* config headers.
uint8_t esp_sync_time(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, RTC_Time *time, int8_t *tz_offset_hours, float *alarm_threshold_c, uint32_t *alarm_delay_min, uint32_t *server_last_key) {
    char buf[512];
    if (!esp_http_head(port, tim_port, host, "/api/data", buf, sizeof(buf))) {
        return 0;
    }

    const char *date_tag = strstr(buf, "Date: ");
    if (!date_tag || !parse_http_date(date_tag + 6, time)) {
        return 0;
    }

    const char *v;
    if ((v = header_value(buf, "X-Tz-Offset: ")))       parse_int8(v, tz_offset_hours);
    if ((v = header_value(buf, "X-Alarm-Threshold: "))) parse_float(v, alarm_threshold_c);
    if ((v = header_value(buf, "X-Alarm-Delay: ")))     parse_u32(v, alarm_delay_min);
    if ((v = header_value(buf, "X-Last-Hour: ")))       parse_u32(v, server_last_key);

    return 1;
}

// POSTs body as JSON over a fresh TCP connection.
static uint8_t esp_http_post_json(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, const char *path, const char *body) {
    char cmd[64];
    char request[384];

    int request_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, (unsigned)strlen(body), body);

    esp_close_connection(port, tim_port);
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
    esp_send_command(port, cmd);
    if (!esp_wait_connect_result(port, tim_port, ESP_CONNECT_TIMEOUT_MS)) {
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", request_len);
    esp_send_command(port, cmd);
    if (!esp_wait_response(port, tim_port, ">", ESP_TIMEOUT_MS)) {
        return 0;
    }

    UART_SendString(port, request); // already ends with its own CRLF

    if (!esp_wait_response(port, tim_port, "SEND OK", ESP_TIMEOUT_MS)) {
        return 0;
    }

    // drain the response so a back-to-back request doesn't read its tail
    esp_wait_response(port, tim_port, "CLOSED", 2000);
    return 1;
}

uint8_t esp_send_reading(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, float temp, uint8_t door, uint32_t timestamp) {
    char formattedTemp[16];
    char body[128];

    format_float(formattedTemp, sizeof(formattedTemp), temp, 2);
    snprintf(body, sizeof(body), "{\"temp\":%s,\"door\":%u,\"timestamp\":%lu}",
            formattedTemp, door, (unsigned long)timestamp);

    return esp_http_post_json(port, tim_port, host, "/api/reading", body);
}

// One finished hour; temperatures go as integer hundredths of a degree.
uint8_t esp_send_hourly(USART_TypeDef *port, TIM_TypeDef *tim_port, const char *host, uint32_t key, int16_t min_x100, int16_t max_x100, int16_t avg_x100, uint16_t door_samples, uint16_t samples) {
    char body[128];

    snprintf(body, sizeof(body), "{\"key\":%lu,\"min_x100\":%d,\"max_x100\":%d,\"avg_x100\":%d,\"door\":%u,\"n\":%u}",
            (unsigned long)key, min_x100, max_x100, avg_x100, door_samples, samples);

    return esp_http_post_json(port, tim_port, host, "/api/hourly", body);
}

// Formats a float as a decimal string
int format_float(char *buf, size_t buf_size, float val, uint8_t decimals) {
    if (decimals > 6) decimals = 6; // float carries about 7 digits anyway

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
    for (int8_t i = decimals - 1; i >= 0; i--) {
        frac_str[i] = (char)('0' + (frac_part % 10));
        frac_part /= 10;
    }
    frac_str[decimals] = '\0';

    // Return final string
    return snprintf(buf, buf_size, "%s%lu.%s", negative ? "-" : "", (unsigned long)int_part, frac_str);
}
