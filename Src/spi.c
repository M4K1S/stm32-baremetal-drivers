#include "spi.h"

#include "clock.h"

#define SPI_TIMEOUT_US 5000U // 5ms timeout for SPI

void SPI_Init(SPI_TypeDef *port, SPI_Prescaler prescaler, TIM_TypeDef *tim_port) {
    if (port == SPI1) {
        RCC->APB2ENR |= (0b1U << 12); // Start SPI1 Clock
        RCC->AHB1ENR |= (0b1U); // Start GPIO A clock

        GPIOA->MODER &= ~((0b11U << 10) | (0b11U << 12) | (0b11U << 14)); // Clear PA5, PA6, PA7
        GPIOA->MODER |= ((0b10U << 10) | (0b10U << 12) | (0b10U << 14)); // Set PA5, PA6, PA7 to AF

        GPIOA->AFR[0] &= ~((0b1111U << 20) | (0b1111U << 24) | (0b1111U << 28)); // Clear AF for PA5, PA6, PA7
        GPIOA->AFR[0] |= (0b0101U << 20) | (0b0101U << 24) | (0b0101U << 28);    // Set AF5
    }

    port->CR1 &= ~(0b1U << 6); // SPE off: the prescaler can only change while disabled

    // master, mode 0, MSB first, 8-bit; SSM=1/SSI=1 since CS is a plain GPIO
    port->CR1 = (0b1U << 2) | (prescaler << 3) | (0b1U << 8) | (0b1U << 9);
    // Enable SPI
    port->CR1 |= (0b1U << 6);

    TIM_Init(tim_port, TIM_1MHZ_PRESCALER, 65535); // 1MHz tick for SPI timeouts
}

uint8_t SPI_TransferByte(SPI_TypeDef *port, TIM_TypeDef *tim_port, uint8_t byte_in, uint8_t *byte_out) {
    tim_port->CNT = 0;
    tim_port->CR1 |= (0b1U); // Start the timeout clock

    while (!(port->SR & (0b1U << 1))) { // Wait for TXE
        if (tim_port->CNT >= SPI_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U);
            return 0; // Timed out
        }
    }
    port->DR = byte_in;

    tim_port->CNT = 0; // reset for the second wait
    while (!(port->SR & (0b1U << 0))) { // Wait for RXNE
        if (tim_port->CNT >= SPI_TIMEOUT_US) {
            tim_port->CR1 &= ~(0b1U);
            return 0; // Timed out
        }
    }

    tim_port->CR1 &= ~(0b1U); // Stop the timeout clock
    *byte_out = port->DR;
    return 1; // Success
}
