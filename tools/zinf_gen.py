#!/usr/bin/env python3
"""
zinf_gen.py — Generate src/config/config.h and src/config/config.c from zinf.yaml

Usage:
    python3 tools/zinf_gen.py zinf.yaml src/config/

Requires: PyYAML  (pip install pyyaml)
"""

import sys
import os
import textwrap

try:
    import yaml
except ImportError:
    sys.exit("error: PyYAML is required — run: pip install pyyaml")


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

FLOAT_TYPES  = {"float", "double"}
INT_TYPES    = {"uint8_t", "uint16_t", "uint32_t", "uint64_t",
                "int8_t",  "int16_t",  "int32_t",  "int64_t",
                "int", "unsigned int", "char"}
KNOWN_TYPES  = FLOAT_TYPES | INT_TYPES

WIRE_SIZES = {
    "float":        4,
    "double":       8,
    "uint8_t":      1, "int8_t":  1, "char":     1,
    "uint16_t":     2, "int16_t": 2,
    "uint32_t":     4, "int32_t": 4, "int":      4, "unsigned int": 4,
    "uint64_t":     8, "int64_t": 8,
}


def validate(cfg: dict) -> dict:
    z = cfg.get("zinf")
    if not z:
        sys.exit("error: top-level 'zinf:' key missing")

    sector_size      = int(z.get("sector_size",      512))
    mirror_count     = int(z.get("mirror_count",     2))
    header_size      = int(z.get("header_size",      1))
    metadata_sectors = int(z.get("metadata_sectors", 2))
    max_bad_sectors  = int(z.get("max_bad_sectors",  16))

    if sector_size < 64 or sector_size > 65536:
        sys.exit(f"error: sector_size={sector_size} out of range [64, 65536]")
    if sector_size & (sector_size - 1):
        sys.exit(f"error: sector_size={sector_size} must be a power of two")
    if mirror_count < 1:
        sys.exit(f"error: mirror_count must be >= 1")
    if metadata_sectors < 2:
        sys.exit(f"error: metadata_sectors must be >= 2")
    if max_bad_sectors < 1 or max_bad_sectors > 255:
        sys.exit(f"error: max_bad_sectors={max_bad_sectors} out of range [1, 255]")
    if mirror_count > 1 and mirror_count % 2 == 0:
        print(f"warning: mirror_count={mirror_count} is even — majority voting disabled; "
              "consider an odd value", file=sys.stderr)

    data_types = z.get("data_types", [])
    parsed_types = []
    for dt in data_types:
        name   = dt.get("name")
        fields = dt.get("fields", [])
        if not name:
            sys.exit("error: data_type entry missing 'name'")
        parsed_fields = []
        wire_total = 0
        for f in fields:
            fname = f.get("name")
            ftype = f.get("type")
            if not fname or not ftype:
                sys.exit(f"error: field in {name} missing 'name' or 'type'")
            if ftype not in KNOWN_TYPES:
                sys.exit(f"error: unknown type '{ftype}' for field {name}.{fname}")
            wire_total += WIRE_SIZES[ftype]
            parsed_fields.append({"name": fname, "type": ftype})
        # Q3: validate that the wire size fits within one PAYLOAD_SIZE
        payload_size = sector_size - header_size - 4
        if wire_total > payload_size:
            sys.exit(f"error: {name} wire_size={wire_total} exceeds PAYLOAD_SIZE={payload_size}")
        if wire_total == 0:
            sys.exit(f"error: {name} has no fields")
        parsed_types.append({"name": name, "fields": parsed_fields,
                              "wire_size": wire_total})

    return {
        "sector_size":      sector_size,
        "mirror_count":     mirror_count,
        "header_size":      header_size,
        "metadata_sectors": metadata_sectors,
        "max_bad_sectors":  max_bad_sectors,
        "data_types":       parsed_types,
    }


# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------

FLOAT_PACK_TEMPLATE = """\
static inline void pack_{name}_{field}_le(uint8_t out[4], float v) {{
    uint32_t u; memcpy(&u, &v, 4);
    out[0]=(uint8_t)(u&0xFF); out[1]=(uint8_t)(u>>8&0xFF);
    out[2]=(uint8_t)(u>>16&0xFF); out[3]=(uint8_t)(u>>24&0xFF);
}}
"""

def gen_config_h(cfg: dict, out_path: str) -> None:
    sector_size      = cfg["sector_size"]
    mirror_count     = cfg["mirror_count"]
    header_size      = cfg["header_size"]
    metadata_sectors = cfg["metadata_sectors"]
    max_bad_sectors  = cfg["max_bad_sectors"]

    # Derived metadata constants (format v4 — 8-byte magic prefix + 64-bit LBA per copy slot)
    meta_format_ver     = 4
    meta_magic_size     = 8   # 4 magic + 2 version + 2 reserved
    meta_copy_slot_base = meta_magic_size                                    # = 8
    meta_copy_stride    = 10  # 8 bytes LBA + 2 bytes version
    meta_copies         = 3
    meta_write_pos_off  = meta_copy_slot_base + meta_copies * meta_copy_stride   # 38
    meta_flags_off      = meta_write_pos_off + 2                                  # 40
    meta_hdr_size       = meta_flags_off + 1                                      # 41
    msg_log_cap_s0      = sector_size - meta_hdr_size
    msg_log_cap_s1     = sector_size
    msg_log_total_cap  = msg_log_cap_s0 + msg_log_cap_s1

    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "#include <stdbool.h>",
        "",
        "/* =========================",
        "   Sector geometry",
        "   ========================= */",
        f"#ifndef SECTOR_SIZE",
        f"#define SECTOR_SIZE {sector_size}u",
        "#endif",
        "",
        f"#ifndef HEADER_SIZE",
        f"#define HEADER_SIZE {header_size}u",
        "#endif",
        "",
        "/* CRC is stored in last 4 bytes of sector */",
        "#ifndef PAYLOAD_SIZE",
        f"#define PAYLOAD_SIZE (SECTOR_SIZE - HEADER_SIZE - 4u)",
        "#endif",
        "",
        "/* Default mirror count (used as fallback; overridden at runtime via zinf_ctx) */",
        "#ifndef RAID_MIRRORS",
        f"#define RAID_MIRRORS {mirror_count}u",
        "#endif",
        "",
        "/* Maximum supported mirror count — raid_read() candidates[] is sized for this */",
        "#define MAX_MIRRORS 5u",
        "",
        "/* Bad-sector blacklist capacity in zinf_ctx_t (from zinf.yaml max_bad_sectors) */",
        f"#define MAX_BAD_SECTORS {max_bad_sectors}u",
        "",
        "/* =========================",
        "   Metadata sector layout (format v4 — magic prefix + 64-bit LBA)",
        "   ---------------------------------",
        "   [0..3]    Magic bytes: 'Z' 'I' 'N' 'F'  (0x5A 0x49 0x4E 0x46)",
        "   [4..5]    Format version: 4 (uint16 LE)",
        "   [6..7]    Reserved (0x00 0x00)",
        "   Each of META_COPIES copy-slots holds (starting at byte META_COPY_SLOT_BASE):",
        "     [+0..+7]  last_sector (64-bit LE)",
        "     [+8..+9]  version     (16-bit LE, monotonic)",
        "   Followed by:",
        f"     [{meta_write_pos_off}..{meta_write_pos_off+1}]  write_pos   (16-bit LE)",
        f"     [{meta_flags_off}]      flags       (1 byte)",
        f"     [{meta_hdr_size}..{sector_size-1}] message log payload",
        "   ========================= */",
        f"#define META_MAGIC_B0       0x5Au  /* 'Z' */",
        f"#define META_MAGIC_B1       0x49u  /* 'I' */",
        f"#define META_MAGIC_B2       0x4Eu  /* 'N' */",
        f"#define META_MAGIC_B3       0x46u  /* 'F' */",
        f"#define META_FORMAT_VER     {meta_format_ver}u",
        f"#define META_MAGIC_SIZE     {meta_magic_size}u     /* 4 magic + 2 version + 2 reserved */",
        f"#define META_COPY_SLOT_BASE META_MAGIC_SIZE                            /* = {meta_copy_slot_base}  */",
        f"#define META_COPY_STRIDE    {meta_copy_stride}u                                        /* bytes per copy slot */",
        f"#define META_COPIES          {meta_copies}u                                        /* redundant copies    */",
        f"#define META_WRITE_POS_OFF  (META_COPY_SLOT_BASE + META_COPIES * META_COPY_STRIDE) /* = {meta_write_pos_off} */",
        f"#define META_FLAGS_OFF      (META_WRITE_POS_OFF + 2u)                              /* = {meta_flags_off} */",
        f"#define META_HDR_SIZE       (META_FLAGS_OFF + 1u)                                  /* = {meta_hdr_size} */",
        "",
        "/* Message log capacity */",
        f"#define MSG_LOG_CAP_S0      (SECTOR_SIZE - META_HDR_SIZE)           /* {msg_log_cap_s0} bytes  */",
        f"#define MSG_LOG_CAP_S1      SECTOR_SIZE                              /* {msg_log_cap_s1} bytes  */",
        f"#define MSG_LOG_TOTAL_CAP   (MSG_LOG_CAP_S0 + MSG_LOG_CAP_S1)       /* {msg_log_total_cap} bytes  */",
        "",
        "/* =========================",
        "   Storage return codes",
        "   ========================= */",
        "#define STORAGE_OK                0u",
        "#define STORAGE_ERR_PARAM         1u",
        "#define STORAGE_ERR_DRIVER        2u",
        "#define STORAGE_ERR_LOG_FULL      3u",
        "#define STORAGE_ERR_UNRECOVERABLE 4u",
        "/* Non-fatal: write committed to >=1 mirror but fewer than mirror_count */",
        "#define STORAGE_WARN_DEGRADED     5u",
        "/* Fatal: write would exceed device capacity or cross the mirror boundary */",
        "#define STORAGE_ERR_FULL          6u",
        "",
        "/* Driver return codes */",
        "#define DRIVER_OK        0",
        "#define DRIVER_ERR_IO    1",
        "#define DRIVER_ERR_INIT  2",
        "#define DRIVER_ERR_PARAM 3",
        "",
        "/* =========================",
        "   Data types",
        "   ========================= */",
    ]

    for dt in cfg["data_types"]:
        lines.append(f"typedef struct {dt['name']} {{")
        for f in dt["fields"]:
            lines.append(f"    {f['type']} {f['name']};")
        lines.append(f"}} {dt['name']};")
        lines.append("")

    lines += [
        "/* =========================",
        "   Runtime context",
        "   ========================= */",
        "struct driver_t;",
        "",
        "typedef struct zinf_ctx_t {",
        "    struct driver_t *driver;",
        "    uint32_t         sector_size;      /* bytes per sector (default SECTOR_SIZE) */",
        "    uint8_t          mirror_count;     /* number of RAID mirrors                 */",
        "    uint8_t          metadata_sectors; /* sectors reserved for metadata          */",
        "    uint64_t         mirror_offset;    /* sectors between mirror copies          */",
        "    uint64_t         log_sector;       /* LBA of metadata sector (default 0)     */",
        "    uint64_t         raid_offset;      /* runtime-computed mirror spacing        */",
        "    /* bad-sector blacklist — RAM only; rebuilt via zinf_scrub() at startup */",
        "    uint64_t         bad_sectors[MAX_BAD_SECTORS];",
        "    uint8_t          bad_sector_count;",
        "} zinf_ctx_t;",
        "",
        "/* Default global instance (set by platform file) */",
        "extern zinf_ctx_t *zinf_ctx;",
        "",
        "/* Initialise *ctx from compile-time defaults.",
        "   Caller must set ctx->driver before calling this. */",
        "void zinf_ctx_init_defaults(zinf_ctx_t *ctx);",
        "",
        "/* =========================",
        "   Bad-sector blacklist helpers (static inline — available everywhere config.h is included)",
        "   ========================= */",
        "static inline bool zinf_is_bad_sector(const zinf_ctx_t *ctx, uint64_t lba) {",
        "    for (uint8_t _i = 0; _i < ctx->bad_sector_count; _i++)",
        "        if (ctx->bad_sectors[_i] == lba) return true;",
        "    return false;",
        "}",
        "",
        "static inline uint8_t zinf_mark_bad_sector(zinf_ctx_t *ctx, uint64_t lba) {",
        "    if (zinf_is_bad_sector(ctx, lba))             return STORAGE_OK;",
        "    if (ctx->bad_sector_count >= MAX_BAD_SECTORS) return STORAGE_ERR_PARAM;",
        "    ctx->bad_sectors[ctx->bad_sector_count++] = lba;",
        "    return STORAGE_OK;",
        "}",
        "",
        "static inline void zinf_clear_bad_sectors(zinf_ctx_t *ctx) {",
        "    ctx->bad_sector_count = 0u;",
        "}",
        "",
    ]

    os.makedirs(out_path, exist_ok=True)
    with open(os.path.join(out_path, "config.h"), "w") as fh:
        fh.write("\n".join(lines))

    print(f"  wrote {os.path.join(out_path, 'config.h')}")


def _emit_to_wire(dt: dict) -> list:
    """Generate a <name>_to_wire(uint8_t *out, const <name> *s) function."""
    name   = dt["name"]
    lines  = [
        f"/* Pack {name} into wire format (little-endian). */",
        f"size_t {name}_to_wire(uint8_t *out, const {name} *s) {{",
    ]
    offset = 0
    for f in dt["fields"]:
        sz    = WIRE_SIZES[f["type"]]
        fname = f["name"]
        ftype = f["type"]
        if ftype in FLOAT_TYPES:
            uint_t = f"uint{sz*8}_t"
            lines.append(f"    {{ {uint_t} _u; memcpy(&_u, &s->{fname}, {sz}); "
                         + "".join(
                             f"out[{offset+b}]=(uint8_t)((_u>>{b*8})&0xFFu); "
                             for b in range(sz)
                         ) + "}")
        else:
            for b in range(sz):
                lines.append(f"    out[{offset+b}] = (uint8_t)((s->{fname} >> {b*8}) & 0xFFu);")
        offset += sz
    lines += [
        f"    return {offset}u;",
        "}",
        "",
    ]
    return lines


def gen_config_c(cfg: dict, out_path: str) -> None:
    metadata_sectors = cfg["metadata_sectors"]
    data_types       = cfg["data_types"]

    lines = [
        '#include "config.h"',
        '#include <string.h>',
        "",
        "/* zinf_ctx is defined in the platform file.",
        "   This translation unit only provides the initialiser helper. */",
        "",
        "void zinf_ctx_init_defaults(zinf_ctx_t *ctx) {",
        "    ctx->sector_size      = SECTOR_SIZE;",
        "    ctx->mirror_count     = (uint8_t)RAID_MIRRORS;",
        "    if (ctx->mirror_count > MAX_MIRRORS) ctx->mirror_count = (uint8_t)MAX_MIRRORS;",
        f"    ctx->metadata_sectors = {metadata_sectors}u;",
        "    ctx->log_sector       = 0u;",
        "    if (ctx->raid_offset == 0u) ctx->raid_offset = 30u;",
        "    ctx->mirror_offset    = ctx->raid_offset;",
        "    ctx->bad_sector_count = 0u;",
        "}",
        "",
    ]

    # Q4: generate serialization helper for each data_type
    for dt in data_types:
        lines += _emit_to_wire(dt)

    os.makedirs(out_path, exist_ok=True)
    with open(os.path.join(out_path, "config.c"), "w") as fh:
        fh.write("\n".join(lines))

    print(f"  wrote {os.path.join(out_path, 'config.c')}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(f"usage: {sys.argv[0]} <zinf.yaml> <out_dir/>")

    yaml_path = sys.argv[1]
    out_dir   = sys.argv[2]

    with open(yaml_path) as fh:
        raw = yaml.safe_load(fh)

    cfg = validate(raw)

    print(f"zinf_gen: generating from {yaml_path}")
    gen_config_h(cfg, out_dir)
    gen_config_c(cfg, out_dir)
    print("zinf_gen: done")


if __name__ == "__main__":
    main()
