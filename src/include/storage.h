#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- Storage return codes ---- */
#define STORAGE_OK             0
#define STORAGE_ERR_PARAM      1
#define STORAGE_ERR_DRIVER     2
#define STORAGE_ERR_LOG_FULL   3

/* Driver return codes (must match your driver implementation) */
#define DRIVER_OK              0

/* ---- External state (defined in main.c / platform) ---- */
struct driver_t;
extern struct driver_t *active_driver;
extern uint32_t log_sector;

/* ---- Storage low-level I/O (implemented in core/data/data.c) ---- */
int read_sector(uint32_t sector, uint8_t *buffer);
int write_sector(uint32_t sector, const uint8_t *buffer);

/* ---- Public API (implemented in core/data/data.c) ---- */
uint8_t setup_storage(void);

/* “msg log” API */
uint8_t save_msg(uint8_t *msg);
uint8_t test_save_msg(void);

/* Log init + writers (compatible names) */
uint8_t init_log_sector(void);
uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
