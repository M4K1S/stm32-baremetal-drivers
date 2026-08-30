#include "i2c.h"
#include "clock.h"

#define I2C_TIMEOUT_US 5000U // 5ms timeout for I2C

static const uint32_t fCK = FCK_HZ; // Clock speed of i2C
static const uint32_t fastCK = 400000U;
static const uint32_t standCK = 100000U;

void I2C_Init(I2C_TypeDef *port, I2C_SPEED speed, TIM_TypeDef *tim_port){
    if (port == I2C1) {
        RCC->APB1ENR |= (0b1U << 21); // Start I2C1 Clock
        RCC->AHB1ENR |= (0b1U << 1);  // Start GPIOB Clock

        GPIOB->MODER &= ~((0b11U << 16) | (0b11U << 18)); // Clear PB8 & PB9
        GPIOB->MODER |= (0b10U << 16) | (0b10U << 18);     // Set PB8 & PB9 to AF
        GPIOB->OTYPER |= (0b1U << 8) | (0b1U << 9); // Set PB8 & PB9 to open-drain
        GPIOB->PUPDR &= ~((0b11U << 16) | (0b11U << 18)); // Clear PB8 & PB9 pull config
        GPIOB->PUPDR |= (0b01U << 16) | (0b01U << 18);     // Set PB8 & PB9 to pull-up


        GPIOB->AFR[1] &= ~((0b1111U << 0) | (0b1111U << 4)); // Clear AF for PB8 (bits 0-3) & PB9 (bits 4-7)
        GPIOB->AFR[1] |= (0b0100U << 0) | (0b0100U << 4);    // Set AF4
    }

    if (port == I2C2) {
        RCC->APB1ENR |= (0b1U << 22); // Start I2C2 Clock
        RCC->AHB1ENR |= (0b1U << 1);  // Start GPIOB Clock

        GPIOB->MODER &= ~((0b11U << 20) | (0b11U << 22)); // Clear PB10 & PB11
        GPIOB->MODER |= (0b10U << 20) | (0b10U << 22);     // Set PB10 & PB11 to AF
        GPIOB->OTYPER |= (0b1U << 10) | (0b1U << 11); // Set PB10 & PB11 to open-drain
        GPIOB->PUPDR &= ~((0b11U << 20) | (0b11U << 22)); // Clear PB10 & PB11 pull config
        GPIOB->PUPDR |= (0b01U << 20) | (0b01U << 22);     // Set PB10 & PB11 to pull-up

        GPIOB->AFR[1] &= ~((0b1111U << 8) | (0b1111U << 12)); // Clear AF for PB10 (bits 8-11) & PB11 (bits 12-15)
        GPIOB->AFR[1] |= (0b0100U << 8) | (0b0100U << 12);    // Set AF4
    }

    if (port == I2C3) {
        RCC->APB1ENR |= (0b1U << 23); // Start I2C3 Clock
        RCC->AHB1ENR |= (0b1U << 0) | (0b1U << 2); // Start GPIOA & GPIOC Clocks

        GPIOA->MODER &= ~(0b11U << 16); // Clear PA8
        GPIOA->MODER |= (0b10U << 16);   // Set PA8 to AF
        GPIOA->OTYPER |= (0b1U << 8);    // Set PA8 to open-drain
        GPIOA->PUPDR &= ~(0b11U << 16); // Clear PA8 pull config
        GPIOA->PUPDR |= (0b01U << 16);   // Set PA8 to pull-up
        GPIOA->AFR[1] &= ~(0b1111U << 0); // Clear AF for PA8 (bits 0-3)
        GPIOA->AFR[1] |= (0b0100U << 0);  // Set AF4

        GPIOC->MODER &= ~(0b11U << 18); // Clear PC9
        GPIOC->MODER |= (0b10U << 18);   // Set PC9 to AF
        GPIOC->OTYPER |= (0b1U << 9);    // Set PC9 to open-drain
        GPIOC->PUPDR &= ~(0b11U << 18); // Clear PC9 pull config
        GPIOC->PUPDR |= (0b01U << 18);   // Set PC9 to pull-up
        GPIOC->AFR[1] &= ~(0b1111U << 4); // Clear AF for PC9 (bits 4-7)
        GPIOC->AFR[1] |= (0b0100U << 4);  // Set AF4
    }

    port->CR2 &= ~(0b111111U); // Clear FREQ bits (bits 5:0)
    port->CR2 |= (fCK / 1000000U); // Set FREQ to fCK in MHz

    if(speed == SPEED_FAST) {
        port->CCR = (0b1U << 15) | (fCK / (3 * fastCK)); // If FAST mode then set F/S bit to 1 and set CCR for fast mode
        port->TRISE = (fCK / 1000000U) * 3U / 10U + 1U; // TRISE (max rise time) = (FREQ_MHz * 300 / 1000) + 1
    }
    if(speed == SPEED_STANDARD) {
        port->CCR = (fCK / (2 * standCK)); // If STANDARD mode then set F/S bit to 0 and set CCR for standard mode
        port->TRISE = (fCK / 1000000U) + 1U; // TRISE (max rise time) FREQ_MHz + 1
    }

    port->CR1 |= (0b1U); // Set PE bit to 1 - enable I2C

    TIM_Init(tim_port, 16, 65535); // configure 1MHz tick rate for I2C timeouts
}

uint8_t I2C_Start(I2C_TypeDef *port, TIM_TypeDef *tim_port) {
    port->CR1 |= (0b1U << 8); // Set START bit

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (!(port->SR1 & (0b1U << 0))) { // Wait for SB flag
        if (tim_port->CNT >= I2C_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
            return 0; // Timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
    return 1; // Success
}

void I2C_Stop(I2C_TypeDef *port) {
    port->CR1 |= (0b1U << 9); // Set STOP bit
}

uint8_t I2C_SendAddress(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t address, I2C_RW rw) {
    port->DR = (address << 1) | rw;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (!(port->SR1 & (0b1U << 1))) { // Wait for ADDR flag
        if (tim_port->CNT >= I2C_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
            return 0; // Timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // Stop the timeout clock

    // Clear ADDR flag by reading SR1 and SR2
    uint32_t clear_addr = port->SR1;
    clear_addr = port->SR2;
    (void)clear_addr;

    return 1;
}

uint8_t I2C_WriteByte(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t byte) {
    port->DR = byte;

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (!(port->SR1 & (0b1U << 7))) { // Wait for TXE bit to be set
        if (tim_port->CNT >= I2C_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
            return 0; // Timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
    return 1; // Success
}

uint8_t I2C_ReadByte(I2C_TypeDef *port, TIM_TypeDef *tim_port, I2C_ACK ack, uint8_t *byte_out) {
    if (ack) {
        port->CR1 |= (0b1U << 10); // Set ACK bit
    } else {
        port->CR1 &= ~(0b1U << 10); // Clear ACK bit, last byte
    }

    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (!(port->SR1 & (0b1U << 6))) { // Wait for RXNE flag
        if (tim_port->CNT >= I2C_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
            return 0; // Timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // Stop the timeout clock

    *byte_out = port->DR; // Read the received byte
    return 1; // Success
}

uint8_t I2C_WriteReg(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t value) {
    // Start I2C
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Send device address
    if (!I2C_SendAddress(port, tim_port, dev_addr, WRITE)) {
        I2C_Stop(port);
        return 0;
    }
    // Send register address
    if (!I2C_WriteByte(port, tim_port, reg_addr)) {
        I2C_Stop(port);
        return 0;
    }
    // Write value to register
    if (!I2C_WriteByte(port, tim_port, value)) {
        I2C_Stop(port);
        return 0;
    }

    I2C_Stop(port);
    return 1;
}

uint8_t I2C_ReadReg(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *value_out) {
    // Start I2C
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Send device address
    if (!I2C_SendAddress(port, tim_port, dev_addr, WRITE)) {
        I2C_Stop(port);
        return 0;
    }
    // Send register address
    if (!I2C_WriteByte(port, tim_port, reg_addr)) {
        I2C_Stop(port);
        return 0;
    }
    // Start I2C again
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Read from register
    if (!I2C_SendAddress(port, tim_port, dev_addr, READ)) {
        I2C_Stop(port);
        return 0;
    }
    // Read byte with NACK - only reading one byte
    if (!I2C_ReadByte(port, tim_port, NACK, value_out)) {
        I2C_Stop(port);
        return 0;
    }

    I2C_Stop(port);
    return 1;
}

uint8_t I2C_WriteBytes(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len) {
    // Start I2C
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Send device address
    if (!I2C_SendAddress(port, tim_port, dev_addr, WRITE)) {
        I2C_Stop(port);
        return 0;
    }
    // Send register address
    if (!I2C_WriteByte(port, tim_port, reg_addr)) {
        I2C_Stop(port);
        return 0;
    }
    // Write data to register
    for (uint8_t i = 0; i < len; i++) {
        if (!I2C_WriteByte(port, tim_port, data[i])) {
            I2C_Stop(port);
            return 0;
        }
    }

    I2C_Stop(port);
    return 1;
}

uint8_t I2C_ReadBytes(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len) {
    // Start I2C
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Send device address
    if (!I2C_SendAddress(port, tim_port, dev_addr, WRITE)) {
        I2C_Stop(port);
        return 0;
    }
    // Send register address
    if (!I2C_WriteByte(port, tim_port, reg_addr)) {
        I2C_Stop(port);
        return 0;
    }
    // Start I2C again
    if (!I2C_Start(port, tim_port)) {
        I2C_Stop(port);
        return 0; // If I2C start times out return 0
    }
    // Switch to read mode
    if (!I2C_SendAddress(port, tim_port, dev_addr, READ)) {
        I2C_Stop(port);
        return 0;
    }
    // Read data into data array
    for (uint8_t i = 0; i < len; i++) {
        I2C_ACK ack = (i == len - 1) ? NACK : ACK; // Last byte - send NACK
        if (!I2C_ReadByte(port, tim_port, ack, &data[i])) {
            I2C_Stop(port);
            return 0;
        }
    }

    I2C_Stop(port);
    return 1;
}
