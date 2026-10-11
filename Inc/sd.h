#ifndef INC_SD_H_
#define INC_SD_H_

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

#define SD_RESPONSE_TIMEOUT_TRIES 8U    // NCR is 1-8 bytes
#define SD_DATA_TIMEOUT_TRIES 500U      // write data-response token
#define SD_READ_TIMEOUT_TRIES 20000U    // read data token (spec: up to 100 ms)
#define SD_BUSY_TIMEOUT_TRIES 100000U   // card busy after a write (spec: up to 250 ms)

// CMD0 - GO_IDLE_STATE: reset the card into SPI mode
#define SD_CMD0      0
#define SD_CMD0_ARG  0x00000000U // no parameters needed for a reset
#define SD_CMD0_CRC  0x95U       // fixed CRC for CMD0

// CMD8 - SEND_IF_COND: check voltage support, detect SD 2.0+
#define SD_CMD8      8
#define SD_CMD8_ARG  0x000001AAU // 2.7-3.6V range + 0xAA check pattern
#define SD_CMD8_CRC  0x87U       // fixed CRC for CMD8

// CMD55 - APP_CMD: flags the next command as application-specific
#define SD_CMD55 55

// ACMD41: start init
#define SD_CMD41      41
#define SD_ACMD41_ARG 0x40000000U // HCS bit: host supports SDHC/SDXC

// CMD58 - READ_OCR: read capacity/voltage info, used for SDHC/SDXC detection
#define SD_CMD58 58

#define SD_DUMMY_CRC    0x01U // CRC isn't checked after CMD0/CMD8
#define SD_R1_IDLE_MASK 0x01U

// CMD17 - READ_SINGLE_BLOCK: read one 512-byte block at the given address
#define SD_CMD17 17
#define SD_DATA_START_TOKEN     0xFEU // precedes block data, both directions
#define SD_BLOCK_SIZE           512U

// CMD24 - WRITE_BLOCK: write one 512-byte block at the given address
#define SD_CMD24 24
#define SD_DATA_ACCEPTED_MASK   0x1FU // lower 5 bits of the data response token
#define SD_DATA_ACCEPTED_VALUE  0x05U // 0bxxx00101 = write accepted

uint8_t sd_send_command(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t has_extra, uint8_t *extra_bytes);
uint8_t sd_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t *is_sdhc);
uint8_t sd_read_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf);
uint8_t sd_write_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, const uint8_t *buf);

#endif /* INC_SD_H_ */
