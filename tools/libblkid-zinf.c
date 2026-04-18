/*
 * libblkid superblock prober for the ZINF filesystem (format v4).
 *
 * This file adds ZINF detection to libblkid so that:
 *   blkid /dev/sdX          → TYPE="zinf"
 *   lsblk -f /dev/sdX       → FSTYPE=zinf  FSVER=4
 *
 * Build path:
 *   See tools/install-libblkid-zinf.sh — it fetches the matching util-linux
 *   source, drops this file in, patches the probe list, and installs the
 *   resulting libblkid.so over the system copy.
 *
 * Format v4 on-disk layout (sector 0, byte 0):
 *   [0..3]  Magic: 'Z','I','N','F'  (0x5A 0x49 0x4E 0x46)
 *   [4..5]  Format version (uint16 LE) — currently 4
 *   [6..7]  Reserved
 */

#include <errno.h>
#include <stdint.h>
#include "superblocks.h"

struct zinf_super {
	uint8_t  magic[4];    /* 'Z','I','N','F' */
	uint16_t version;     /* format version, little-endian */
	uint16_t reserved;
} __attribute__((packed));

static int probe_zinf(blkid_probe pr, const struct blkid_idmag *mag)
{
	const struct zinf_super *sb;

	sb = blkid_probe_get_sb(pr, mag, struct zinf_super);
	if (!sb)
		return errno ? -errno : 1;

	blkid_probe_set_version(pr, "%u", le16_to_cpu(sb->version));
	return 0;
}

const struct blkid_idinfo zinf_idinfo = {
	.name      = "zinf",
	.usage     = BLKID_USAGE_FILESYSTEM,
	.probefunc = probe_zinf,
	.magics    = (const struct blkid_idmag[]) {
		{
			.magic = "ZINF",
			.len   = 4,
			.kboff = 0,     /* kilobyte offset from start of device */
			.sboff = 0,     /* byte offset within that kilobyte */
		},
		{ NULL }
	}
};
