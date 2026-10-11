#ifndef INC_CLOCK_H_
#define INC_CLOCK_H_

// Peripheral clock used for the TIM/UART/I2C/SPI/PWM calculations
#define FCK_HZ 16000000U

// Prescaler that makes a timer count in microseconds
#define TIM_1MHZ_PRESCALER (FCK_HZ / 1000000U)

#endif
