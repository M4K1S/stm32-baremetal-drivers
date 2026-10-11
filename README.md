# STM32 Bare-Metal Drivers

Register-level (no HAL) peripheral drivers for the STM32F446, written from the reference manual (RM0390) and each device's datasheet. The only vendor code they include is the CMSIS device header (`stm32f4xx.h`) for register names; there are no HAL or LL calls anywhere.

They were built for [ColdTrace](https://github.com/M4K1S/coldtrace), a fridge/freezer monitor, and are pulled out into their own repo so they can be reused and versioned separately. Everything in ColdTrace's hardware (temperature sensor, RTC, OLED, SD card, WiFi module) runs through these files.

[`notes.md`](notes.md) is the long-form design log: register sequences, the timing maths, and the debugging stories behind them. This README is the practical reference. Where the two disagree, this README and the code are right.

## Contents

- [Using the drivers](#using-the-drivers)
- [Conventions](#conventions)
- [Pins and timers](#pins-and-timers)
- [Drivers](#drivers): [GPIO](#gpio) · [TIM](#tim) · [UART](#uart) · [PWM](#pwm) · [I2C](#i2c) · [SPI](#spi) · [OneWire](#onewire) · [DS18B20](#ds18b20) · [DS3231](#ds3231) · [SD card](#sd-card) · [ESP-01S WiFi](#esp-01s-wifi) · [SSD1306 OLED](#ssd1306-oled)
- [Verification status](#verification-status)
- [Limitations](#limitations)
- [License](#license)

## Using the drivers

Add `Inc/` to the include path, compile everything in `Src/`, and define the device (`-DSTM32F446xx`). CMSIS headers must be on the include path. A complete working build is the [Makefile in ColdTrace](https://github.com/M4K1S/coldtrace/blob/main/Makefile). Add the repo as a submodule:

```bash
git submodule add https://github.com/M4K1S/stm32-baremetal-drivers.git BareMetalDrivers
```

**Clock assumption.** The drivers never configure the system clock. They assume the reset default (HSI, 16 MHz, no bus prescalers), so every peripheral and timer runs at 16 MHz. This is one define in [`Inc/clock.h`](Inc/clock.h):

```c
#define FCK_HZ 16000000U
```

If you move to a PLL clock, change `FCK_HZ` and also check the bus prescalers: timers on a bus with a prescaler other than 1 run at twice the bus clock, which the drivers do not account for.

**Minimal bring-up** (temperature, display and RTC; this is the pattern ColdTrace uses):

```c
#define TEMP_TIM TIM2   // 1 us timebase for 1-Wire
#define I2C_TIM  TIM3   // timeouts for I2C

ow_init(GPIOC, 0, TEMP_TIM);                     // DS18B20 on PC0

I2C_Init(I2C1, SPEED_FAST, I2C_TIM);             // OLED on PB8/PB9
ssd1306_init(I2C1, I2C_TIM);

I2C_Init(I2C3, SPEED_STANDARD, I2C_TIM);         // RTC on PA8/PC9

float t;
if (ds18b20_start_conversion(GPIOC, 0, TEMP_TIM)) {
    // wait at least 750 ms (12-bit conversion), doing other work if you like
    if (ds18b20_read_temp(GPIOC, 0, TEMP_TIM, &t)) {
        oled_draw_status_screen(0, 0, 0, 0, 0, 1, (int32_t)(t * 10), 0, 0, 0);
        oled_update(I2C1, I2C_TIM);
    }
}
```

## Conventions

- **No hidden state in the caller's way.** Each function takes the peripheral pointer (`GPIO_TypeDef *`, `I2C_TypeDef *`, `TIM_TypeDef *`...) plus plain parameters. Peripheral clocks and pins are enabled inside the `*_Init` calls.
- **Return values.** Anything that can fail returns `uint8_t`: `1` is success, `0` is failure or timeout. Exceptions: `sd_send_command` returns the card's raw R1 byte (`0xFF` means no response), and `GPIO_Read` / `TIM_Ready` return the bit they read.
- **Blocking, with timeouts.** I2C, SPI, SD and ESP calls spin on status flags, but never forever: each one is guarded by a timer you pass in. A dead device or unplugged wire gives a `0`, not a hang. Nothing uses interrupts or DMA, and nothing allocates memory.
- **Timers are borrowed, not owned.** The timeout/delay timer is a parameter. I2C, SPI, OneWire and TIM_Init configure it to count in microseconds (`TIM_Init(t, TIM_1MHZ_PRESCALER, 65535)`). Give each subsystem its own timer, or reconfigure a shared one after each use (ColdTrace does this for TIM2, see below).
- **Static state.** The SD driver remembers whether the card is SDHC in a file-level variable, so one card at a time. The OLED has one global framebuffer, so one display. Everything else is stateless.
- **Comments say why**, not what. The register-level reasoning is in `notes.md`.

## Pins and timers

Pins are fixed per peripheral instance by the `Init` functions. Pins on a different alternate-function mapping need a code change.

| Driver | Instance | Pins | Notes |
|---|---|---|---|
| UART | USART1 | PA9 TX, PA10 RX | AF7 |
| | USART2 | PA2 TX, PA3 RX | AF7 |
| | USART3 | PB10 TX, PB11 RX | AF7 |
| | UART4 | PC10 TX, PC11 RX | AF8, used for the ESP-01S |
| | UART5 | PC12 TX, PD2 RX | AF8 |
| | USART6 | PC6 TX, PC7 RX | AF8 |
| I2C | I2C1 | PB8 SCL, PB9 SDA | AF4, internal pull-ups on |
| | I2C2 | PB10 SCL, PB11 SDA | AF4. PB11 is not broken out on all Nucleo-64 boards |
| | I2C3 | PA8 SCL, PC9 SDA | AF4 |
| SPI | SPI1 only | PA5 SCK, PA6 MISO, PA7 MOSI | AF5. Chip select is a plain GPIO you choose |
| PWM | TIM2 channel 2 only | PA1 | AF1 |
| OneWire | any GPIO | your choice | open-drain with an **external** pull-up (about 4.7 kΩ) |

**Timer roles**, with the allocation ColdTrace uses:

| Timer | Width | Used for | In ColdTrace |
|---|---|---|---|
| TIM2 | 32-bit | 1-Wire delays, ESP timeouts | shared: reconfigured with `TIM_Init` before each ESP call, `ow_init` after |
| TIM3 | 16-bit | I2C timeouts | RTC and OLED, used one after the other |
| TIM4 | 16-bit | `TIM_Delay_*` | loop pacing and the 25 ms steps in the 750 ms wait |
| TIM5 | 32-bit | SPI timeouts | SD card |

`TIM_Init`, `TIM_Delay_*` and `TIM_Ready` support TIM2-TIM5. The ESP driver needs a **32-bit** timer (TIM2 or TIM5) because its timeouts run for seconds at 1 MHz. A 16-bit timer wraps after 65 ms.

## Drivers

### GPIO

`GPIO_Init`, `GPIO_SetMode`, `GPIO_SetPull`, `GPIO_Set`, `GPIO_Toggle`, `GPIO_Read`.

- `GPIO_Init` turns on the port clock (A to E) and sets the mode. Output type (push-pull or open-drain) is applied for both `OUTPUT` and `ALT_FUNC`, since I2C's SCL/SDA are alternate-function outputs that still need open-drain.
- `GPIO_SetPull` sets the internal pull-up or pull-down. `GPIO_Init` leaves the pull setting alone.
- There is no output-speed control (`OSPEEDR`); pins stay at the reset default, which is plenty for everything here.
- `GPIO_Set` and `GPIO_Toggle` use read-modify-write on `ODR`, so don't change the same port from both an interrupt and the main loop. Port H is not supported.

### TIM

`TIM_Init`, `TIM_Delay_us`, `TIM_Delay_ms`, `TIM_Start`, `TIM_Stop`, `TIM_Ready`.

- `TIM_Init(port, prescaler, autoReload)` takes plain counts: the tick is `FCK_HZ / prescaler` and the counter wraps after `autoReload` ticks. `TIM_Init(t, TIM_1MHZ_PRESCALER, 65535)` gives a 1 MHz microsecond counter.
- `TIM_Delay_us` returns after the counter passes the requested count. It must be **shorter than the auto-reload period** (about 65 ms for a 16-bit setup); longer requests never finish. `TIM_Delay_ms` chains 1 ms delays, so it has no such limit.
- `TIM_Delay_us` resets and stops the counter, so don't use a timer for a delay while something else is timing a timeout on it.

### UART

`UART_Init`, `UART_SendChar`, `UART_SendString`, `UART_ReceiveChar`, `UART_WaitForTC`, `UART_DataAvailable`.

- 8N1, no flow control, any baud rate. The baud register is `FCK_HZ / baud` rounded to the nearest integer (a 12.4 fixed-point number, since oversampling is 16). At 115200 and 16 MHz that is 139, which is 0.08 % off.
- `UART_ReceiveChar` **blocks** until a byte arrives. Check `UART_DataAvailable` first for a non-blocking read. There is no receive buffer, so the main loop has to keep up: at 115200 baud a byte arrives every 87 µs, and an unread byte is lost if the next one comes in first.
- `UART_SendChar` waits for the transmit register, not for the line to go idle. Call `UART_WaitForTC` after the last byte if you need to know it has fully left.

### PWM

`PWM_Init`, `PWM_Start`, `PWM_SetDuty`, `PWM_Stop`.

- **TIM2 channel 2 on PA1 only.** The `channel` parameter exists, but nothing else is implemented.
- PWM mode 1 with 1000 steps per period, so duty is 0 to 100 in 0.1 % hardware steps. `frequency` is in Hz, up to 16 kHz at 16 MHz. Out-of-range values are clamped or ignored.
- `PWM_Stop` disables the output but leaves the counter running.
- ColdTrace drives its buzzer with plain GPIO, so this driver is not exercised by it.

### I2C

`I2C_Init`, `I2C_Start`, `I2C_Stop`, `I2C_SendAddress`, `I2C_WriteByte`, `I2C_ReadByte`, `I2C_WriteReg`, `I2C_ReadReg`, `I2C_WriteBytes`, `I2C_ReadBytes`.

- Standard mode (100 kHz) or fast mode (about 381 kHz; the divider is rounded up so SCL never exceeds the 400 kHz limit). The timing registers (`FREQ`, `CCR`, `TRISE`) are derived from `FCK_HZ`.
- `dev_addr` is the **7-bit** address; the read/write bit is added for you.
- Every wait uses the timer you pass in, with a 5 ms limit per step. On any failure the high-level calls send a STOP and return `0`.
- `I2C_WriteReg`/`I2C_ReadReg` handle one register; `I2C_WriteBytes`/`I2C_ReadBytes` do a burst starting at a register address (the device auto-increments). The last byte of a read is NACKed automatically.
- Pins get internal pull-ups, which is fine for short wires and a single OLED/RTC. For longer buses add external 4.7 kΩ resistors. There is no bus-recovery routine for a device that holds SDA low.

### SPI

`SPI_Init`, `SPI_TransferByte`.

- SPI1 only, master, mode 0, 8-bit, MSB first. The prescaler is selectable (÷2 gives 8 MHz at 16 MHz, ÷64 gives 250 kHz).
- It sets `SSM`/`SSI` so chip select is just a GPIO you drive. Without these the peripheral sees the floating NSS pin as a Mode Fault and silently drops out of master mode.
- `SPI_TransferByte(port, tim, byte_in, &byte_out)` sends one byte and returns the byte received, with a 5 ms timeout.
- Calling `SPI_Init` again (for example to switch the SD card from 250 kHz to full speed) is safe; the peripheral is disabled while the divider changes.

### OneWire

`ow_init`, `ow_reset`, `ow_write_bit`, `ow_read_bit`, `ow_write_byte`, `ow_read_byte`.

- Bit-banged on any GPIO, timed by a microsecond timer. Timings follow the DS18B20 datasheet: reset pulse 480 µs with presence sampled at 100 µs, write-1 slot 5 µs low, write-0 slot 60 µs low, read slot sampled about 7 µs after the falling edge.
- The pin is **open-drain**: the driver only ever pulls low or releases. You need an **external pull-up** (about 4.7 kΩ to 3.3 V); the internal one is too weak.
- Bytes go LSB first. `ow_init` also reconfigures the timer, so call it again if another driver took the timer in between (see ESP below).
- Slot timing depends on the code running fast enough. The read sample point has a 15 µs window; ColdTrace runs this at 16 MHz with `-O0` and it holds, but re-check if you slow the clock or add work between the calls.

### DS18B20

`ds18b20_start_conversion`, `ds18b20_read_temp`.

- Single sensor on the bus (it uses Skip ROM, no addressing), powered from Vdd, not parasite power.
- Two calls so your code isn't stuck waiting: `start_conversion` begins a measurement; **wait at least 750 ms** (default 12-bit resolution), then `read_temp` returns the value in °C (0.0625 °C steps).
- `read_temp` reads all nine scratchpad bytes and checks the sensor's own CRC-8. It also rejects an all-zero scratchpad, because a data line shorted to ground reads as zeros and zeros pass the CRC, which would otherwise show as a perfectly valid 0.0 °C. A missing sensor, a bad CRC or a stuck bus all return `0` and leave the output unchanged.
- If you read before the conversion finishes the sensor returns its power-on value of 85.0 °C, which is a valid reading as far as the driver can tell. Respect the wait time.

### DS3231

`rtc_set_time`, `rtc_get_time`, `rtc_time_is_valid`, `bcd_to_dec`, `dec_to_bcd`.

- Runs on I2C at address `0x68`. `RTC_Time` holds seconds, minutes, hours (24-hour), day of week, date, month and a two-digit year.
- `rtc_set_time` writes all seven time registers in one burst and then clears the chip's Oscillator Stop Flag.
- `rtc_time_is_valid` reads that flag. It is set when the chip lost all power (dead backup battery, or first power-up), meaning the time can't be trusted. Check it at boot and set the clock from another source when it reports `0`.
- `rtc_get_time` does not check the flag; call `rtc_time_is_valid` for that.
- Alarms, temperature and the square-wave output are not used.

### SD card

`sd_init`, `sd_send_command`, `sd_read_block`, `sd_write_block`.

- SPI mode (not SDIO), raw 512-byte blocks, no filesystem. `sd_init` runs the identification sequence (74+ clocks, CMD0, CMD8, ACMD41, CMD58) and detects SDHC/SDXC versus older SDSC cards, so block numbers are translated to byte addresses where needed.
- Needs an **SD v2.0 or newer** card (SDHC/SDXC, or a v2 standard-capacity one). Version 1.x cards don't understand CMD8 and are rejected.
- **The caller sets the SPI speed.** The card must be identified at 400 kHz or less, so call `SPI_Init` with a slow prescaler (÷64 is 250 kHz) before `sd_init`, then again with a fast one (÷2 is 8 MHz) afterwards.
- Chip select is a GPIO you pass in; the driver pulls it low per command and always releases it on every error path.
- Waits are counted in bytes clocked, so the real time depends on the SPI speed: a write waits for the card's busy signal for up to 100000 bytes, and a read waits up to 20000 bytes for the data token. These are sized (by estimate, not measured on the board) to cover the spec's 250 ms write and 100 ms read limits at full speed, where per-byte software overhead dominates. They only matter when something is wrong; a healthy card answers within a few bytes.
- CRC checking is off, as is the default in SPI mode. The card's own write-protect and card-detect pins aren't read.
- There's no wear levelling or error-correction layer. ColdTrace uses it as a simple append-only log with its own record format.

### ESP-01S WiFi

`esp_set_idle_tick`, `esp_send_command`, `esp_wait_response`, `esp_reset`, `esp_wifi_associated`, `esp_connect_wifi`, `esp_send_reading`, `esp_send_hourly`, `esp_sync_time`, `format_float`.

This is an AT-command layer for an ESP8266 running the stock Espressif AT firmware over a UART. It builds the ASCII commands, matches the replies, and hand-builds plain HTTP/1.1 requests over a TCP socket.

**What the calls do**

| Call | Does |
|---|---|
| `esp_reset` | Soft reset (`AT+RST`), waits for `ready`. Recovers an ESP stuck mid-command. |
| `esp_connect_wifi` | Station mode, then `AT+CWJAP`. Returns `1` only on a real `OK`; `FAIL` or a 15 s timeout returns `0`. |
| `esp_wifi_associated` | `AT+CWJAP?`: is the ESP still joined to the router? Tells a dropped WiFi link apart from an unreachable server. |
| `esp_send_reading` | POSTs one live reading as JSON to `/api/reading`. |
| `esp_send_hourly` | POSTs one finished hour (min/max/avg ×100, door samples, sample count) to `/api/hourly`. |
| `esp_sync_time` | `HEAD /api/data`. Reads the server's `Date` header into an `RTC_Time` (UTC), plus optional config headers: `X-Tz-Offset`, `X-Alarm-Threshold`, `X-Alarm-Delay`, `X-Last-Hour`. Missing headers leave the caller's values unchanged. |
| `format_float` | Float to decimal text without float-capable `printf`. |

A POST returns `1` when the ESP reports `SEND OK`, meaning the bytes left the module, not that the server accepted them. The driver then drains the server's reply (up to `CLOSED`) so the next request doesn't mistake that reply's `200 OK` for its own.

**How it stays reliable**

- **Constant-cost reply matching.** Replies are matched by comparing only the last few received bytes, not by searching the whole buffer on every byte. A `strstr` per byte took longer than one byte time at 115200 baud and lost data, which showed up as the connection mysteriously failing.
- **Sliding buffers.** When a reply buffer fills, the newest half is kept, so the tail (where `OK` or `CLOSED` arrives) is never lost, however long the reply.
- **Flush before each command.** Leftover bytes from the previous reply are discarded, so each reply can only be the answer to the command just sent.
- **Join echo skipped.** The echoed `AT+CWJAP="ssid","password"` line is ignored when waiting for the join result.
- **Fail fast.** A connect attempt stops on `ERROR` instead of waiting out the timeout.

**The idle hook.** The blocking waits can last up to 15 seconds. `esp_set_idle_tick(fn)` registers a function that is called about every 25 ms from inside every wait, so the application can keep LEDs, buttons, the display and the watchdog serviced while WiFi is slow. Pass `NULL` to disable it. The hook runs inside the ESP calls, so it **must not** call any `esp_*` function and **must not touch the ESP's timer**.

**Requirements and limits**

- Pass a **32-bit timer at 1 MHz** (`TIM_Init(tim, TIM_1MHZ_PRESCALER, 0xFFFFFFFF)`). If a 1-Wire driver shares that timer, set it up again with `ow_init` after the ESP call.
- Set up for 115200 8N1 on the ESP's UART.
- Host name up to about 39 characters; the request, command and reply buffers are fixed size (`HEAD` replies are read up to 511 bytes, so the headers you need must come early enough). Header names are matched case-sensitively.
- The WiFi name and password go into the command as-is, so a quote, comma or backslash in them would need to be escaped with a backslash.
- Plain HTTP on port 80 only, no TLS.

For the hardware side, see the "bring-up" section of [`notes.md`](notes.md). The ESP-01S needs its own supply that can deliver about 300 mA peaks (a weak one makes it reset mid-transmit), a common ground with the STM32, and a large capacitor on its 3.3 V rail.

### SSD1306 OLED

`ssd1306_init`, `oled_set_pixel`, `oled_get_pixel`, `oled_clear`, `oled_update`, `oled_draw_icon`, `oled_draw_small_time`, `oled_draw_status_screen`.

- 128×64, I2C address `0x3C`, built on the I2C driver. The control byte (command or data) rides in the register-address slot of `I2C_WriteBytes`, so no extra I2C primitives were needed.
- Drawing happens in a RAM framebuffer (1 KB) that matches the display's page layout, so `oled_update` is one sequential dump of eight 128-byte chunks. Nothing appears on the panel until `oled_update`.
- `ssd1306_init` enables the charge pump and sets sensible defaults (contrast `0x7F`, flipped to match common modules). Call `oled_update` after it to show the cleared buffer.
- `oled_draw_status_screen` is an application-level layout kept here for convenience: WiFi and SD icons top left, a clock top right, a large temperature (one decimal place, `temp_deci` in tenths of a degree) in the middle, and a mute icon with the time left bottom left. It is specific to ColdTrace; use the primitives above for your own screens.
- Fonts and icons are in `oled_fonts.c`: 14×11 icons, 8×11 small digits for times, and 11×16 big digits that are drawn at 2×. Everything is stored as plain byte arrays, with no font library.

## Verification status

| Area | State |
|---|---|
| GPIO, TIM, UART, I2C, SPI, OneWire, DS18B20, DS3231, SD, OLED | Run on real hardware (WeAct STM32F446RE) as part of ColdTrace |
| ESP (reset, join, live and hourly POST, time and config sync, idle hook) | Run on real hardware. The request/reply logic was also simulated on a host with a fake UART (join failure, timeouts, back-to-back posts) |
| PWM | Not used by ColdTrace; the latest edits to it have not been run |
| This revision's changes (integer baud calculation, I2C fast-mode divider rounded up, SD wait lengths, DS18B20 stuck-bus check, DS3231 century bit) | Compile clean with `-Wall -Wextra`. The DS18B20 check and baud values were verified on a host; the rest need a run on the board |

## Limitations

- STM32F446 at 16 MHz with no clock setup; other F4 parts need pin/clock checks, other families need register changes.
- No interrupts, DMA, or RTOS integration: everything blocks, bounded by timeouts.
- I2C has no bus-recovery routine; SPI is SPI1 only; PWM is TIM2 channel 2 only.
- SD cards must be SD v2.0 or newer, and the caller manages the SPI speed switch.

## License

MIT, see [`LICENSE`](LICENSE).
