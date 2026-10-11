#include "sd.h"

// SDHC/SDXC cards use block addressing; set by sd_init()
static uint8_t sd_card_is_sdhc = 0;

// SPI_TransferByte with a timeout check; deselects the card on failure
static uint8_t sd_xfer(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t byte_in, uint8_t *byte_out) {
    if (!SPI_TransferByte(spi_port, tim_port, byte_in, byte_out)) {
        GPIO_Set(cs_port, cs_pin, 1); // deselect on SPI timeout
        return 0;
    }
    return 1;
}

uint8_t sd_send_command(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t has_extra, uint8_t *extra_bytes) {
    uint8_t dummy;
    uint8_t response = 0xFF; // R1 reply; 0xFF = none (a real one has bit 7 clear)

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    // Send the 6-byte command frame
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0x40 | cmd, &dummy)) return 0xFF;          // start bits + command
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (arg >> 24) & 0xFF, &dummy)) return 0xFF;  // argument, MSB first
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (arg >> 16) & 0xFF, &dummy)) return 0xFF;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (arg >> 8)  & 0xFF, &dummy)) return 0xFF;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (arg >> 0)  & 0xFF, &dummy)) return 0xFF;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, crc, &dummy)) return 0xFF;                 // CRC + stop bit

    // Poll for the R1 response - keep clocking 0xFF until the top bit clears, or give up
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &response)) return 0xFF;
        if ((response & 0x80) == 0) { // top bit clear - valid response byte
            break;
        }
    }

    // R3/R7 responses have 4 extra trailing bytes - read them before releasing CS
    if (has_extra) {
        for (uint8_t i = 0; i < 4; i++) {
            if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &extra_bytes[i])) return 0xFF;
        }
    }

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    return response; // R1 byte - 0xFF still set means we never got a real response
}

uint8_t sd_init(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint8_t *is_sdhc) {
    uint8_t dummy;
    uint8_t response;
    uint8_t extra[4];

    sd_card_is_sdhc = 0; // reset until CMD58 confirms otherwise

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    // Send 10 dummy bytes to get 74 clock pulses (74/8 = 9.25, round up to 10)
    for (uint8_t i = 0; i < 10; i++) {
        if (!SPI_TransferByte(spi_port, tim_port, 0xFF, &dummy)) return 0; // SPI timeout
    }

    // Send CMD0 - expect idle state (0x01)
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD0, SD_CMD0_ARG, SD_CMD0_CRC, 0, NULL);
    if (response != 0x01) return 0; // card didn't reset correctly

    // Send CMD8 - check voltage/version, expect idle state + echoed check pattern
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD8, SD_CMD8_ARG, SD_CMD8_CRC, 1, extra);
    if (response != 0x01) return 0; // card doesn't support CMD8
    if (extra[2] != 0x01 || extra[3] != 0xAA) return 0; // echo mismatch, something's wrong

    // Loop CMD55 + ACMD41 until the card reports it's ready (0x00), or give up
    uint8_t ready = 0;
    for (uint16_t i = 0; i < 1000; i++) {
        response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD55, 0, SD_DUMMY_CRC, 0, NULL);
        if (response > 0x01) return 0; // CMD55 itself failed unexpectedly

        response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD41, SD_ACMD41_ARG, SD_DUMMY_CRC, 0, NULL);
        if (response == 0x00) {
            ready = 1;
            break;
        }
    }
    if (!ready) return 0; // card never finished initializing

    // CMD58: read the OCR; the CCS bit marks SDHC/SDXC
    response = sd_send_command(spi_port, tim_port, cs_port, cs_pin, SD_CMD58, 0, SD_DUMMY_CRC, 1, extra);
    if (response != 0x00) return 0;

    *is_sdhc = (extra[0] & 0x40) ? 1 : 0; // bit 6 of OCR byte 0 = CCS
    sd_card_is_sdhc = *is_sdhc; // remember for sd_read_block/sd_write_block addressing

    return 1; // success
}

uint8_t sd_read_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, uint8_t *buf) {
    uint8_t dummy;
    uint8_t response;

    // SDHC/SDXC cards address by block number; SDSC cards need a raw byte offset
    uint32_t card_addr = sd_card_is_sdhc ? addr : (addr * SD_BLOCK_SIZE);

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    // Send CMD17's 6-byte frame directly (CS must remain LOW)
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0x40 | SD_CMD17, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 24) & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 16) & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 8)  & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 0)  & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, SD_DUMMY_CRC, &dummy)) return 0;

    // Poll for the R1 response
    response = 0xFF;
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &response)) return 0;
        if ((response & 0x80) == 0) break;
    }
    if (response != 0x00) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the read command
    }

    // wait for the data start token (the card is reading flash)
    uint8_t got_token = 0;
    for (uint32_t i = 0; i < SD_READ_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &response)) return 0;
        if (response == SD_DATA_START_TOKEN) {
            got_token = 1;
            break;
        }
    }
    if (!got_token) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // data never arrived
    }

    // Read the 512 data bytes into the caller's buffer
    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &buf[i])) return 0;
    }

    // Read and discard the 2 trailing CRC bytes
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &dummy)) return 0;

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    return 1; // success
}

uint8_t sd_write_block(SPI_TypeDef *spi_port, TIM_TypeDef *tim_port, GPIO_TypeDef *cs_port, uint8_t cs_pin, uint32_t addr, const uint8_t *buf) {
    uint8_t dummy;
    uint8_t response;

    // SDHC/SDXC cards address by block number; SDSC cards need a raw byte offset
    uint32_t card_addr = sd_card_is_sdhc ? addr : (addr * SD_BLOCK_SIZE);

    GPIO_Set(cs_port, cs_pin, 0); // select the card

    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0x40 | SD_CMD24, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 24) & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 16) & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 8)  & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, (card_addr >> 0)  & 0xFF, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, SD_DUMMY_CRC, &dummy)) return 0;

    response = 0xFF;
    for (uint8_t i = 0; i < SD_RESPONSE_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &response)) return 0;
        if ((response & 0x80) == 0) break;
    }
    if (response != 0x00) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the write command
    }

    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, SD_DATA_START_TOKEN, &dummy)) return 0;

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, buf[i], &dummy)) return 0;
    }

    // Send 2 dummy CRC bytes
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, SD_DUMMY_CRC, &dummy)) return 0;
    if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, SD_DUMMY_CRC, &dummy)) return 0;

    // Poll for data response token
    uint8_t got_token = 0;
    for (uint16_t i = 0; i < SD_DATA_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &response)) return 0;
        if ((response & SD_DATA_ACCEPTED_MASK) == SD_DATA_ACCEPTED_VALUE) {
            got_token = 1;
            break;
        }
    }
    if (!got_token) {
        GPIO_Set(cs_port, cs_pin, 1);
        return 0; // card rejected the data
    }

    // wait out the card's internal write (line held at 0x00)
    uint8_t busy_response = 0x00;
    uint8_t finished = 0;
    for (uint32_t i = 0; i < SD_BUSY_TIMEOUT_TRIES; i++) {
        if (!sd_xfer(spi_port, tim_port, cs_port, cs_pin, 0xFF, &busy_response)) return 0;
        if (busy_response == 0xFF) {
            finished = 1;
            break; // no longer busy
        }
    }

    GPIO_Set(cs_port, cs_pin, 1); // deselect the card

    if (!finished) {
        return 0; // card never finished writing
    }

    return 1; // success
}
