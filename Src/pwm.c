#include "pwm.h"
#include "clock.h"

static const uint32_t fCK = FCK_HZ; // Clock speed of TIM

void PWM_Init(TIM_TypeDef *port, uint8_t channel, uint32_t frequency, uint8_t dutyCycle) {

    if (frequency < 1 || frequency > fCK / 1000U) return; // PSC must stay >= 0
    if (dutyCycle > 100) dutyCycle = 100;

    if(port == TIM2) {
        RCC->APB1ENR |= (0b1); // Start TIM2 clock
        if (channel == 2) {
            RCC->AHB1ENR |= (0b1U << 0); // Start GPIOA Clock
            GPIOA->MODER &= ~(0b11U << 2); // Clear PA1
            GPIOA->MODER |= (0b10U << 2); // Set PA1to AF
            GPIOA->AFR[0] &= ~(0b1111U << 4); // Clear AF
            GPIOA->AFR[0] |= (0b0001U << 4); // Set AF1
        }
    }

    port->ARR = (1000U - 1U); // Set auto reload to 1000
    port->PSC = (fCK / (1000U * frequency)) - 1U; // 1000 steps per period

    if (channel == 2) {
        port->CCMR1 &= ~(0b1111U << 11); // Clear OC2M & OC2PE bits
        port->CCMR1 |= 0b1101U << 11; // PWM mode 1, preload on

        port->CCR2 = dutyCycle * 10; // 0-100 % scaled to ARR = 1000
    }

    port->EGR |= (0b1U); // load PSC/ARR/CCR now instead of at the first overflow
}

void PWM_Start(TIM_TypeDef *port, uint8_t channel) {
    if (channel == 2) {
        port->CCER |= (0b1U << 4); // Enable CC2R for channel 2 output
    }

    port->CR1 |= (0b1U); // Start the counter


}

void PWM_Stop(TIM_TypeDef *port, uint8_t channel) {
    if (channel == 2) {
        port->CCER &= ~(0b1U << 4); // Clear CC2R disable channel 2 output
    }
}

void PWM_SetDuty(TIM_TypeDef *port, uint8_t channel, uint8_t dutyCycle) {
    if (dutyCycle > 100) dutyCycle = 100;
    if (channel == 2) port->CCR2 = dutyCycle * 10;
}

