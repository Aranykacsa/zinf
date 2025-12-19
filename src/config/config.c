#include "config.h"

extern config_t* get_config(void) {
    return &config;
}

config_t config = {
    .sector = 512,
    .crc_key = 0xEDB88320,
    .mirror_count = 3,
    .mirror_offset = 512,
};
