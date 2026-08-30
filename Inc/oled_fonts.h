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

// 24x36 digits
extern const uint8_t OLED_BigDigits[10][108];

// 24x36 symbols
extern const uint8_t OLED_BigC[108];
extern const uint8_t OLED_BigMinus[108];
extern const uint8_t OLED_BigDegree[108];
extern const uint8_t OLED_BigPeriod[108];

#endif
