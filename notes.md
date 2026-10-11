# Bare-Metal STM32F446 Peripheral Notes

Step-by-step register sequences for each peripheral, plus the math behind any timing calculations. Written as a personal reference for how/why each driver was built.

**This is a design log, not the API reference.** [`README.md`](README.md) is the current reference; where this file disagrees with it or the code, those win. Known differences: `UART_Init` now computes `BRR` with integer maths (see the UART section), and the ESP8266 section below describes the first version of the driver. It has since gained the idle hook, constant-cost reply matching, hourly uploads and time/config sync (see the README).

---

## GPIO

**`GPIO_Mode` enum values are deliberately chosen to match `MODER`'s actual bit encoding:**
```c
typedef enum { INPUT, OUTPUT, ALT_FUNC, ANALOG } GPIO_Mode;   // 00, 01, 10, 11
typedef enum { PUSH_PULL, OPEN_DRAIN } GPIO_OutputType;        // 0, 1
```
Because C enums auto-assign sequential values from 0, and those values happen to equal the register's real 2-bit (`MODER`) and 1-bit (`OTYPER`) codes, both registers can be set directly from the enum value with no if-chain:
```c
port->MODER |= (mode << (pin * 2));
port->OTYPER |= (type << pin);
```

**Steps to configure a pin (`GPIO_Init`, one-time setup):**
1. Enable the GPIO port's clock in `RCC_AHB1ENR` (bit position = port index: A=0, B=1, C=2...)
2. Clear the pin's 2 bits in `MODER` (each pin gets 2 bits, at position `pin_number * 2`), then set from the `GPIO_Mode` value
3. If `mode == OUTPUT` or `mode == ALT_FUNC`: clear then set the pin's 1 bit in `OTYPER` from the `GPIO_OutputType` value. Both modes actively drive the pin (a peripheral's alternate-function output is still a physical output, e.g. I2C's SCL/SDA), so both need push-pull vs open-drain configured. `OTYPER` is genuinely irrelevant only for Input or Analog modes, which is why it's gated on `mode` at all rather than always written. (Earlier this only checked `mode == OUTPUT`, so an `ALT_FUNC` + `OPEN_DRAIN` call would silently leave `OTYPER` untouched — fixed, though no caller was hitting that path since I2C configures its own AF pins by hand instead of going through `GPIO_Init`.)

**`GPIO_SetMode` (lightweight, for switching direction mid-transaction):** only touches `MODER`, does not re-enable clocks or touch `OTYPER`. Needed for protocols like 1-Wire that flip a single pin between Output and Input repeatedly within one transaction, `OTYPER`'s open-drain setting (configured once via `GPIO_Init`) persists correctly across these switches since `SetMode` never clears it.

**Push-pull vs open-drain, and why 1-Wire (and I2C) need open-drain:** push-pull can actively drive both HIGH and LOW. Open-drain can only actively pull LOW, releasing (floating) is the only way to go HIGH, relying on an external pull-up resistor to do it. Necessary any time multiple devices might drive the same line, so two devices disagreeing can never short-circuit each other, worst case is one device's LOW wins.

**To write a pin:** `ODR` (Output Data Register), set the bit to drive HIGH, clear it to drive LOW. `GPIO_Toggle` XORs the bit instead of set/clear. Only meaningful once a pin is in Output mode.

**To read a pin:** `IDR` (Input Data Register), shift the target bit down to position 0 and mask with `& 0b1U` to isolate it:
```c
uint8_t GPIO_Read(GPIO_TypeDef *port, uint8_t pin) {
    return (port->IDR >> pin) & 0b1U;
}
```
Needed any time an open-drain line is being driven by something else (another device pulling it low) and you need to sense that state, rather than just recalling what you last wrote to `ODR`.

---

## UART

**Steps to initialize:**
1. Enable the USART/UART peripheral's clock (`RCC_APB1ENR` for USART2/3, UART4/5; `RCC_APB2ENR` for USART1/6, split because USART1/6 sit on the faster APB2 bus)
2. Enable the GPIO port clock for TX/RX pins
3. Clear then set `MODER` bits for TX/RX pins to alternate function (`10`)
4. Clear then set `AFR[]` to the correct AF number (AF7 for USART1/2/3, AF8 for UART4/5/USART6)
5. Calculate and write `BRR` (baud rate register), see equation below
6. Set `CR1`: enable `UE` (USART enable), `RE` (receiver enable), `TE` (transmitter enable)
7. Set `CR2`'s `STOP[1:0]` field, **bits 13:12** (00 = 1 stop bit) — this project only ever writes `00`, so a bit-position mixup here (`<<13` instead of `<<12`) produced the same all-zero result and went unnoticed; worth calling out explicitly since it's not visible from the current value alone, only from actually checking the field's bit range in RM0390

**Baud rate (BRR) equation:**
```
USARTDIV = fCK / (16 × baudRate)
```
`USARTDIV` isn't a whole number in general. `BRR` stores it as a 12.4 fixed-point number: the whole part (mantissa) in bits 15:4 and a 4-bit fraction in bits 3:0. Multiplying `USARTDIV` by 16 gives exactly that fixed-point value:
```
BRR = round(16 × USARTDIV) = round(fCK / baudRate)
```
In code that is one integer expression, `(FCK_HZ + baudRate / 2) / baudRate`, which rounds to nearest and carries a fraction overflow into the mantissa automatically. (The first version split the mantissa and fraction by hand with floating point; it produced identical values for every common baud rate, but pulled in `libm` and doubles. At 115200 and 16 MHz, `BRR = 139`, 0.08 % off.)

**Sending a char:** wait for `TXE` (bit 7 of `SR`) to be 1 (transmit buffer empty), then write the byte to `DR`.
**Receiving a char:** wait for `RXNE` (bit 5 of `SR`) to be 1 (data ready), then read `DR`.
**Checking data availability (non-blocking):** read `RXNE` once, don't wait, return true/false immediately.

---

## TIM (base timer, no PWM)

**Steps to initialize (explicit PSC/ARR):**
1. Enable the timer's clock (`RCC_APB1ENR` for TIM2-7, `RCC_APB2ENR` for TIM1/8-11)
2. Write `PSC` (prescaler - 1) and `ARR` (auto-reload - 1)
3. Reset `CNT` to 0
4. Set `EGR`'s `UG` bit to force an update event (loads PSC/ARR immediately rather than waiting for the next natural overflow)
5. Clear `SR`'s `UIF` bit (clean starting state)

**To start/stop:** set/clear `CR1`'s `CEN` bit (bit 0).
**To poll for a completed period:** check `SR`'s `UIF` bit; if set, clear it and report "ready."

**Timer frequency equation:**
```
Output frequency = Timer input clock / ((PSC + 1) × (ARR + 1))
```
Timer input clock isn't always the same as the APB bus clock, **if the APB prescaler isn't 1 (i.e. APB clock ≠ AHB clock), the timer clock is automatically doubled** by hardware. Example: APB1 = 45MHz (from a /4 prescaler off 180MHz AHB) → TIM2/4's actual input clock = 90MHz, not 45MHz.

---

## PWM

**Steps to initialize (TIM2 Channel 2 example):**
1. Enable timer clock + GPIO clock, same as base TIM
2. Configure the output pin as alternate function, correct AF number for the timer
3. Set `ARR` (period) and `PSC` (prescaler), same frequency equation as base TIM, since PWM runs on the same counter
4. Configure `CCMRx`'s `OCxM` bits to `110` (PWM Mode 1) and `OCxPE` bit to `1` (preload enabled, prevents mid-cycle glitches when duty cycle is updated while running)
5. Set `CCRx` to the initial duty cycle value

**To start:** set `CCER`'s `CCxE` bit (connects the channel's internal signal to the physical pin), then set `CR1`'s `CEN` bit (starts the counter).
**To stop:** clear `CCER`'s `CCxE` bit.
**To change duty cycle live:** just write a new value to `CCRx`, safe to do while running because `OCxPE` is enabled, so the change applies cleanly at the next period boundary instead of glitching mid-cycle.

**Duty cycle scaling:** if `ARR = 999` (1000 counts) and duty is expressed as a 0-100 percentage:
```
CCRx = dutyCycle × 10
```
(50% → CCRx = 500 → half of 1000 counts → 50% of each period spent "on.")

**CCMRx register layout (why two registers):** `CCMR1` covers channels 1-2, `CCMR2` covers channels 3-4. Each channel gets one byte within its register: channel 1 = bits 0-7, channel 2 = bits 8-15 (of CCMR1); channel 3 = bits 0-7, channel 4 = bits 8-15 (of CCMR2). Within each byte: `OCxPE` is at relative bit 3, `OCxM` is at relative bits 6:4, so for channel 2 specifically, that's absolute bit 11 (OC2PE) and bits 14:12 (OC2M).

**PWM Mode 1 vs Mode 2:** Mode 1 = output HIGH while counter < CCR, LOW after. Mode 2 = inverted (LOW while counter < CCR, HIGH after). Achieves the same kind of inversion as flipping `OCPolarity`, only need one or the other, not both (they'd cancel out).

**Active-high vs active-low wiring (why polarity matters):**
- Active-high (`GPIO → LED → GND`): pin HIGH = current flows = LED on. Bigger duty cycle = brighter, intuitively.
- Active-low (`3.3V → LED → GPIO`): pin LOW = current flows = LED on (pin sinks current to ground). Bigger duty cycle = pin spends more time HIGH = LED off more, inverted from what you'd expect. Fix by using `OCPOLARITY_LOW` instead of `HIGH`, or use PWM Mode 2 instead of Mode 1 (equivalent effect, pick one).

---

## I2C

I2C uses two shared, open-drain lines (SDA = data, SCL = clock) that any number of devices can sit on. No device can ever actively drive either line HIGH, only pull it LOW; an external pull-up resistor is what brings the line back HIGH whenever nothing is pulling it down. This is the same open-drain concept as GPIO's `OPEN_DRAIN` type, just required here because the bus is shared: if a device could drive HIGH, two devices disagreeing on the line's state would short-circuit each other. Open-drain means the worst case is just one device's LOW winning, never a conflict.

One consequence worth knowing up front, since it explains a register later on: pulling LOW is fast (a transistor actively switching on), but rising back to HIGH is slower (passive charging of the wire's capacitance through the pull-up resistor). SDA and SCL only get read as valid data while SCL is HIGH; a START condition is SDA going HIGH→LOW *while SCL is HIGH* (a deliberate break from the normal rule that SDA only changes while SCL is LOW, which is exactly why it's recognizable as a special signal and not a data bit), and STOP is the reverse (LOW→HIGH while SCL is HIGH).

### Setup (`I2C_Init`, I2C1 example, PB8=SCL/PB9=SDA)

1. Enable the I2C peripheral clock (`RCC_APB1ENR`) and GPIO clock
2. Configure SDA/SCL pins as alternate function, **AF4** (consistent across I2C1/2/3 on this chip)
3. Set `OTYPER` to open-drain and `PUPDR` to pull-up on both pins, for the shared-bus reasons above. `OTYPER` is 1 bit/pin (0=push-pull, 1=open-drain); `PUPDR` is 2 bits/pin like `MODER` (`01`=pull-up)
4. Set `CR2`'s `FREQ` bits (bits 5:0) to the peripheral's own input clock speed in MHz, this is just telling the hardware what clock it's running on, not the bus speed itself
5. Set `CCR`, the actual SCL bus-speed divider, plus the `F/S` bit (bit 15: 0=Standard mode/100kHz, 1=Fast mode/400kHz). See the CCR equation below for how this value is calculated
6. Set `TRISE`, the maximum time (in clock cycles) the peripheral will allow for that passive rise-to-HIGH to complete before it treats the line as having a fault. See the TRISE equation below
7. Set `CR1`'s `PE` bit (Peripheral Enable) **last**, the peripheral locks `CCR`/`TRISE` once enabled, so they must be configured first

**One-time timeout timer setup, added inside `I2C_Init`:** also calls `TIM_Init(tim_port, 16, 65535)` to configure a dedicated timer at a 1MHz tick rate, used by every transaction function below for timeouts (see next section). Same one-time-setup pattern as `ow_init()`.

**Multiple I2C instances (`I2C1`/`I2C2`/`I2C3`), added for the SSD1306:** `I2C_Init` takes the pin-config branch (`if (port == I2C1) {...}`) purely as a way to pick the right GPIO pins/AF for whichever peripheral pointer was passed in — the CCR/TRISE/FREQ/PE steps below it are identical regardless of instance, so those aren't duplicated per-port. Two more branches were added the same way:
- **I2C2** (`PB10`=SCL, `PB11`=SDA, AF4) — the chip's documented second I2C bus.
- **I2C3** (`PA8`=SCL, `PC9`=SDA, AF4) — the chip's third I2C bus. This is the one actually wired up for the OLED (see the SSD1306 section below): on this specific Nucleo-64 board, `PB11` isn't broken out to any header pin at all (confirmed against the board's own printed pinout diagram, not just assumed) — a known Nucleo-64 limitation, not a wiring mistake — so `I2C2` is present in the driver for completeness but not usable on this board. `PA8`/`PC9` are both present on the Arduino-style header (`D7` and the unlabeled pin above `D15`), and are the standard fallback "second I2C bus" pins used on Nucleo boards for exactly this reason.

Multiple I2C peripherals can be brought up independently and used concurrently (e.g. `I2C1` for the DS3231, `I2C3` for the OLED) — each is a fully separate set of registers/pins, nothing shared between them except, in this project, the *timeout timer* (see the OLED wiring note below for why that's safe).

### Transaction primitives (built on top of `I2C_Init`)

Every function below that waits on the bus or an external device shares an identical timeout structure: reset the timeout timer's `CNT` to 0, start it, poll a status flag while checking `CNT` against `I2C_TIMEOUT_US` (`#define`'d as 5000, i.e. 5ms, applied uniformly rather than tuned per-function), stop the timer, return 1 (success) or 0 (timeout). This is required because a bare `while(!flag);` hangs the whole program forever if a device never ACKs, wiring is bad, or the bus is stuck.

**`I2C_Start()`:** sets `CR1`'s `START` bit (bit 8) to generate a Start condition, polls `SR1`'s `SB` flag (bit 0) which the hardware sets once that condition has actually finished being generated on the bus. Hardware auto-clears `START` once done, no manual clear needed.

**`I2C_Stop()`:** sets `CR1`'s `STOP` bit (bit 9). No flag to wait on since nothing downstream depends on the Stop condition completing before the code moves on; hardware auto-clears the bit once the Stop condition completes.

**`I2C_SendAddress(addr, rw)`:** formats the address byte as `(addr << 1) | rw` (the 7-bit device address shifted up one, with the read/write bit in position 0; per I2C convention `rw=0` means write, `rw=1` means read), writes it to `DR`, then polls `SR1`'s `ADDR` flag (bit 1), which sets once the addressed device has acknowledged.

Clearing `ADDR` requires a specific two-register read sequence, not a direct bit clear: **read `SR1`, then read `SR2`**, discarding both values with `(void)`. This isn't optional or just informational, it's the literal hardware mechanism that clears the flag; skipping either read leaves `ADDR` stuck set and corrupts whatever operation comes next. `SR2` itself holds additional transaction-state bits (`MSL`, `BUSY`, `TRA`) that aren't used here but are part of why the two-step sequence exists, the address-match event bundles "address sent" (SR1) together with "here's the resulting mode" (SR2), and the hardware wants both looked at before it lets the flag clear.

**`I2C_WriteByte(byte)`:** writes to `DR`, polls `SR1`'s `TXE` flag (bit 7, "transmit register empty" = the peripheral has moved the last byte along and is ready for the next one to be loaded).

**`I2C_ReadByte(ack, *byte_out)`:** polls `SR1`'s `RXNE` flag (bit 6, "receive register not empty" = a byte has fully arrived and is sitting in `DR`). The `ACK` bit (`CR1` bit 10) has to be set **before** the wait loop starts, not after, because the hardware auto-generates the ACK/NACK for a byte at the exact moment that byte finishes arriving, based on whatever `ACK` was set to at that instant. Setting it after `RXNE` fires is too late, that byte's ACK/NACK has already gone out. `ACK=1` tells the sender "keep going" (use for every byte except the last one in a multi-byte read); `ACK=0` tells it "stop" (use for the final byte).

**Output-parameter pattern (`I2C_ReadByte`, and `ds18b20_read_temp` updated to match):** the return value is dedicated purely to success/failure (1/0); the actual data comes back through a pointer parameter (`*byte_out`, `*temp_out`) that the function writes into via the caller's address (`&variable`, i.e. `*ptr = value` follows the pointer to write directly into the caller's memory). This avoids the sentinel-value problem, a `uint8_t` byte has no value that's safely "impossible" to use as an error marker (unlike, say, `-999°C` for a temperature, which is outside any real range), since any of the 256 possible byte values could be genuinely valid data.

### Higher-level (built on the primitives above)

Every function below bails out to `I2C_Stop()` + `return 0` the moment any primitive call fails, rather than pressing on regardless, so a mid-transaction failure never gets reported as a success and never leaves the bus in an unknown state.

**`I2C_WriteReg(dev_addr, reg_addr, value)`:** `Start → SendAddress(WRITE) → WriteByte(reg_addr) → WriteByte(value) → Stop`. The two data bytes aren't special to the I2C protocol itself, electrically they're just two ordinary bytes sent one after another; their meaning ("set register pointer" then "store this value there") comes entirely from how the receiving device's own internal logic is designed to interpret the byte immediately following its address, a convention most I2C peripheral chips follow, documented per-device in that device's own datasheet.

**`I2C_ReadReg(dev_addr, reg_addr, *value_out)`:** needs a **repeated Start**, not a straight sequence, because reading requires two distinct phases: first tell the device which register you want (write phase: `Start → SendAddress(WRITE) → WriteByte(reg_addr)`), then actually receive it (read phase: `Start` again (repeated, no `Stop` in between) `→ SendAddress(READ) → ReadByte(NACK)`, NACK since it's the only/last byte). The repeated Start (rather than a full Stop + fresh Start) matters because a full Stop releases the bus, letting another device potentially interleave a transaction in the gap, a repeated Start keeps the whole write-then-read exchange atomic and uninterrupted. Electrically, a (repeated) Start is also the only defined mechanism for signaling "direction may now switch", there's no other way to tell a device "stop listening, start talking" without it, since something has to hand control of SDA from the master (driving it during write) to the device (driving it during read).

**`I2C_WriteBytes(dev_addr, reg_addr, *data, len)`:** same shape as `WriteReg` but loops `WriteByte(data[i])` for `len` bytes after the register address, no repeated Start needed since the whole operation stays in write mode throughout.

**`I2C_ReadBytes(dev_addr, reg_addr, *data, len)`:** same repeated-start shape as `ReadReg`, but loops `len` times, ACKing every byte except the last (which gets NACK'd). Needed over calling `ReadReg` `len` times in a loop for two reasons: efficiency (one `Start`/`Stop` cycle instead of `len` of them), and atomicity, for something like the DS3231's 7 time registers, `len` separate reads risk a value changing (e.g. seconds rolling over) between calls, giving a self-inconsistent time snapshot; one held-open burst read guarantees all 7 bytes reflect the same instant.

**Caller must guarantee `data` points to a buffer of at least `len` bytes.** `uint8_t *data` carries no size information at runtime, C's type system can't and won't check this, a pointer to a single byte and a pointer to the first element of a 100-byte array look identical to the compiler. Passing a `len` larger than the actual buffer writes past its end (undefined behavior, a real and classic class of C bug), it's a contract enforced only by the caller matching the buffer's real allocated size to what's passed as `len`.

---

## DS3231 (RTC, device-specific layer built on I2C)

Address `0x68`, confirmed directly from the datasheet ("the 7-bit DS3231 address, which is 1101000").

**Why register auto-increment isn't an I2C feature, it's chip-specific:** I2C itself has no concept of "registers", it just moves raw bytes. The DS3231's own internal logic is what interprets the first byte after its address as "set my internal register pointer here", and increments that pointer itself after every subsequent byte transferred (confirmed in the datasheet: "This sets the register pointer... The register pointer increments after each data byte is transferred"). This is a convention this specific chip was designed to follow, not something the I2C protocol or your STM32 peripheral guarantees, different I2C devices can and do behave differently, always check the specific device's datasheet.

**Time/date registers (`0x00`-`0x06`) are stored in BCD**, not plain binary, confirmed directly in the datasheet. BCD packs each individual decimal digit into its own 4-bit nibble (upper nibble = tens digit, lower nibble = ones digit) rather than converting the whole number into one combined binary value, e.g. decimal 47 as BCD is `0100 0111` (`0x47`), completely different bits from plain-binary 47 (`00101111`). Historically chosen because it let old RTC/calculator hardware drive 7-segment displays directly from the raw register bits with zero binary-to-decimal conversion circuitry needed, one nibble maps straight to one digit. Largely legacy baggage on a modern microcontroller-based project like this one, doesn't buy anything a plain binary register wouldn't, it's just what this chip's convention already is.

**`bcd_to_dec(bcd)` / `dec_to_bcd(dec)`:** the translation layer between "how the chip stores a value" (BCD) and "how the rest of the code wants to use it" (plain decimal), same shift-and-mask technique used throughout this whole project:
```c
uint8_t tens = bcd >> 4;        // upper nibble
uint8_t ones = bcd & 0b1111U;   // lower nibble, masked
uint8_t dec = tens * 10 + ones;
```
and the reverse:
```c
uint8_t tens = dec / 10;   // integer division
uint8_t ones = dec % 10;   // remainder
uint8_t bcd = (tens << 4) | ones;
```

**Hours register (`0x02`) is the one exception to pure BCD**, per the datasheet's register map: bit 7 is unused (always 0), bit 6 is a 12/24-hour mode select, and bit 5 means different things depending on that mode (AM/PM in 12-hour mode, or the "20-hour" flag in 24-hour mode). This project always uses 24-hour mode, so bit 5 is genuinely part of the encoded hour value (needed to represent hours 20-23, since the BCD tens digit alone can't exceed what 2 bits allow) and must be kept, only bits 6 and 7 get masked off before running the byte through `bcd_to_dec()`:
```c
time->hours = bcd_to_dec(buffer[2] & 0b00111111U);
```
No equivalent masking is needed on the write side (`dec_to_bcd()` on any hour 0-23 never sets bit 6 or 7 in the first place, so 24-hour mode is preserved automatically).

**`rtc_get_time(*time)`:** one `I2C_ReadBytes()` burst call starting at register `0x00` for 7 bytes (relying on the chip's auto-increment to walk seconds through year in one atomic transaction), then each byte through `bcd_to_dec()` (masked for hours) into the matching `RTC_Time` struct field via the pointer (`time->field = ...`, arrow operator since `time` is a pointer to the struct, not the struct itself).

**`rtc_set_time(*time)`:** the mirror operation, each struct field through `dec_to_bcd()` into a local `buffer[7]`, then one `I2C_WriteBytes()` burst call to push all 7 at once. Per the datasheet, writing all 7 registers within the same burst (rather than 7 separate writes) matters because "the countdown chain is reset whenever the seconds register is written... the remaining time and date registers must be written within 1 second" to avoid rollover inconsistencies, a single held-open transaction guarantees this automatically.

**Why a burst read/write instead of 7 separate `ReadReg`/`WriteReg` calls:** efficiency (one Start/Stop cycle instead of seven), and atomicity, separate reads risk a value changing (e.g. seconds rolling over) between calls, giving a self-inconsistent time snapshot. The datasheet confirms the chip is explicitly designed around this: reads are synchronized to a secondary buffer on every I2C START specifically so a multi-byte burst read is guaranteed self-consistent even if the internal clock ticks over mid-read.

**`rtc_time_is_valid(port, tim_port)`:** checks whether the chip's own clock has been running continuously, before trusting any time it reports. Reads the Status register (`0x0F`) and checks bit 7, the **Oscillator Stop Flag (OSF)** — the chip sets this bit itself, in hardware, whenever its internal oscillator has stopped and restarted, which happens specifically when it loses both main power and backup battery power at the same time (dead/missing coin cell, or a fresh chip that's never been set). This is a better signal to check than inspecting the time values themselves, a reset RTC can land on a plausible-looking but wrong time (e.g. midnight, Jan 1) that would pass a naive sanity-range check while still being meaningless, OSF is the chip's own explicit "don't trust me" flag rather than something inferred after the fact.
```c
uint8_t status;
if (!I2C_ReadReg(port, tim_port, DS3231_ADDR, DS3231_REG_STATUS, &status)) {
    return 0; // can't talk to the chip at all, treat as untrustworthy
}
return (status & DS3231_OSF_BIT) == 0; // true only if OSF is clear
```
`status & DS3231_OSF_BIT` masks out every bit except bit 7 (same shift-and-mask pattern used throughout this project), giving `0x80` if the flag is set or `0` if it's clear. The explicit `== 0` turns that into a clean `1`/`0` rather than returning the raw `0x80`, which would happen to still work in an `if` check (non-zero is truthy) but is a confusing value to return given the function's own success/fail convention elsewhere. The return is also intentionally inverted from the raw flag (`1` = valid = OSF *not* set), so the function reads naturally at the call site ("is the time valid") rather than making every caller remember to negate the raw hardware flag themselves. Used in `App_Init()` to conditionally prompt for a time-set over UART only when actually needed, instead of every boot.

### Equations

**CCR (clock divider), derivation:**

The manual defines, for Standard mode: `Thigh = CCR × TPCLK1`, `Tlow = CCR × TPCLK1`, where `TPCLK1` is the *period* of the peripheral clock (`TPCLK1 = 1 / fPCLK1`), not the frequency. Total SCL period = Thigh + Tlow = `2 × CCR × TPCLK1`, and SCL frequency is the reciprocal of that period: `fSCL = 1 / (2 × CCR × TPCLK1)`. Solving for CCR and substituting `TPCLK1 = 1/fPCLK1`:
```
CCR = fPCLK1 / (2 × fSCL)      [Standard mode]
```
Fast mode (DUTY=0) uses `Tlow = 2 × CCR × TPCLK1` instead of `1×`, making the total period `3 × CCR × TPCLK1`:
```
CCR = fPCLK1 / (3 × fSCL)      [Fast mode, DUTY=0]
```
Worked example (fPCLK1 = 16MHz, Standard mode, fSCL = 100kHz): `CCR = 16,000,000 / (2 × 100,000) = 80`.

**TRISE:**

Derived from the I2C bus spec's maximum allowed SCL rise time: 1000ns for Standard mode, 300ns for Fast mode (Fast mode's shorter clock period leaves less timing margin, so the allowed rise time is tighter):
```
TRISE = (fPCLK1 × maxRiseTime) + 1
```
Standard mode (1000ns) simplifies to just FREQ in MHz + 1.

---

## TIM microsecond delay (`TIM_Delay_us`)

1-Wire's timing (tens to hundreds of microseconds) is too fast and too precise for a software counting loop, needs a hardware timer with a known, exact tick rate.

**Setup:** call `TIM_Init(port, prescaler, autoReload)` once with a prescaler chosen so the tick rate is exactly 1MHz (1 tick = 1µs), so the delay function needs no extra math at the call site:
```
PSC + 1 = timer_input_clock / 1,000,000
```
At 16MHz input clock: `PSC = 16,000,000/1,000,000 - 1 = 15`, so call `TIM_Init(port, 16, 65535)` (large `autoReload` just gives headroom, actual delay length is controlled per-call, not by ARR).

**The delay function itself:** reset `CNT` to 0, start the counter, busy-wait until `CNT` reaches the requested microsecond count, stop the counter:
```c
void TIM_Delay_us(TIM_TypeDef *port, uint32_t micros){
    port->CNT = 0;
    port->CR1 |= (0b1U);
    while(port->CNT < micros) {}
    port->CR1 &= ~(0b1U);
}
```
Resetting `CNT` every call sidesteps any overflow/wraparound concerns entirely, since no 1-Wire delay used here comes anywhere close to the counter's max value (65535 for a 16-bit timer).

---

## OneWire (1-Wire protocol, DS18B20)

No dedicated peripheral, bit-banged over GPIO + `TIM_Delay_us`. Every timing number below comes from the DS18B20 datasheet's AC Electrical Characteristics table and Figure 15/16 timing diagrams.

**One-time setup (`ow_init`, called once at startup, not per-transaction):**
- `GPIO_Init(gpio_port, pin, OUTPUT, OPEN_DRAIN)`, the DQ pin must be open-drain since the DS18B20 can only pull the line LOW, never drive it HIGH, an external ~4.7kΩ pull-up resistor between DQ and VCC does the rest
- `TIM_Init(tim_port, 16, 65535)`, configures the 1MHz tick rate `TIM_Delay_us` needs

**`ow_reset()`, the handshake every single transaction begins with:**
1. Output mode, pull DQ low, hold **480µs minimum** (`tRSTL`)
2. Switch to Input mode, releasing the line (pull-up brings it back HIGH)
3. Wait ~70µs then sample DQ, this falls inside the DS18B20's response window: it waits 15-60µs (`tPDHIGH`) after sensing the release, then pulls the line low itself for 60-240µs (`tPDLOW`) as its presence pulse. Reading LOW here means a device responded
4. Wait out the remainder of **480µs total since release** (`tRSTH`, confirmed from datasheet Figure 15 that this window already contains the presence pulse, it isn't additional time on top)
5. Return whether presence was detected

**`ow_write_bit(bit)`:** every slot ≥60µs (`tSLOT`), plus ≥1µs recovery (`tREC`) after.
- Write 1: pull low ~5µs (must release within `tLOW1` max of 15µs), then release for the remainder of the slot
- Write 0: pull low and hold for the entire slot (`tLOW0`, 60-120µs)

**`ow_read_bit()`:** the DS18B20's response is only valid for 15µs total, measured from when the master first pulls low (`tRDV`), so the initial low-pulse and the release-to-sample gap both need to be as short as possible.
1. Pull low ~1-2µs to initiate the slot
2. Switch to Input mode (release)
3. Wait a few µs, then sample, total elapsed since step 1 must stay under 15µs
4. HIGH sampled = DS18B20 sent a `1` (it just left the line alone). LOW sampled = DS18B20 sent a `0` (it's actively pulling low itself during this slot)
5. Pad out the remainder of the ≥60µs slot before returning

Single sample is sufficient, unlike UART's oversampling: UART is asynchronous (transmitter/receiver clocks are independent, so the receiver has to guess where a bit boundary falls and average out uncertainty). A 1-Wire read slot is master-initiated, the master already knows exactly when the window started because it triggered it, no clock-drift uncertainty to compensate for.

**`ow_write_byte(byte)` / `ow_read_byte()`:** loop 8 times over the bit functions above. All 1-Wire data is transmitted **LSB first** (opposite of how the byte reads left-to-right in binary), so bit 0 goes first, bit 7 last.

Extracting bit `i` from a byte to send:
```c
uint8_t bit = (byte >> i) & 0b1U;   // shift bit i down to position 0, mask off the rest
```
Assembling a byte from bits read back, in the same LSB-first order:
```c
byte |= bit << i;   // shift the just-read bit up to position i, OR it in without disturbing bits already placed
```

---

## DS18B20 (device-specific layer, built on OneWire)

**Command bytes** (from the datasheet's ROM Command and Function Command tables):
```c
#define DS18B20_SKIP_ROM        0xCC // address whatever's on the bus, no 64-bit ROM code needed (single sensor)
#define DS18B20_CONVERT_T       0x44 // start a temperature conversion
#define DS18B20_READ_SCRATCHPAD 0xBE // read back the 9-byte scratchpad (temp data + config + CRC)
```

**Every DS18B20 operation starts with the same sequence** (reset, check presence, Skip ROM), factored into a shared `static` helper so it isn't duplicated across every function:
```c
static uint8_t ds18b20_begin(...) {
    if (!ow_reset(...)) return 0;
    ow_write_byte(..., DS18B20_SKIP_ROM);
    return 1;
}
```
`static` here means private to this file, not declared in `ds18b20.h`, since nothing outside this file should call it directly.

**`ds18b20_start_conversion()`:** `ds18b20_begin()`, then `ow_write_byte(DS18B20_CONVERT_T)`. Doesn't wait for or read the result, just triggers it, the sensor needs up to 750ms (`tCONV` at default 12-bit resolution) afterward before a read will return valid data.

**`tCONV` (conversion time) depends on resolution, which the app layer must wait out between triggering and reading.** The DS18B20 supports 9/10/11/12-bit resolution, higher resolution means finer temperature steps (12-bit gives the full `0.0625°C` step size worked out earlier) but takes longer to measure: roughly 93.75ms / 187.5ms / 375ms / 750ms max respectively. The driver never explicitly sets a resolution, so the sensor defaults to 12-bit on power-up, hence the 750ms wait in `app.c` between `ds18b20_start_conversion()` and `ds18b20_read_temp()`. Reading before the conversion finishes returns stale or garbage data, commonly the chip's power-on default of 85°C rather than a real measurement.

**`ds18b20_read_temp()`:** `ds18b20_begin()`, then `ow_write_byte(DS18B20_READ_SCRATCHPAD)`, then `ow_read_byte()` 9 times for the whole scratchpad (temp LS/MS bytes, `TH`, `TL`, config, 3 reserved bytes, then the chip's own CRC byte).

**Scratchpad CRC check:** 1-Wire is bit-banged over an unshielded, unclocked GPIO line (see the timing notes below) — noise, a delayed sample, or a slow pull-up edge can flip a bit in transit with nothing to catch it. The DS18B20 computes a CRC8 over scratchpad bytes 0-7 and appends it as byte 8 specifically so a reader can verify the transfer; `ds18b20_read_temp()` recomputes the same CRC8 locally (`ow_crc8()`, standard Dallas/Maxim polynomial `x⁸+x⁵+x⁴+1`, reflected form `0x8C`, processed LSB-first per bit — same LSB-first convention as the rest of 1-Wire) and returns failure (0) on a mismatch instead of decoding a possibly-corrupted temperature. `app.c` already treats a 0 return as "read failed" and retries next loop, so this turns a silent bad reading into a skipped one. Previously only bytes 0-1 were read and trusted unconditionally, with no way to tell a corrupted transfer apart from a real (if wrong-looking) temperature.

**Temperature register format:** 16-bit two's complement, LS byte holds bits 2³ down to 2⁻⁴ (4 whole-number bits, 4 fractional bits), MS byte holds 5 sign bits plus bits 2⁶-2⁴. Since the smallest bit is 2⁻⁴ = 1/16, the whole 16-bit value scaled by 1/16 gives the temperature directly:
```c
int16_t temp_byte = (scratchpad[1] << 8) | scratchpad[0];   // int16_t, not uint16_t, so negative values decode correctly via two's complement
float temp = ((float)temp_byte) / 16.0f;
```
Verified against the datasheet's own worked examples (e.g. `0191h` = 401 decimal, `401/16 = 25.0625°C`, matching the table exactly; `FFF8h` as signed int16 = -8, `-8/16 = -0.5°C`, also matching).

---

## SPI

Full-duplex, point-to-point (with a per-device Chip Select line, not addressing like I2C). Simpler at the peripheral/register level than I2C, on purpose: no device addressing protocol, no ACK/NACK concept, no repeated-start/direction-switch complexity, only one status register (`SR`) instead of two (`SR1`/`SR2`). The real protocol complexity for something like an SD card lives one layer up, in the SD-specific command/response scheme built on top of SPI, not in SPI itself.

**Setup (`SPI_Init`, SPI1 on PA5/PA6/PA7 example):**
1. Enable the SPI peripheral clock and GPIO clock
2. Configure SCK/MISO/MOSI pins as alternate function (AF5 for SPI1 on these pins)
3. Configure `CR1`: `MSTR` (bit 2, master mode), baud rate prescaler (bits 5:3, `BR`), leave `CPOL`/`CPHA` at 0 (SPI Mode 0, what SD cards in SPI mode expect), leave 8-bit frame format (default). **Also sets `SSI` (bit 8) and `SSM` (bit 9) to 1** — see the callout below, this isn't optional for this driver's setup.
4. Set `SPE` (bit 6, peripheral enable) **last**, same "enable after everything else is configured" pattern as I2C's `PE`
5. Also calls `TIM_Init(tim_port, 16, 65535)`, same one-time timeout-timer setup pattern as `I2C_Init`/`ow_init`

**Why `SSM`/`SSI` matter here (found the hard way, via real hardware):** SPI has its own `NSS` concept, separate from how this driver actually handles chip-select (a plain GPIO pin, toggled manually via `GPIO_Set` — see the CS section below). By default (`SSM=0`), the peripheral expects the *hardware* `NSS` pin to reflect whether it's currently selected, and uses that to detect a **Mode Fault (MODF)**: if a device configured as SPI master ever sees its `NSS` input pulled low by something else, the hardware assumes another master has taken the bus and automatically clears `MSTR` (kicking the peripheral out of master mode) — a protection meant for multi-master setups. Since this driver's `CS` pin isn't wired to the peripheral's actual `NSS` alternate function at all (it's a bare GPIO), that `NSS` input is left effectively unmanaged, which was enough to trip MODF and silently disable the peripheral moments after `SPI_Init()` returned — before a single byte had been clocked. Symptom: every `SPI_TransferByte()` timed out waiting for `TXE`, even the very first one, with nothing yet touching the actual SD card. Setting `SSM=1` tells the peripheral to stop watching the hardware `NSS` pin altogether and use `SSI` as its internal, software-controlled stand-in instead; setting `SSI=1` then tells it "I am always selected/master," which permanently satisfies that internal check. This is the standard fix any time chip-select is handled by hand instead of the peripheral's dedicated `NSS` pin.

**Baud rate:** a simple prescaler (÷2 through ÷256, powers of 2) dividing the peripheral clock, not a target-frequency calculation like I2C's CCR. At 16MHz: ÷2=8MHz down to ÷256=62.5kHz. SD card initialization specifically requires ≤400kHz (a slow prescaler, e.g. ÷64=250kHz); normal block read/write can go faster once initialized.

**`SPI_TransferByte(byte_in, *byte_out)`:** every SPI transfer is simultaneously send-and-receive (full-duplex, no way to opt out of either direction), so one function does both, unlike I2C's separate `WriteByte`/`ReadByte`. Polls `TXE` (bit 1 of `SR`) before writing to `DR`, then `RXNE` (bit 0) before reading `DR` back. Uses the same `tim_port`-based timeout pattern as I2C (`SPI_TIMEOUT_US`, same 5ms value as `I2C_TIMEOUT_US`), mainly as a defensive check against a software misconfiguration (SPI never enabled, wrong port) rather than an external-device failure mode, since a correctly-initialized SPI transfer is deterministic and can't really "not respond" the way an unaddressed/unresponsive I2C device can. (`SPI_TIMEOUT_US` is `#define`'d in `spi.c` the same way `I2C_TIMEOUT_US` is in `i2c.c` — it was referenced in the timeout checks but never actually defined for a while, which would have failed to compile.)

**Chip Select (CS):** a plain GPIO output pin, not part of the SPI peripheral itself, driven with `GPIO_Set()`. Active-low by convention. Whether a given function handles CS internally (select, transact, deselect, all inside one call) or leaves it to the caller is a per-function design choice, not something SPI enforces, this project chose "handled internally" as the default, with the SD block read/write functions as a deliberate exception (see below) since they need CS held across multiple phases (command, then data) that can't be split into two separate `sd_send_command()`-style calls.

---

## SD card over SPI (device-specific layer, built on SPI)

The WeAct core board used for this project has an onboard MicroSD slot, but it's wired to **SDIO** (`CMD`, `CLK`, `DAT0-3` pins), a different, more complex peripheral (command/response state machine, typically DMA-driven) than SPI. Deliberately not used for v1, not worth the schedule risk of learning a second, bigger protocol before the pilot deadline. A separate SPI SD breakout is wired to different pins instead; the onboard SDIO pins are simply left unused. Logged as a v2 upgrade.

**The SD card has its own command/response language layered on top of SPI.** SPI is just the wire protocol (how bits move); the SD card's controller chip expects a specific, structured sequence of commands and replies in a particular format, defined by the SD Association's Physical Layer Specification (a spec that's traditionally hard to access publicly, unlike the DS18B20/DS3231 datasheets, community references like elm-chan.org's "How to Use MMC/SDC" and rjhcoding.com's AVR SD tutorial series were used to cross-check values instead).

**Command frame format — every command is a fixed 6-byte frame:**
```
Byte 0:   0x40 | cmd_number   (bit7=0 start bit, bit6=1 transmission bit, bits5:0=command number)
Byte 1-4: 32-bit argument, MSB first
Byte 5:   CRC7 (7 bits) + stop bit (bit 0, always 1)
```
CRC is only actually *checked* by the card for CMD0 and CMD8 in SPI mode; since those two commands always use the same fixed argument in this driver, their correct CRC7 output is also always the same fixed value, so it's hardcoded (`0x95` for CMD0, `0x87` for CMD8) rather than computed at runtime. Every other command uses a dummy CRC byte (`0x01`, satisfying only the required stop bit) since CRC checking is off for them by default in SPI mode.

**Response formats — "R" + a number is just a label for a response's byte-length and meaning, not a different mechanism.** Every response arrives the same way electrically (keep clocking dummy `0xFF` bytes and reading MISO until a valid byte shows up, or give up after some max tries), the R-number just tells you in advance how many bytes to expect and how to interpret them.
- **R1** (1 byte): a set of error-flag bits. Bit 7 always 0 (this is what distinguishes a real response from line-idle `0xFF`, where bit 7 is 1). Bit 0 = "still in idle state." Used by CMD0, CMD55, ACMD41.
- **R7** (R1 + 4 more bytes): used by CMD8. The trailing bytes carry the voltage-acceptance flag and an echo of the check pattern sent in the command.
- **R3** (same shape as R7, 1+4 bytes): used by CMD58. The trailing bytes are the OCR (Operating Conditions Register), not the same information as R7's despite the identical byte count.

**`sd_send_command()` handles both R1-only and R3/R7-shaped commands** via a `has_extra` flag and an output buffer, rather than two separate near-duplicate functions:
1. Select CS (low)
2. Send the 6-byte frame
3. Poll for R1 (up to `SD_RESPONSE_TIMEOUT_TRIES`, checking bit 7 clear), starting `response` at `0xFF` since that value is otherwise impossible for a real response (bit 7 must be 0) and so doubles as a safe "nothing received yet" sentinel
4. If `has_extra`, read 4 more bytes into the caller's buffer, **before** releasing CS (this is why R3/R7 handling can't be bolted onto `sd_send_command()`'s CS-release timing after the fact, the card would already be deselected)
5. Deselect CS (high), return the R1 byte

**Init sequence (`sd_init`), each step's purpose:**
1. **≥74 dummy clock pulses**, CS high, MOSI high (`0xFF` × 10 bytes ≈ 80 clocks) — lets the card's internal power supply stabilize before any real command; a documented SD-spec requirement, not optional
2. **CMD0** (GO_IDLE_STATE, arg=0): hard reset. Also what forces the card out of its native SDIO-style protocol into SPI mode, sending it while CS is low is specifically what triggers that switch. Expect R1=`0x01`
3. **CMD8** (SEND_IF_COND, arg=`0x000001AA`): asks "what voltage do you support" and, implicitly, "do you even understand this command" — CMD8 postdates SD 1.x, so an old card/MMC card simply won't recognize it, making it a de facto version-detection mechanism. The low byte of the argument (`0xAA`) is an arbitrary check pattern the card must echo back exactly in its response, confirming the exchange genuinely worked rather than trusting a coincidentally-valid-looking reply. Only SD 2.0+ cards are handled by this driver (rejection on CMD8 = bail out)
4. **CMD55 + CMD41 (as "ACMD41"), looped:** CMD55 is a required "the next command is application-specific" flag, sent immediately before every single ACMD, every time, it is not remembered across commands. ACMD41's argument sets bit 30 (HCS, Host Capacity Support) telling the card the host can handle SDHC/SDXC media. The card's real initialization is a physical process taking variable time, so this pair is resent in a loop until the response goes from `0x01` (still busy) to `0x00` (ready)
5. **CMD58** (READ_OCR, arg=0), sent only after the ACMD41 loop succeeds (the OCR's capacity info isn't meaningful before that): reads the OCR register. Bit 30 of the OCR (bit 6 of the first response byte) is CCS (Card Capacity Status), 0=SDSC (byte-addressed), 1=SDHC/SDXC (block-addressed, fixed 512-byte blocks). Output via the `is_sdhc` pointer parameter **and** cached in a module-static `sd_card_is_sdhc` flag, since `sd_read_block()`/`sd_write_block()` need to know this too and don't take an `is_sdhc` parameter of their own

**Block read (`sd_read_block`, CMD17) and write (`sd_write_block`, CMD24):** unlike `sd_send_command()`, these can't release CS after the R1 response, since the actual 512-byte data phase still needs to happen with the card selected — so they inline their own copy of the 6-byte command-frame-sending logic rather than reusing `sd_send_command()`, a deliberate, necessary exception to reuse given the CS-timing requirement.

**Address translation (SDHC vs SDSC):** both functions take `addr` as a block number, then convert it before building the command frame: `card_addr = sd_card_is_sdhc ? addr : (addr * SD_BLOCK_SIZE)`. SDHC/SDXC cards want the raw block number; SDSC cards want a byte offset, so the block number gets scaled by 512 first. This has to happen per-call rather than once, since the caller (`log.c`) always thinks in block numbers regardless of card type.

**Every `SPI_TransferByte()` call in this file is wrapped through a small `sd_xfer()` helper** that checks the return value and, on a timeout, deselects CS and reports failure up the call chain. This used to not be the case — every raw `SPI_TransferByte()` call in `sd_send_command()`/`sd_init()`/`sd_read_block()`/`sd_write_block()` ignored its return value, so a SPI-level timeout mid-transfer (as opposed to a card-level command rejection or missing response token, which *were* already checked) would silently be treated as if the byte had transferred fine.

Read sequence: send CMD17 frame → poll R1 (expect `0x00`) → poll for the **data start token** (`0xFE`, sent by the card once it's actually retrieved the data from flash, needs many more retries than a command response, `SD_DATA_TIMEOUT_TRIES`=500 rather than the 8 used for NCR, since the card is doing real physical flash-read work in the meantime) → read 512 bytes into the caller's buffer → read and discard 2 trailing CRC bytes → deselect CS.

Write sequence: send CMD24 frame → poll R1 → send the data start token yourself (`0xFE`, reversed from the read case, now the host signals "here comes the data") → send 512 bytes from the caller's buffer → send 2 dummy CRC bytes → poll for the **data response token**, masking the low 5 bits (`SD_DATA_ACCEPTED_MASK = 0x1F`) and comparing against `SD_DATA_ACCEPTED_VALUE = 0x05` (a non-match, including the line-idle `0xFF` case, correctly keeps the poll loop going rather than false-accepting) → then a **separate** busy-wait: after accepting the data, the card needs real time to physically commit it to flash, holding the SPI line at `0x00` the whole time; keep clocking `0xFF` until a non-zero byte comes back, using a `finished` flag checked after the loop so a write that never actually completes is correctly reported as a failure rather than being silently claimed as a success just because the loop's fixed iteration count ran out.

Both explicitly track success/failure through every phase (command rejected, data/token timeout, busy-wait timeout) and always deselect CS before returning on any path, mirroring the same "bail out and clean up, never leave the bus/line in an unknown state" discipline used throughout the I2C driver's higher-level functions.

---

## SSD1306 OLED (device-specific layer, built on I2C)

128x64 monochrome OLED, address `0x3C`, driven over I2C rather than the SPI variant some breakouts offer. Two files: `ssd1306.c/h` (hardware I/O + framebuffer + drawing primitives) and `oled_fonts.c/h` (raw bitmap data only, no logic) — kept separate so the bitmap tables (large, rarely-touched) don't clutter the driver logic they're used by.

**Command vs. data framing is just I2C's normal register-address byte, repurposed:** the SSD1306 doesn't have "registers" in the addressable sense DS3231/DS18B20 do — instead, every I2C write starts with one **control byte** that tells the chip whether the bytes that follow are commands (`0x00`) or GDDRAM pixel data (`0x40`). That's structurally identical to "register address, then value(s)" (`I2C_WriteBytes(dev_addr, reg_addr, data, len)`), so no new I2C-level code was needed at all:
```c
uint8_t ssd1306_write_command(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t cmd) {
    return I2C_WriteBytes(port, tim_port, SSD1306_ADDR, SSD1306_CMD_MODE, &cmd, 1);
}
uint8_t ssd1306_write_data(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t *data, uint8_t len) {
    return I2C_WriteBytes(port, tim_port, SSD1306_ADDR, SSD1306_DATA_MODE, data, len);
}
```
(These two were declared in `ssd1306.h` from the start but had no matching definitions for a while — an easy thing to miss since the header compiles fine on its own; it only surfaces as an `undefined reference` at the final link step, once something actually calls them.)

**`ssd1306_init()`** is a fixed command sequence straight from the datasheet's recommended init procedure: charge pump on (`0x8D`/`0x14`, since this board doesn't wire an external VCC for the panel), MUX ratio 63 (`0xA8`/`0x3F`, i.e. 64 rows), zero display offset/start line, segment remap + COM scan direction flipped (`0xA1`/`0xC8`, matches how these particular panels are usually wired internally so the image comes out right-side-up instead of mirrored/upside-down), contrast, normal (non-inverted, non-all-on) display, oscillator frequency, COM pin hardware config (`0xDA`/`0x12`, the standard value for a 128x64 panel), horizontal addressing mode (`0x20`/`0x00`), then display on (`0xAF`) last.

**Framebuffer layout matches the panel's own GDDRAM layout exactly, on purpose:** `OLED_FB` is a flat `1024`-byte array (`128 × 64 / 8`), organized the same way the SSD1306's internal memory is — 8 "pages" of 128 columns each, one byte per column holding 8 vertically-stacked pixels (bit 0 = top row of that page, bit 7 = bottom):
```c
int page = y / 8;
int byte_index = x + (page * OLED_WIDTH);
uint8_t bit_mask = (1 << (y % 8));
```
Because of that match, `oled_update()` (with horizontal addressing mode set in `ssd1306_init()`) can push the whole framebuffer to the panel as one flat sequential dump — set the column range to 0-127 and the page range to 0-7, then stream all 1024 bytes as 8 chunks of 128 (chunked only because `ssd1306_write_data()`'s `len` parameter is a `uint8_t`, so it can't take more than 255 at once) — no per-pixel translation needed between "framebuffer" and "what the panel expects."

**Three different bitmap encodings exist across the icons/fonts, because they were authored for different purposes, not by a consistent convention:**
- **Icons** (`OLED_Icon_Wifi/SD/Refresh`, 14x11, 28 bytes each): page-major, same layout as `OLED_FB` itself (byte = one column of one page, bit = row within that page). Chosen specifically so `oled_draw_icon()` is nearly a direct copy into the framebuffer — the exact same page/bit math as `oled_set_pixel()`, just walking the icon's own byte array as the source instead of a single set/clear.
- **Small digits/colon** (`OLED_SmallDigits[10][11]`, `OLED_SmallColon[11]`, 8x11): row-major, **one byte per row**, bit = column within that row.
- **"Big" digits and minus** (`OLED_BigDigits[10][108]`, `OLED_BigMinus[108]`, 18x36 real content in a 3-bytes/row = 24-bit-wide cell): also row-major, same LSB-first bit order as the small font, just 3 bytes per row instead of 1 (`byte = glyph[row*3 + col/8]`, `bit = col%8`). Extracted from a full `LOCHAR..HICHAR` ASCII bitmap font table (a classic GLCD-style font export, `FONT_BWIDTH=3`/`FONT_HEIGHT=36`) — only the `'0'`-`'9'` and `'-'` cells (indices 16-25 and 13, i.e. `ASCII - 32`) were pulled out, since nothing else in that table is ever drawn here. `OLED_BigC`/`OLED_BigDegree`/`OLED_BigPeriod` are still older, unfinished placeholder data (only an 8px-wide strip populated) and remain unused.

**Row-major bit order — LSB-first, bit 0 = leftmost column — the opposite of how a byte reads left-to-right in binary, and initially implemented backwards.** `oled_draw_small_glyph()` originally read `(bits >> (7 - col))`, i.e. MSB-first, which mirrors every glyph horizontally. This went unnoticed until the first real hardware test, where every digit was legible but flipped left-right, most obviously on `1` (whose top serif normally flares toward the left as you go down; MSB-first made it flare right instead). Fixed to `(bits >> col)`. Worth remembering if any new row-major glyph data is ever hand-authored or imported: there's no compiler check for this, only "does it look right on the actual panel."

**Icons are unaffected by that bug** — their page-major format extracts a *row within a page* from `bit`, not a *column within a row*, so column order was never ambiguous there in the first place. This is exactly why the icon and small-glyph formats needed separate draw functions (`oled_draw_icon()` vs `oled_draw_small_glyph()`) rather than one generic blitter: the two bit layouts aren't interchangeable.

**Big (enlarged) temperature digits are drawn straight from `OLED_BigDigits`/`OLED_BigMinus` — this took three attempts to land on.** First pass scaled the small 8x11 font up in software (nearest-neighbor block-fill per bit) as a stopgap around `OLED_BigDigits`' original unfinished data. Second pass filled `OLED_BigDigits` in with real digit art rendered from Arial Bold via a script, thresholded to 1-bit at 24x36 — but that font rasterized rough on the actual panel: bold-face curves and diagonals that look clean in a preview don't survive a hard 1-bit threshold as cleanly as a font actually designed for small pixel grids. Third pass replaced that data with real digits extracted from an existing pre-rendered 18x36 pixel font instead of a rasterized TrueType face — that font's shapes hold up fine, but drawing the *full* padded 18x36 cell (as delivered) surfaced two more bugs only visible once actually flashed and photographed: the degree mark, positioned at the cell's top edge, floated well above where the digit strokes actually start; and the gap between digits looked much wider than `BIG_GAP` alone should produce.

**Root cause of both: the source font's cells have a lot of blank padding baked in.** Measuring the actual data directly (not assuming) showed every digit's ink only occupies rows 20-35 and columns 1-11 of the nominal 18x36 cell — the rest is headroom for characters with ascenders/descenders that digits don't have. Fixed by cropping to that measured ink box and upscaling the crop 2x, rather than drawing the full padded cell at 1x:
```c
#define BIG_ROW_OFF 20
#define BIG_ROW_H   16
#define BIG_COL_OFF 1
#define BIG_COL_W   11
#define BIG_SCALE   2

static void oled_draw_big_glyph_region(int x, int y, const uint8_t *glyph, int row_off, int row_h) {
    for (int row = 0; row < row_h; row++) {
        for (int col = 0; col < BIG_COL_W; col++) {
            uint8_t byte = glyph[(row_off + row) * BIG_BYTES_PER_ROW + (BIG_COL_OFF + col) / 8];
            if (!((byte >> ((BIG_COL_OFF + col) % 8)) & 0x1)) continue;
            for (int sy = 0; sy < BIG_SCALE; sy++)
                for (int sx = 0; sx < BIG_SCALE; sx++)
                    oled_set_pixel(x + col * BIG_SCALE + sx, y + row * BIG_SCALE + sy, 1);
        }
    }
}
```
`BIG_GLYPH_W`/`BIG_GLYPH_H` (used everywhere else — layout math, vertical centering, point/degree placement) are now `BIG_COL_W * BIG_SCALE` / `BIG_ROW_H * BIG_SCALE` — i.e. they describe the actual visible digit size (22x32), not the padded source cell. This is what fixes the degree mark: placed at `y` (the digit box top), it now lines up with the top of the crop, which *is* where the ink starts, instead of the top of 20 blank rows above it. It also fixes the spacing: two digit boxes are now genuinely 22px of real ink each, so `BIG_GAP` between them is the actual visible gap rather than `BIG_GAP` plus however much blank cell-padding was left over on each side.

**Minus needed its own vertical offset once digits were cropped, since it isn't in the same row range.** Minus's own ink sits at rows 27-29 in the source font — 7 rows into the digits' 20-35 ink window, which is why the two lined up automatically before (same 36-row cell, no cropping). Once digits crop out everything before row 20, minus needs to be drawn 7 scaled rows down from the digit box's top to preserve that same relative position:
```c
#define MINUS_ROW_OFF 27
#define MINUS_Y_OFFSET ((MINUS_ROW_OFF - BIG_ROW_OFF) * BIG_SCALE)
```

**18px-wide real glyphs (now cropped+2x to 22px effective) fit the full "sign + int digits + point + 1 fraction digit + degree" layout with real margin (worst case ~90px of 128), which is also why 1 decimal place was chosen over 2 in the first place** — a sign + 3 integer digits + point + 2 fraction digits + degree mark, at any font source, is 6+ elements that don't fit on a 128px panel at a legible size. The realistic worst cases (DS18B20 range -55°C to +125°C — a negative reading is at most 2 integer digits, a 3-digit reading is never negative) both land comfortably under 100px at 1 decimal place.

**Degree mark is a small hand-drawn 6x6 ring, not the square-outline loop used before.** A ring built from four straight `for` loops (top/bottom/left/right edges of a bounding box) reads as a square, not a degree sign, at any size — there's no amount of scaling that fixes a fundamentally square shape. Replaced with an explicit `uint8_t ring[6][6]` mask (plain 0/1 per pixel, not packed bits — not worth the bit-packing complexity for something this small and one-off) with the corners left unset, giving an actual rounded-looking ring:
```c
static const uint8_t ring[DEGREE_SIZE][DEGREE_SIZE] = {
    {0, 1, 1, 1, 1, 0},
    {1, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1},
    {0, 1, 1, 1, 1, 0},
};
```
The decimal point stays a small procedural filled square rather than a font glyph — a period is a single dot, not worth its own bitmap entry.

**`oled_draw_status_screen()` lays out the whole display in one pass, then leaves pushing it to the panel (`oled_update()`) to the caller** — same separation as `oled_clear()`/`oled_set_pixel()` already have, so multiple draw calls can build up one frame before a single I2C transfer sends it. Layout, top to bottom:
- **Top-left:** wifi icon, then SD icon 2px to its right — each drawn only when its own "ready" flag is true, so e.g. no SD card just means blank space there rather than a dimmed/placeholder icon. Wifi is currently **always** hidden (`wifi_connected` hardcoded `0` in `app.c`) since no wifi hardware/driver exists yet on this project — a stub, not a bug, waiting on real hardware.
- **Top-right:** refresh icon, right-aligned to the panel edge, with the `HH:MM` of the last *successful* SD write drawn to its left in the small font — only drawn once at least one write has actually succeeded (`have_refresh`), so a fresh boot with no writes yet just shows the icon alone rather than a misleading `00:00`.
- **Center:** the big temperature reading described above, horizontally centered based on the actual measured width of whatever's being drawn (varies with digit count and sign), vertically centered against the big-digit height.

**Wiring note — why the OLED ended up on `I2C3` instead of sharing the RTC's `I2C1` bus:** electrically nothing stops two I2C devices at different addresses from sharing one bus (that's the whole point of I2C), and that was the original plan. It moved to its own bus for a simpler reason: separate pins are easier to probe/debug independently on the bench, and once `I2C2`'s default SDA pin (`PB11`) turned out not to be broken out on this Nucleo-64 board's headers at all, `I2C3` on `PA8`/`PC9` was used instead (see the I2C section's multi-instance note above). `OLED_TIM` is still set to the *same* timer the RTC's `I2C1` uses (`RTC_TIM`/`TIM3`) rather than a fourth dedicated timer — safe because this timer is only ever used as a disposable microsecond stopwatch for timeout detection (reset-start-poll-stop, see the I2C timeout pattern), never left running or relied on to hold state between calls, and `app.c`'s I2C transactions are always sequential (one blocking call finishes completely before the next starts), never concurrent, so there's no possibility of two timeout checks racing on the same counter.

**Build-system gotcha, not a driver bug:** the first time `oled_fonts.c` was added to the `BareMetalDrivers` submodule, the STM32CubeIDE-generated `Debug/` build files (`BareMetalDrivers/Src/subdir.mk`, `Debug/objects.list`) still only listed the files that existed when they were last generated — so the new file silently wasn't compiled/linked in at all, producing `undefined reference` errors for every symbol in it (`OLED_Icon_Wifi`, `OLED_SmallDigits`, etc.) despite the source itself being correct. STM32CubeIDE normally regenerates these automatically, but only once it notices the new file — if a driver file is added outside the IDE (e.g. via git, like this submodule), do a **Project → Refresh (F5)** before the next build so the IDE's indexer picks it up and rewrites its own copy of these files.

---

## ESP8266 (WiFi, AT-command layer, built on UART)

> **Historical.** This section describes the first version of the driver (one `esp_wait_response` that used `strstr`, and a single `esp_send_reading`). The current behaviour is in the README: per-byte `strstr` was replaced by a constant-cost tail match because it overran the 115200-baud UART, and the driver gained an idle hook, join/connect result handling, `esp_send_hourly`, and `esp_sync_time` with the server's config headers. The bring-up story at the end of this section is still accurate.

The ESP-01S isn't driven at the register/protocol level like the other peripherals in this repo — it runs its own AT-command firmware over UART, so this driver's job is building/sending the right ASCII command strings and pattern-matching the module's ASCII replies, not bit-banging anything. `esp_send_command`, `esp_wait_response`, `esp_connect_wifi`, `esp_send_reading`.

**`esp_send_command`** just wraps `UART_SendString` with a trailing `"\r\n"` appended — every AT command needs that terminator, so it's centralized here instead of repeated at every call site.

**`esp_wait_response`** is a blocking, timeout-guarded read loop: pull characters in via `UART_DataAvailable`/`UART_ReceiveChar`, append them to a growing buffer (capped, null-terminated after every char so `strstr` always has a valid string to search), and return success the moment `expected` appears anywhere in what's been received so far. Same timeout pattern as I2C/SPI/SD's timeout-guarded calls elsewhere in this repo — reset a dedicated timer's `CNT`, start it, poll, bail out past `timeout_ms`.

**`esp_connect_wifi`** is just two AT commands in sequence, each gated on the previous one succeeding: `AT+CWMODE=1` (station mode) waited on for `"OK"`, then `AT+CWJAP="ssid","password"` waited on for `"OK"` with its own longer timeout (`ESP_WIFI_TIMEOUT_MS` — actually joining a network takes much longer than a local mode-set command).

**`esp_send_reading` builds a full raw HTTP/1.1 POST request as one string, then pushes it through the AT firmware's "transparent transmission" mode:**
1. Build the JSON body first (`{"temp":...,"door":...,"timestamp":...}`) — needed before the HTTP header can be built, since `Content-Length` has to be the body's exact byte count
2. Build the full request text (request line + headers + blank line separator + body) into one buffer via `snprintf`, capturing its length
3. `AT+CIPSTART="TCP","host",80` — opens a raw TCP socket, wait for `"OK"`
4. `AT+CIPSEND=<n>` — tells the module "exactly `n` raw bytes are coming next," wait for the `">"` prompt
5. Send the request bytes via `UART_SendString` — **not** `esp_send_command`, since that appends `"\r\n"`, which would be 2 extra bytes injected into a stream the module was told to expect an exact byte count for
6. Wait for `"SEND OK"`

**`AT+CIPSEND`'s `">"` prompt is a genuine mode switch, not just an echo.** Once it appears, everything sent over UART stops being parsed as AT commands and is instead forwarded byte-for-byte, raw, straight onto the open TCP socket until the module has counted exactly the number of bytes declared in `AT+CIPSEND=<n>`. This is why step 5 has to bypass `esp_send_command`'s trailing `\r\n` — those two bytes would silently become part of the HTTP payload sent over the wire, not part of AT command framing.

**Known truncation risk, not yet guarded against:** `request_len` is captured from `snprintf`'s return value, which per the C standard is the length that *would* have been written if the buffer were infinite — not the number of bytes actually written. If the request ever exceeds `sizeof(request)` (currently 384 bytes), `request_len` reports a bigger number than what's actually sitting in the buffer; `AT+CIPSEND=<request_len>` then promises the module more bytes than `UART_SendString` (which stops at the null terminator) actually sends, and the module sits waiting for the shortfall until `ESP_TIMEOUT_MS` expires — a timeout failure with no obvious connection to its real cause (buffer sizing, not networking). Not hit yet at current host/JSON lengths, but worth either sizing `request` with real margin or adding an explicit `request_len >= (int)sizeof(request)` bailout before trusting it.

**Why `esp_send_reading` needed its own float-to-string function (`format_float`) instead of `%.2f`:** newlib links a small printf variant by default that doesn't understand `%f`/`%e`/`%g` at all — using one anywhere in the project produces a `-u _printf_float` linker warning and silently-wrong output, not a build error. Pulling in the full float-capable printf is a one-line linker fix (`-u _printf_float`) and is the right call for most projects; `format_float` exists here as the alternative that avoids that dependency entirely, matching this repo's general habit of deriving things from primitives (see `UART_Init`'s manual mantissa/fraction split for `BRR`, which is the same kind of problem — separating a float into integer pieces by hand — for the same reason: keep peripheral-adjacent code off the float-printf path).

**`format_float(buf, buf_size, val, decimals)` — scale-to-integer, then build digits by hand:**
1. Record the sign, then work with the positive magnitude for the rest of the function
2. Multiply by `10^decimals` (built via a small loop, `scale *= 10.0f` per decimal place, rather than pulling in `pow()`) and round by adding `0.5` before truncating to `uint32_t` — casting float→int always truncates in C, so `+0.5` before the cast is what turns truncation into round-to-nearest for a positive value (this is exactly why sign-handling has to happen first: the trick only rounds correctly on positive magnitudes)
3. Split the now-integer `scaled` value into `int_part`/`frac_part` via plain `/` and `%` against `10^decimals`
4. Build `frac_part`'s digits into a small `char` buffer **from the last index backward** (`% 10` peels off the last digit, `/ 10` drops it, repeat) — filling right-to-left is what preserves leading zeros (`frac_part=5` at `decimals=2` correctly yields `"05"`, not `"5"`), which a plain `%u` on `frac_part` would get wrong
5. Assemble the final string with `snprintf(buf, buf_size, "%s%lu.%s", sign, int_part, frac_str)` — only `%s`/`%lu` here, deliberately, so the function itself never touches the float-printf path it exists to avoid

`buf_size` is threaded through to `snprintf` for the same reason any caller-buffer-writing function needs its destination's real size: without it there's nothing stopping a longer-than-expected result (e.g. a 3-digit integer part) from writing past the end of a too-small caller buffer. `esp_send_reading` calls this with `sizeof(formattedTemp)` on a 16-byte local array — comfortable margin even at the DS18B20's most extreme values (`-55.00` to `125.00`).

**Bugs found getting this to actually compile and link:** `esp.h` existed as an empty file for a while (declarations for everything above lived only in `esp.c`, nothing `#include`d it), so nothing calling into this driver would even build. Also `esp_send_reading` originally declared `formattedTemp` as a single `char` instead of a buffer (`format_float` writing a multi-character string through a one-byte pointer — real stack corruption, not just a compile error) — fixed alongside the header by giving it the 16-byte array `format_float`'s own doc comment above already assumed existed. Neither was caught until something actually tried to build and call this file, which is why `app.c`'s ESP wiring is worth checking against the header/source in place, not just this writeup, if either changes again.

### ESP8266 hardware bring-up: the full debugging saga

Getting a physical ESP-01S actually talking to the STM32 took far longer than writing the driver itself, and almost none of the early "results" turned out to mean what they first looked like. Written up in full because most of these are generic ESP8266/breadboard gotchas, not specific to this one board.

**No debug UART exists once the ESP is wired up.** USART1 (or whichever UART the ESP ends up on) is dedicated entirely to the ESP's AT command channel — anything else writing to it corrupts AT commands/responses in flight. That ruled out the usual "just print it" approach for every step below. The workaround: a temporary debug helper that sent one AT command and displayed the raw response on the OLED as `<byte count>:<last received byte's ASCII value, split across two rows since it can be 0-255>`, using the small-font time-digit glyphs as ad hoc numeric readout since no full ASCII font exists yet. This lived in `app.c` only, never in this driver, and was removed once the link was confirmed working — but the technique itself (encode whatever you need as digits, dump it via `oled_draw_small_time`) is worth remembering for the next headless bring-up problem.

**Symptom 1 — module visibly powered (broadcasting its own default AP) but zero UART response.** Worked backward from "is the chip even alive" (yes, per the AP) to "is anything reaching either UART pin." Root cause was banal: `esp.h` was still empty (see above) and USART1 was double-booked as both the old debug UART and the ESP's link — moving the ESP to its own pins (eventually UART4/PC10-PC11) and dedicating that UART fully to it was step one, before any of the rest of this made sense to chase.

**Symptom 2 — a UART peripheral vs. pin mismatch that looked like a hardware fault.** At one point the ESP was wired to PC10/PC11 while the code still called `UART_Init(USART3, ...)`. In this driver, USART3 is PB10/PB11 — PC10/PC11 is UART4. Since `UART_Init` only configures the alternate-function pins for whichever peripheral you actually pass it, wiring to PC10/PC11 while initializing USART3 leaves PC10/PC11 completely unconfigured; nothing was wrong with the ESP or the wiring, the code just wasn't talking to the pins that were physically connected. Caught by literally tracing "which pin, to which peripheral" against `uart.c`'s own pin-map comment rather than trusting a verbal "I moved it to USART3."

**Symptom 3 — random, differently-garbled bytes on every single reset, even with wiring/pins confirmed correct.** This consumed the most time and turned out to have three independent, stacked causes:

1. **Missing common ground.** The ESP was eventually moved onto its own external 3.3V source (a second board's onboard regulator, and briefly batteries) to rule out the STM32's own rail being underpowered. But its GND was never actually tied back to the main STM32's GND for a while. UART only means anything relative to a shared ground reference — without one, the receiver can trigger on coincidental voltage-crossing artifacts between the two floating grounds and "decode" something that looks like a real, even *repeatable*, byte sequence, despite carrying zero actual information. This produced the single most misleading result of the whole bring-up: sending three completely different AT commands (`AT`, `AT+CWMODE=1`, and a full `AT+CWJAP="ssid","password"`) all "replied" with the exact same few bytes — which is only possible if what's being received has nothing to do with what was sent. Tying the external supply's GND to the main board's GND immediately changed the readout (to a clean, honest `00:00`), confirming everything captured before that point was noise, not signal.
2. **A capture-loop bug: polling with a sleep between reads.** The very first version of the debug capture loop slept 5ms between checking the UART for new data. A byte transmits in about 1ms at these bauds, and the STM32's USART data register has no FIFO (holds exactly one byte) — so any byte that arrived during that 5ms nap got silently overwritten by the next one before software ever read it. Fixed by polling as fast as the CPU can in a tight loop, using a widened 32-bit timer's raw `CNT` as the deadline instead of `TIM_Delay_ms` between checks.
3. **A second, subtler capture-loop bug: sending fully before ever starting to listen.** Even after fixing (2), a byte-perfect loopback test (STM32's own TX jumpered straight to its own RX, no ESP involved at all) showed every command — 4 bytes, 13 bytes, 54 bytes, wildly different lengths — coming back as the *same* short 2-3 byte tail. Cause: `esp_send_command`/`UART_SendString` blocks until the whole string is queued for transmission before returning, and nothing drains the receive register during that time. On a fast loopback (or a fast real reply), earlier bytes arrive and get overwritten while still mid-send, so only whatever's left over from the last character or two survives by the time the capture loop finally starts. Fixed by interleaving transmission and reception — sending one character at a time, draining any pending received byte between each one, rather than treating send and receive as two separate sequential blocking steps. Confirmed fixed by re-running the same loopback test and getting byte-perfect, length-correct results for every command (`04:xx` for a 4-character send, `13:xx` for 13 characters, `54:xx` for 54 characters).

Once both the ground and both capture bugs were fixed, the "random garbage" symptom was gone entirely — every prior random-looking result across dozens of reset cycles had been either a floating-ground artifact or a self-inflicted capture bug, not evidence of a flaky ESP or a flaky connection.

**Symptom 4 — the ESP8266 boot-time dual-baud quirk.** Independent of the above: right after power-up, the ESP's ROM bootloader dumps diagnostic text at a fixed 74880 baud, before its firmware takes over at whatever baud is actually configured. Talking to it (or listening) during that window decodes as garbage regardless of what baud is assumed, since it's a real, different baud. Fixed with a 2-second delay (`TIM_Delay_ms`, not `HAL_Delay` — this project has no HAL) at the very start of `App_Init`, before `ESP_UART` is ever touched, using `DELAY_TIM` initialized first specifically so this wait can start as early in boot as possible. Not the root cause of the main symptom above (removing this delay entirely didn't reintroduce the randomness once ground/capture were fixed), but a real, separate gotcha worth keeping regardless.

**Power: current spikes, not a fixed brownout.** ESP8266 modules draw short current spikes (~200-300mA) during wifi transmit that a small shared linear regulator — especially on a breadboard, with a breadboard's own parasitic resistance/inductance on top — often can't supply fast enough, causing exactly the kind of instability this whole saga chased for a while (before the ground/capture bugs were found and shown to be the actual explanation for most of it). The standard fix, for whenever real RF activity under marginal power resurfaces this: a 100-470µF electrolytic capacitor directly across the ESP's VCC/GND pins, as physically close to the module as possible — this supplies the fast transient a regulator or even a battery pack can't always react to instantly on its own. Not strictly needed once ground/capture bugs were fixed for *this* symptom, but worth having on hand before adding heavier duty cycles (e.g. sending on every loop iteration) later.

**Every boot-mode/enable pin needs to sit solidly at the same voltage as VCC.** `EN`/`CH_PD`, `RST`, `IO0` (GPIO0), and `IO2` (GPIO2) all need to be HIGH for the ESP to boot into its normal AT-firmware run mode rather than staying in reset or entering the UART bootloader. All five (including VCC itself) should come from the *same* supply rail — mixing, e.g., VCC from a battery pack with EN from the STM32's own 3.3V, puts a control pin's voltage slightly above the chip's own VCC, which can forward-bias internal ESD/clamp diodes and cause its own subtle instability. Verified with a multimeter, one pin at a time, during bring-up — all five reading a solid 3.3V relative to GND was the confirmation that nothing electrical was left unaccounted for before concluding the remaining symptom (at the time) was software/config, not wiring.

**Final working configuration:** ESP8266 on UART4 (PC10=TX, PC11=RX), `ESP_BAUD` = 115200 (the module's actual AT-firmware baud all along — every earlier "wrong baud" reading was actually the ground/capture bugs above, never resolved by trying other baud rates, only by fixing those two things and then finding 115200 genuinely worked). Confirmed end-to-end by real `AT`/`AT+CWMODE=1`/`AT+CWJAP=...` responses matching their expected byte-for-byte lengths (echo + `OK`), and the OLED's wifi icon lighting up from a real `esp_connect_wifi()` success rather than a hardcoded placeholder.

**The general lesson, if this happens again on different hardware:** a loopback test (jumper the UART's own TX directly to its own RX, target device fully disconnected) is the cheapest way to prove or disprove that the microcontroller side — baud config, pin/peripheral mapping, and any custom capture code — is correct, *before* spending more time suspecting the external device or its wiring. Every real bug in this saga except the boot-time quirk was eventually isolated this way; every red herring (the missing ground, both capture-loop bugs) looked exactly like "the ESP is flaky" until a loopback test proved the STM32 side was the thing actually lying.



