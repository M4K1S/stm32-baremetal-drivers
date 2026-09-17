#ifndef OLED_FONTS_H
#define OLED_FONTS_H

#include <stdint.h>

// 14x11 icons
extern const uint8_t OLED_Icon_Wifi[28];
extern const uint8_t OLED_Icon_SD[28];
extern const uint8_t OLED_Icon_Refresh[28];

// 8x11 digits
extern const uint8_t OLED_SmallDigits[10][11];

// 8x11 colon
extern const uint8_t OLED_SmallColon[11];

// 11x16 digits and minus
extern const uint8_t OLED_BigDigits[10][32];
extern const uint8_t OLED_BigMinus[6];

#define DEGREE_SIZE 6
extern const uint8_t OLED_Degree[DEGREE_SIZE][DEGREE_SIZE];

#endif
