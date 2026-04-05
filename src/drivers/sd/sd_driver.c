/*
 * SD card driver — SPI mode
 *
 * Supports SD (byte addressing) and SDHC/SDXC (block addressing).
 * Addressing mode is detected at init via CMD58 OCR bit 30 (CCS).
 *
 * SD SPI command protocol summary:
 *   Byte 0:  0x40 | cmd_index
 *   Byte 1-4: argument (big-endian)
 *   Byte 5:  CRC7 | 0x01 (CRC is only checked for CMD0 and CMD8)
 *   Response: R1 (1 byte) or R3/R7 (5 bytes = R1 + 4 data bytes)
 */

#include "sd_driver.h"
#include "driver.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -----------------------------------------------------------------------
   SD command indices
   ----------------------------------------------------------------------- */
#define SD_CMD0     0u   /* GO_IDLE_STATE        — software reset, enter SPI mode */
#define SD_CMD8     8u   /* SEND_IF_COND         — voltage/version check (SDv2) */
#define SD_CMD12   12u   /* STOP_TRANSMISSION    — end multi-block read */
#define SD_CMD17   17u   /* READ_SINGLE_BLOCK    */
#define SD_CMD18   18u   /* READ_MULTIPLE_BLOCK  */
#define SD_CMD24   24u   /* WRITE_BLOCK          */
#define SD_CMD25   25u   /* WRITE_MULTIPLE_BLOCK */
#define SD_CMD55   55u   /* APP_CMD              — prefix for ACMD */
#define SD_CMD58   58u   /* READ_OCR             — read OCR register */
#define SD_ACMD41  41u   /* SD_SEND_OP_COND      — start initialisation */

/* -----------------------------------------------------------------------
   R1 response bits
   ----------------------------------------------------------------------- */
#define SD_R1_IDLE          0x01u
#define SD_R1_ERASE_RESET   0x02u
#define SD_R1_ILLEGAL_CMD   0x04u
#define SD_R1_CRC_ERR       0x08u
#define SD_R1_ERASE_SEQ_ERR 0x10u
#define SD_R1_ADDR_ERR      0x20u
#define SD_R1_PARAM_ERR     0x40u
#define SD_R1_ERROR_MASK    0xFEu   /* any bit except IDLE means error */

/* Data token for CMD17/CMD18/CMD24 */
#define SD_TOKEN_START_BLOCK   0xFEu
/* Data token for CMD25 */
#define SD_TOKEN_START_WRITE_MULTIPLE 0xFCu
#define SD_TOKEN_STOP_TRAN            0xFDu

/* Data response mask/value */
#define SD_DATA_ACCEPTED    0x05u
#define SD_DATA_RESP_MASK   0x1Fu

#define SD_MAX_RETRY        2000u
#define SD_SECTOR_SIZE      512u

/* -----------------------------------------------------------------------
   Driver private context
   ----------------------------------------------------------------------- */
typedef struct {
    spi_handle_t   spi;
    sd_spi_ops_t   ops;
    int            is_hc;    /* 1 = SDHC/SDXC (block addressing), 0 = SD (byte) */
    int            ready;    /* 1 after successful init */
} sd_ctx_t;

static sd_ctx_t g_sd_ctx = {
    .spi    = NULL,
    .is_hc  = 0,
    .ready  = 0,
};

/* -----------------------------------------------------------------------
   SPI helpers
   ----------------------------------------------------------------------- */

static inline void cs_high(sd_ctx_t *c) { c->ops.cs_set(c->spi, 0); }
static inline void cs_low(sd_ctx_t *c)  { c->ops.cs_set(c->spi, 1); }

static inline uint8_t spi_xfer(sd_ctx_t *c, uint8_t tx) {
    return c->ops.xfer(c->spi, tx);
}

static void spi_tx(sd_ctx_t *c, const uint8_t *buf, size_t len) {
    if (c->ops.tx_buf) {
        c->ops.tx_buf(c->spi, buf, len);
    } else {
        for (size_t i = 0; i < len; i++) spi_xfer(c, buf[i]);
    }
}

static void spi_rx(sd_ctx_t *c, uint8_t *buf, size_t len) {
    if (c->ops.rx_buf) {
        c->ops.rx_buf(c->spi, buf, len);
    } else {
        for (size_t i = 0; i < len; i++) buf[i] = spi_xfer(c, 0xFFu);
    }
}

/* Transmit 0xFF bytes until the SD card releases MISO (busy wait) */
static int sd_wait_ready(sd_ctx_t *c) {
    for (uint32_t i = 0; i < SD_MAX_RETRY; i++) {
        if (spi_xfer(c, 0xFFu) == 0xFFu) return 0;
    }
    return -1; /* timeout */
}

/* -----------------------------------------------------------------------
   CRC helpers (CRC7 for commands, CRC16 unused in SPI mode by default)
   ----------------------------------------------------------------------- */

static uint8_t crc7_byte(uint8_t crc, uint8_t data) {
    for (int i = 0; i < 8; i++) {
        crc = (uint8_t)(((crc << 1) | (data >> 7)) ^
                        (((crc & 0x40u) != 0) ? 0x09u : 0u));
        data <<= 1;
    }
    return crc;
}

static uint8_t sd_crc7(const uint8_t *buf, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) crc = crc7_byte(crc, buf[i]);
    return (uint8_t)((crc << 1) | 0x01u);
}

/* -----------------------------------------------------------------------
   Command layer
   ----------------------------------------------------------------------- */

/* Send one SD command, return R1 response byte (0xFF = timeout) */
static uint8_t sd_send_cmd(sd_ctx_t *c, uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];
    frame[0] = (uint8_t)(0x40u | cmd);
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >>  8);
    frame[4] = (uint8_t)(arg);
    frame[5] = sd_crc7(frame, 5);

    /* Allow SD card to finish any previous operation */
    sd_wait_ready(c);

    spi_tx(c, frame, sizeof(frame));

    /* Poll for R1 (up to 8 bytes of 0xFF are acceptable per spec) */
    uint8_t r1 = 0xFFu;
    for (int i = 0; i < 8; i++) {
        r1 = spi_xfer(c, 0xFFu);
        if ((r1 & 0x80u) == 0) break;
    }
    return r1;
}

/* Send ACMD (preceded by CMD55), return R1 */
static uint8_t sd_send_acmd(sd_ctx_t *c, uint8_t acmd, uint32_t arg) {
    uint8_t r1 = sd_send_cmd(c, SD_CMD55, 0);
    if (r1 & SD_R1_ERROR_MASK) return r1;
    return sd_send_cmd(c, acmd, arg);
}

/* Read R3/R7 trailing 4 bytes into `out` (call after sd_send_cmd) */
static void sd_read_r3_tail(sd_ctx_t *c, uint8_t out[4]) {
    spi_rx(c, out, 4);
}

/* Wait for data start token 0xFE; return 0 on success */
static int sd_wait_token(sd_ctx_t *c) {
    for (uint32_t i = 0; i < SD_MAX_RETRY; i++) {
        uint8_t b = spi_xfer(c, 0xFFu);
        if (b == SD_TOKEN_START_BLOCK) return 0;
        if (b != 0xFFu) return -1; /* error token */
    }
    return -1;
}

/* -----------------------------------------------------------------------
   Init
   ----------------------------------------------------------------------- */

static int sd_init(driver_t *self) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;

    if (!c->ops.cs_set || !c->ops.xfer) return DRIVER_ERR_INIT;

    cs_high(c);

    /* ≥74 clock cycles with CS high to enter SPI mode */
    for (int i = 0; i < 10; i++) spi_xfer(c, 0xFFu);

    cs_low(c);

    /* CMD0 — software reset */
    uint8_t r1 = sd_send_cmd(c, SD_CMD0, 0);
    if (r1 != SD_R1_IDLE) { cs_high(c); return DRIVER_ERR_INIT; }

    /* CMD8 — check voltage range (SDv2 detection) */
    int sdv2 = 0;
    r1 = sd_send_cmd(c, SD_CMD8, 0x000001AAu); /* VHS=1 (2.7-3.6V), check=0xAA */
    if ((r1 & SD_R1_ERROR_MASK) == 0 && r1 == SD_R1_IDLE) {
        uint8_t r7[4];
        sd_read_r3_tail(c, r7);
        /* Verify echo: lower 12 bits must be 0x1AA */
        if ((r7[2] & 0x0Fu) == 0x01u && r7[3] == 0xAAu) sdv2 = 1;
    }

    /* Send ACMD41 repeatedly until card leaves idle state */
    uint32_t hcs_arg = sdv2 ? 0x40000000u : 0u; /* HCS bit for SDv2 */
    uint32_t retry = 0;
    do {
        r1 = sd_send_acmd(c, SD_ACMD41, hcs_arg);
        if (++retry > SD_MAX_RETRY) { cs_high(c); return DRIVER_ERR_INIT; }
    } while (r1 == SD_R1_IDLE);

    if (r1 != 0x00u) { cs_high(c); return DRIVER_ERR_INIT; }

    /* CMD58 — read OCR to detect SDHC/SDXC (CCS bit 30) */
    c->is_hc = 0;
    if (sdv2) {
        r1 = sd_send_cmd(c, SD_CMD58, 0);
        if ((r1 & SD_R1_ERROR_MASK) == 0) {
            uint8_t ocr[4];
            sd_read_r3_tail(c, ocr);
            c->is_hc = (ocr[0] & 0x40u) ? 1 : 0; /* CCS = bit 30 = byte[0] bit 6 */
        }
    }

    cs_high(c);
    spi_xfer(c, 0xFFu); /* extra clock */

    self->sector_size   = SD_SECTOR_SIZE;
    self->total_sectors = 0; /* unknown without CSD read */
    c->ready = 1;

    return DRIVER_OK;
}

/* -----------------------------------------------------------------------
   Single-block read / write
   ----------------------------------------------------------------------- */

static int sd_read_block(driver_t *self, uint64_t lba, uint8_t *buf) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;
    if (!c->ready) return DRIVER_ERR_IO;

    /* Byte-addressed SD cards use byte offset; SDHC/SDXC use block index */
    uint32_t addr = c->is_hc ? (uint32_t)lba
                              : (uint32_t)(lba * SD_SECTOR_SIZE);

    cs_low(c);

    uint8_t r1 = sd_send_cmd(c, SD_CMD17, addr);
    if (r1 != 0x00u) { cs_high(c); return DRIVER_ERR_IO; }

    if (sd_wait_token(c) != 0) { cs_high(c); return DRIVER_ERR_IO; }

    spi_rx(c, buf, SD_SECTOR_SIZE);

    /* Read and discard 2-byte CRC (not checked in SPI mode by default) */
    spi_xfer(c, 0xFFu);
    spi_xfer(c, 0xFFu);

    cs_high(c);
    spi_xfer(c, 0xFFu); /* trailing clock */
    return DRIVER_OK;
}

static int sd_write_block(driver_t *self, uint64_t lba, const uint8_t *buf) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;
    if (!c->ready) return DRIVER_ERR_IO;

    uint32_t addr = c->is_hc ? (uint32_t)lba
                              : (uint32_t)(lba * SD_SECTOR_SIZE);

    cs_low(c);

    uint8_t r1 = sd_send_cmd(c, SD_CMD24, addr);
    if (r1 != 0x00u) { cs_high(c); return DRIVER_ERR_IO; }

    spi_xfer(c, 0xFFu);                     /* one idle byte before token */
    spi_xfer(c, SD_TOKEN_START_BLOCK);
    spi_tx(c, buf, SD_SECTOR_SIZE);
    spi_xfer(c, 0xFFu);                     /* dummy CRC high */
    spi_xfer(c, 0xFFu);                     /* dummy CRC low  */

    uint8_t resp = spi_xfer(c, 0xFFu);
    if ((resp & SD_DATA_RESP_MASK) != SD_DATA_ACCEPTED) {
        cs_high(c); return DRIVER_ERR_IO;
    }

    /* Wait until card finishes programming */
    if (sd_wait_ready(c) != 0) { cs_high(c); return DRIVER_ERR_IO; }

    cs_high(c);
    spi_xfer(c, 0xFFu);
    return DRIVER_OK;
}

/* -----------------------------------------------------------------------
   Multi-block read / write
   ----------------------------------------------------------------------- */

static int sd_read_blocks(driver_t *self, uint64_t lba, uint8_t *buf, uint32_t count) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;
    if (!c->ready || count == 0) return DRIVER_ERR_IO;

    if (count == 1) return sd_read_block(self, lba, buf);

    uint32_t addr = c->is_hc ? (uint32_t)lba
                              : (uint32_t)(lba * SD_SECTOR_SIZE);

    cs_low(c);

    uint8_t r1 = sd_send_cmd(c, SD_CMD18, addr);
    if (r1 != 0x00u) { cs_high(c); return DRIVER_ERR_IO; }

    for (uint32_t i = 0; i < count; i++) {
        if (sd_wait_token(c) != 0) {
            sd_send_cmd(c, SD_CMD12, 0);
            cs_high(c);
            return DRIVER_ERR_IO;
        }
        spi_rx(c, buf + (i * SD_SECTOR_SIZE), SD_SECTOR_SIZE);
        spi_xfer(c, 0xFFu); /* CRC high */
        spi_xfer(c, 0xFFu); /* CRC low  */
    }

    /* CMD12 — stop transmission */
    sd_send_cmd(c, SD_CMD12, 0);
    sd_wait_ready(c);

    cs_high(c);
    spi_xfer(c, 0xFFu);
    return DRIVER_OK;
}

static int sd_write_blocks(driver_t *self, uint64_t lba, const uint8_t *buf, uint32_t count) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;
    if (!c->ready || count == 0) return DRIVER_ERR_IO;

    if (count == 1) return sd_write_block(self, lba, buf);

    uint32_t addr = c->is_hc ? (uint32_t)lba
                              : (uint32_t)(lba * SD_SECTOR_SIZE);

    cs_low(c);

    uint8_t r1 = sd_send_cmd(c, SD_CMD25, addr);
    if (r1 != 0x00u) { cs_high(c); return DRIVER_ERR_IO; }

    spi_xfer(c, 0xFFu); /* one idle byte before first token */

    for (uint32_t i = 0; i < count; i++) {
        spi_xfer(c, SD_TOKEN_START_WRITE_MULTIPLE);
        spi_tx(c, buf + (i * SD_SECTOR_SIZE), SD_SECTOR_SIZE);
        spi_xfer(c, 0xFFu); /* dummy CRC high */
        spi_xfer(c, 0xFFu); /* dummy CRC low  */

        uint8_t resp = spi_xfer(c, 0xFFu);
        if ((resp & SD_DATA_RESP_MASK) != SD_DATA_ACCEPTED) {
            /* Send stop token and abort */
            spi_xfer(c, SD_TOKEN_STOP_TRAN);
            sd_wait_ready(c);
            cs_high(c);
            return DRIVER_ERR_IO;
        }
        if (sd_wait_ready(c) != 0) {
            spi_xfer(c, SD_TOKEN_STOP_TRAN);
            cs_high(c);
            return DRIVER_ERR_IO;
        }
    }

    /* Stop multiple-block write */
    spi_xfer(c, SD_TOKEN_STOP_TRAN);
    spi_xfer(c, 0xFFu);
    sd_wait_ready(c);

    cs_high(c);
    spi_xfer(c, 0xFFu);
    return DRIVER_OK;
}

/* -----------------------------------------------------------------------
   Deinit
   ----------------------------------------------------------------------- */

static void sd_deinit(driver_t *self) {
    sd_ctx_t *c = (sd_ctx_t *)self->ctx;
    cs_high(c);
    c->ready = 0;
}

/* -----------------------------------------------------------------------
   Public API
   ----------------------------------------------------------------------- */

void sd_driver_init_spi(spi_handle_t handle, const sd_spi_ops_t *ops) {
    g_sd_ctx.spi   = handle;
    g_sd_ctx.ops   = *ops;
    g_sd_ctx.ready = 0;
    g_sd_ctx.is_hc = 0;
}

driver_t sd_driver = {
    .name         = "sd_spi",
    .sector_size  = SD_SECTOR_SIZE,
    .ctx          = &g_sd_ctx,
    .init         = sd_init,
    .deinit       = sd_deinit,
    .read_block   = sd_read_block,
    .write_block  = sd_write_block,
    .read_blocks  = sd_read_blocks,
    .write_blocks = sd_write_blocks,
    .sync         = NULL,
};
