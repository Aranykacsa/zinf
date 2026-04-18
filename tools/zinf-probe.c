/*
 * zinf-probe — detect a ZINF format v4 filesystem on a block device.
 *
 * Usage (human):
 *   zinf-probe <device>
 *   Prints "zinf" and exits 0 if the device starts with the ZINF magic;
 *   exits 1 otherwise.
 *
 * Usage (udev IMPORT):
 *   zinf-probe --udev <device>
 *   On match: prints KEY=VALUE pairs for udev (ID_FS_TYPE, ID_FS_VERSION)
 *   and exits 0.
 *   On no match: exits 1 with no output.
 *
 * The udev rule in 99-zinf.rules uses the --udev form.
 *
 * Build:
 *   gcc -std=c11 -O2 -o zinf-probe tools/zinf-probe.c
 *
 * Install:
 *   sudo install -m 755 zinf-probe /usr/local/bin/zinf-probe
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

/* Format v4 magic: bytes 0-3 = 'Z','I','N','F'; bytes 4-5 = version LE */
static const uint8_t ZINF_MAGIC[4] = { 0x5A, 0x49, 0x4E, 0x46 };

int main(int argc, char **argv) {
    int udev_mode = 0;
    const char *dev = NULL;

    if (argc == 3 && strcmp(argv[1], "--udev") == 0) {
        udev_mode = 1;
        dev = argv[2];
    } else if (argc == 2) {
        dev = argv[1];
    } else {
        fprintf(stderr, "usage: zinf-probe [--udev] <device>\n");
        return 1;
    }

    int fd = open(dev, O_RDONLY);
    if (fd < 0) return 1;

    uint8_t header[6];
    ssize_t n = read(fd, header, sizeof(header));
    close(fd);

    if (n < 6) return 1;
    if (memcmp(header, ZINF_MAGIC, 4) != 0) return 1;

    uint16_t version = (uint16_t)header[4] | ((uint16_t)header[5] << 8);

    if (udev_mode) {
        printf("ID_FS_TYPE=zinf\n");
        printf("ID_FS_VERSION=%u\n", (unsigned)version);
    } else {
        printf("zinf\n");
    }

    return 0;
}
