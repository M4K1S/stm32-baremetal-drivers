# STM32 Bare-Metal Drivers

Register-level (no HAL) peripheral drivers for the STM32F446, written from the reference manual (RM0390) with no ST HAL calls anywhere in the driver layer. Every function takes a peripheral typedef pointer (`GPIO_TypeDef *`, `TIM_TypeDef *`, etc.) plus plain parameters and does direct `PERIPH->REG |=/&= (mask)` bit manipulation.

Developed and tested on a Nucleo-F446RE, originally as the driver layer for a larger embedded project. Pulled out into its own repo so it can be reused across projects and versioned independently of any one application.

See [`notes.md`](notes.md) for the full register-level walkthrough of every driver — why each bit is set, the timing/protocol math behind each peripheral, and the datasheet/reference-manual values used.

## Board notes (Nucleo-F446RE)

- **Clock:** HSE via ST-LINK MCO must use `RCC_HSE_BYPASS`, not `RCC_HSE_ON` — there is no physical crystal on this board.
- **LD2 (onboard LED):** PA5, wired **active-low** (pin LOW = ON). Use `TIM_OCPOLARITY_LOW` if driving it via PWM.
- **PA1** doubles as **TIM2_CH2** — used by the PWM driver.
- System clock is assumed to be 16MHz (`FCK_HZ` in `clock.h`) — update that one define if your `SystemClock_Config()` differs.

## Drivers

### GPIO (`gpio.c/h`)
`GPIO_Init`, `GPIO_SetMode`, `GPIO_Set`, `GPIO_Toggle`, `GPIO_Read`. Input/Output/AltFunc/Analog modes, push-pull or open-drain output type (applied for both `OUTPUT` and `ALT_FUNC` modes, since alternate-function pins like I2C's SCL/SDA are still actively-driven outputs).
- **Limitation:** no `PUPDR` (pull-up/down) or `OSPEEDR` (speed) control — peripherals that need an internal pull-up (I2C) configure those registers by hand instead of going through `GPIO_Init`.

### UART (`uart.c/h`)
`UART_Init`, `UART_SendChar`, `UART_SendString`, `UART_ReceiveChar`, `UART_WaitForTC`, `UART_DataAvailable`. Supports USART1/2/3, UART4/5, USART6. 8N1 only, no flow control.
- **Limitation:** `UART_ReceiveChar` is **blocking** (spins on RXNE). Use `UART_DataAvailable()` first if you need a non-blocking check.

### TIM base (`tim.c/h`)
`TIM_Init` (explicit PSC/ARR), `TIM_Delay_us`, `TIM_Delay_ms`, `TIM_Start`, `TIM_Stop`, `TIM_Ready` (polls/clears UIF). Supports TIM2/3/4/5.
- **Limitation:** no interrupt-driven mode — `TIM_Ready()` requires polling.
- Give each subsystem that needs microsecond timing (OneWire, I2C, SPI) its own dedicated `TIM_TypeDef*` rather than sharing one timer across unrelated peripherals.

### PWM (`pwm.c/h`)
`PWM_Init`, `PWM_Start`, `PWM_SetDuty`, `PWM_Stop`. PWM Mode 1, preload enabled (`OCxPE`), 1000-count period (ARR=999).
- **Limitation: only TIM2 Channel 2 (PA1) is implemented.** `channel` param exists but all internal logic is gated on `channel == 2`.
- **Limitation:** `dutyCycle` is 0-100, not range-checked — values >100 overflow past ARR and pin the output at 100%.

### I2C (`i2c.c/h`)
`I2C_Init`, `I2C_Start`, `I2C_Stop`, `I2C_SendAddress`, `I2C_WriteByte`, `I2C_ReadByte`, `I2C_WriteReg`, `I2C_ReadReg`, `I2C_WriteBytes`, `I2C_ReadBytes`. I2C1 (PB8=SCL, PB9=SDA), Standard (100kHz) or Fast (400kHz) mode. Every blocking call is timeout-guarded via a caller-supplied `tim_port`.
- **Limitation:** I2C1 only wired up.

### SPI (`spi.c/h`)
`SPI_Init`, `SPI_TransferByte`. SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI), Mode 0, 8-bit frames, selectable prescaler (÷2-÷256). Timeout-guarded the same way as I2C.
- Sets `SSM`/`SSI` (`CR1` bits 9/8) so chip-select can be handled as a plain GPIO pin instead of the hardware `NSS` pin — without this, the peripheral trips a Mode Fault and silently drops out of master mode. See `notes.md` for the full explanation; this one cost real hardware-debugging time to track down.
- **Limitation:** SPI1 only wired up.

### OneWire (`onewire.c/h`)
`ow_init`, `ow_reset`, `ow_write_bit`, `ow_read_bit`, `ow_write_byte`, `ow_read_byte`. Bit-banged over GPIO, timed via a dedicated `TIM_Delay_us`. Every timing constant comes from the DS18B20 datasheet's AC electrical characteristics.

### DS18B20 temperature sensor (`ds18b20.c/h`)
`ds18b20_start_conversion`, `ds18b20_read_temp`. Built on OneWire.
- `ds18b20_read_temp` reads the full 9-byte scratchpad and validates it against the sensor's own Dallas/Maxim CRC8 (byte 8) before trusting the temperature bytes — 1-Wire is bit-banged with no dedicated clock line, so a corrupted read is rejected instead of silently returned.

### DS3231 RTC (`ds3231.c/h`)
`rtc_set_time`, `rtc_get_time`, `rtc_time_is_valid`, `bcd_to_dec`/`dec_to_bcd`. Built on I2C. `rtc_time_is_valid` checks the chip's Oscillator Stop Flag (OSF) to detect a dead/missing backup battery rather than trusting whatever time value comes back.

### SD card over SPI (`sd.c/h`)
`sd_send_command`, `sd_init`, `sd_read_block`, `sd_write_block`. SPI-mode SD card access (not SDIO). Handles the full CMD0/CMD8/ACMD41/CMD58 identification sequence, SDHC vs. SDSC block/byte addressing (detected automatically and cached from the OCR's CCS bit), and per-byte SPI timeout checking with clean chip-select handling on every error path.
- **Limitation:** the SD spec's ≤400kHz SPI clock requirement during card identification isn't enforced by this file — the caller is responsible for calling `SPI_Init` with a slow prescaler before `sd_init` and a faster one after.

## License

MIT — see [`LICENSE`](LICENSE).
