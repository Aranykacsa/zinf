#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "storage.h"
#include "config.h"

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s cli            # interactive CLI (default)\n", argv0);
    printf("  %s <cmd...>        # run one CLI command\n", argv0);
    printf("\nExamples:\n");
    printf("  sudo %s log init\n", argv0);
    printf("  sudo %s log last\n", argv0);
    printf("  sudo %s msg save 42\n", argv0);
    printf("  sudo %s raid u8 1 4 10 20 30 40\n", argv0);
}

static void init_for_linux_loop0(void) {
#ifdef ZINF_PLATFORM_LINUX
    /* Keep your existing workflow: /dev/loop0 + sudo */
    /* RAID_OFFSET is used by config_init_defaults() */
    RAID_OFFSET = 30; /* safe fallback if you don’t compute here */
#endif

    if (setup_storage() != STORAGE_OK) {
        fprintf(stderr, "setup_storage failed (need sudo for /dev/loop0?)\n");
        exit(1);
    }
}

static void repl(void) {
    printf("ZINF CLI. Type 'help'.\n> ");
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        cli_rx_char((char)c);
        fflush(stdout);
    }
}

static void run_one_command(int argc, char **argv) {
    char line[512];
    size_t pos = 0;

    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (pos + n + 2 >= sizeof(line)) {
            fprintf(stderr, "command too long\n");
            exit(2);
        }
        memcpy(&line[pos], argv[i], n);
        pos += n;
        if (i != argc - 1) line[pos++] = ' ';
    }
    line[pos] = '\0';

    cli_process_line(line);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    init_for_linux_loop0();

    if (argc == 1 || (argc >= 2 && strcmp(argv[1], "cli") == 0)) {
        repl();
        return 0;
    }

    run_one_command(argc, argv);
    return 0;
}
