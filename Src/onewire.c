#include "onewire.h"
#include "clock.h"

// Interrupts off for one bit slot (~65 µs); returns the old state for ow_irq_restore
static inline uint32_t ow_irq_save(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void ow_irq_restore(uint32_t primask) {
    __set_PRIMASK(primask);
}

void ow_init(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    GPIO_Init(gpio_port, pin, OUTPUT, OPEN_DRAIN); // open-drain setup
    TIM_Init(tim_port, TIM_1MHZ_PRESCALER, 65535); // 1MHz tick
    GPIO_Set(gpio_port, pin, 1);  // release the line, let it idle HIGH
}

uint8_t ow_reset(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port) {
    // Hold the line low for 480us (minimum reset pulse)
    GPIO_SetMode(gpio_port, pin, OUTPUT);
    GPIO_Set(gpio_port, pin, 0);
    TIM_Delay_us(tim_port, 480);

    // Release the line and let the pull-up bring it high
    GPIO_SetMode(gpio_port, pin, INPUT);

    // Wait 100us then sample.
    TIM_Delay_us(tim_port, 100);
    uint8_t presence = !GPIO_Read(gpio_port, pin); // LOW = presence detected, so invert

    // finish tRSTH (480 us total) so the bus is idle
    TIM_Delay_us(tim_port, 380);

    return presence;
}

void ow_write_bit(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port, uint8_t bit) {
    uint32_t primask = ow_irq_save();
    if (bit == 1) {
        // Pull the line low for 5us
        GPIO_SetMode(gpio_port, pin, OUTPUT);
        GPIO_Set(gpio_port, pin, 0);
        TIM_Delay_us(tim_port, 5);
        // Release the line for remainder (60us total)
        GPIO_Set(gpio_port, pin, 1);
        TIM_Delay_us(tim_port, 55);
    } else {
        // Pull the line low for 60us
        GPIO_SetMode(gpio_port, pin, OUTPUT);
        GPIO_Set(gpio_port, pin, 0);
        TIM_Delay_us(tim_port, 60);
        // Release bus
        GPIO_Set(gpio_port, pin, 1);
    }

    // Recovery time before next slot
    TIM_Delay_us(tim_port, 2);
    ow_irq_restore(primask);
}

uint8_t ow_read_bit(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port){
    uint32_t primask = ow_irq_save();
    // Pull the line low for at least 1us to initialize read
    GPIO_SetMode(gpio_port, pin, OUTPUT);
    GPIO_Set(gpio_port, pin, 0);
    TIM_Delay_us(tim_port, 2);
    // Release the line
    GPIO_SetMode(gpio_port, pin, INPUT);
    // Wait 5us then sample
    TIM_Delay_us(tim_port, 5);
    uint8_t read_bit = GPIO_Read(gpio_port, pin);
    // Wait remainder of slot (60us + recovery time)
    TIM_Delay_us(tim_port, 55);
    ow_irq_restore(primask);
    return read_bit;
}

void ow_write_byte(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port, uint8_t byte) {
    // Repeat for 8 bits
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t bit = (byte >> i) & 0b1U; // Get LSB
        ow_write_bit(gpio_port, pin, tim_port, bit); // Write bit to DS18B20
    }
}

uint8_t ow_read_byte(GPIO_TypeDef *gpio_port, uint8_t pin, TIM_TypeDef *tim_port){
    uint8_t byte = 0;
    // Repeat for 8 bits
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t bit = ow_read_bit(gpio_port, pin, tim_port); // Read byte
        byte |= bit << i; // Put bit into byte LSB
    }
    return byte;
}
