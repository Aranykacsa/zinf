#include "cli.h"

#include "storage.h"
#include "data.h"
#include "log.h"
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef CLI_MAX_LINE
#define CLI_MAX_LINE 256
#endif

#ifndef CLI_MAX_TOKENS
#define CLI_MAX_TOKENS 32
#endif

static char  g_line[CLI_MAX_LINE];
static size_t g_len = 0;

static void prompt(void) { printf("> "); }

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return 0;
    *out = (uint32_t)v;
    return 1;
}

static int tokenize(char *line, char *argv[], int max_argv) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void help(void) {
    printf("Commands:\r\n");
    printf("  help\r\n");
    printf("  storage init\r\n");
    printf("  log init\r\n");
    printf("  log last\r\n");
    printf("  msg save <0-255>\r\n");
    printf("  msg test\r\n");
    printf("  raid u8 <header> <count> <v0> <v1> ...\r\n");
    printf("  cfg show\r\n");
}

static void cfg_show(void) {
    printf("SECTOR_SIZE   : %u\r\n", (unsigned)config->sector_size);
    printf("PAYLOAD_SIZE  : %u\r\n", (unsigned)PAYLOAD_SIZE);
    printf("MIRRORS       : %u\r\n", (unsigned)config->mirror_count);
    printf("MIRROR_OFFSET : %u\r\n", (unsigned)config->mirror_offset);
}

static void cmd_storage_init(void) {
    uint8_t rc = setup_storage();
    printf("setup_storage: %u\r\n", (unsigned)rc);
}

static void cmd_log_init(void) {
    uint8_t rc = init_log_sector();
    printf("init_log_sector: %u\r\n", (unsigned)rc);
}

static void cmd_log_last(void) {
    uint32_t last = 0;
    uint8_t rc = log_get_last_sector(&last);
    if (rc != STORAGE_OK) {
        printf("log_get_last_sector error: %u\r\n", (unsigned)rc);
        return;
    }
    printf("last_sector: %lu\r\n", (unsigned long)last);
}

static void cmd_msg_save(const char *arg) {
    uint32_t v = 0;
    if (!parse_u32(arg, &v) || v > 255) {
        printf("usage: msg save <0-255>\r\n");
        return;
    }
    uint8_t b = (uint8_t)v;
    uint8_t rc = save_msg(&b);
    printf("save_msg: %u\r\n", (unsigned)rc);
}

static void cmd_msg_test(void) {
    uint8_t rc = test_save_msg();
    printf("test_save_msg: %u\r\n", (unsigned)rc);
}

static void cmd_raid_u8(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: raid u8 <header> <count> <v0> <v1> ...\r\n");
        return;
    }

    uint32_t header_u32 = 0, count_u32 = 0;
    if (!parse_u32(argv[2], &header_u32) || header_u32 > 255 ||
        !parse_u32(argv[3], &count_u32)) {
        printf("invalid header/count\r\n");
        return;
    }

    uint32_t count = count_u32;
    if (count == 0) { printf("count must be > 0\r\n"); return; }

    uint32_t provided = (uint32_t)(argc - 4);
    if (provided < count) {
        printf("need %lu values, got %lu\r\n",
               (unsigned long)count, (unsigned long)provided);
        return;
    }

    /* keep it bounded for safety */
    if (count > PAYLOAD_SIZE) {
        printf("count too big for single sector payload (max %u)\r\n", (unsigned)PAYLOAD_SIZE);
        return;
    }

    uint8_t buf[PAYLOAD_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = 0;
        if (!parse_u32(argv[4 + i], &v) || v > 255) {
            printf("invalid value at index %lu\r\n", (unsigned long)i);
            return;
        }
        buf[i] = (uint8_t)v;
    }

    uint8_t header = (uint8_t)header_u32;
    uint8_t rc = raid_u8bit_values(buf, (size_t)count, &header);
    printf("raid_u8bit_values: %u\r\n", (unsigned)rc);
}

void cli_process_line(const char *line_in) {
    if (!line_in) return;

    char line[CLI_MAX_LINE];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *argv[CLI_MAX_TOKENS];
    int argc = tokenize(line, argv, CLI_MAX_TOKENS);

    if (argc == 0) { prompt(); return; }

    if (strcmp(argv[0], "help") == 0) {
        help(); prompt(); return;
    }

    if (strcmp(argv[0], "cfg") == 0 && argc >= 2 && strcmp(argv[1], "show") == 0) {
        cfg_show(); prompt(); return;
    }

    if (strcmp(argv[0], "storage") == 0) {
        if (argc >= 2 && strcmp(argv[1], "init") == 0) cmd_storage_init();
        else printf("usage: storage init\r\n");
        prompt(); return;
    }

    if (strcmp(argv[0], "log") == 0) {
        if (argc >= 2 && strcmp(argv[1], "init") == 0) cmd_log_init();
        else if (argc >= 2 && strcmp(argv[1], "last") == 0) cmd_log_last();
        else printf("usage: log init | log last\r\n");
        prompt(); return;
    }

    if (strcmp(argv[0], "msg") == 0) {
        if (argc >= 2 && strcmp(argv[1], "test") == 0) cmd_msg_test();
        else if (argc >= 3 && strcmp(argv[1], "save") == 0) cmd_msg_save(argv[2]);
        else printf("usage: msg save <0-255> | msg test\r\n");
        prompt(); return;
    }

    if (strcmp(argv[0], "raid") == 0) {
        if (argc >= 2 && strcmp(argv[1], "u8") == 0) cmd_raid_u8(argc, argv);
        else printf("usage: raid u8 <header> <count> <v0> ...\r\n");
        prompt(); return;
    }

    printf("unknown command: %s\r\n", argv[0]);
    prompt();
}

void cli_rx_char(char c) {
    if (c == '\r') return;

    if (c == '\n') {
        g_line[g_len] = '\0';
        printf("\r\n");
        cli_process_line(g_line);
        g_len = 0;
        return;
    }

    if (c == '\b' || c == 127) {
        if (g_len > 0) {
            g_len--;
            printf("\b \b");
        }
        return;
    }

    if (g_len < (CLI_MAX_LINE - 1)) {
        g_line[g_len++] = c;
        putchar(c);
    }
}
