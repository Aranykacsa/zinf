#define _GNU_SOURCE
#include "api.h"
#include "config.h"
#include "linux_driver.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <xlsxwriter.h>

/* -----------------------------------------------------------------------
   Constants
   ----------------------------------------------------------------------- */

#define CSV_IMG_PATH  "/var/tmp/zinf_csv_fault.img"
#define CSV_IMG_SECTS 4096u
#define MAX_FIELD     256
#define MAX_LINE      1024
#define MAX_SCENARIOS 512
#define MAX_RESULTS   4096

/* -----------------------------------------------------------------------
   Excel colour palette
   ----------------------------------------------------------------------- */

#define XL_COLOR_PASS_ROW   0xC6EFCEu  /* light green  */
#define XL_COLOR_WARN_ROW   0xFFEB9Cu  /* light yellow */
#define XL_COLOR_FAIL_ROW   0xFFC7CEu  /* light pink   */
#define XL_COLOR_PASS_DARK  0x375623u  /* dark green text  */
#define XL_COLOR_WARN_DARK  0x7F6000u  /* dark amber text  */
#define XL_COLOR_FAIL_DARK  0x9C0006u  /* dark red text    */
#define XL_COLOR_CRC_CELL   0xF4B183u  /* orange — CRC_FAIL */
#define XL_COLOR_BL_CELL    0xBFBFBFu  /* grey — BLACKLIST  */
#define XL_COLOR_IOERR_CELL 0xFFC7CEu  /* red — IO_ERR      */
#define XL_COLOR_NA_CELL    0xEDEDEDu  /* light grey — N/A  */
#define XL_COLOR_HDR_BG     0x4472C4u  /* blue header bg    */
#define XL_COLOR_HDR_FG     0xFFFFFFu  /* white header text */
#define XL_COLOR_FUZZ_BG    0xD9E1F2u  /* light blue — fuzz */
#define XL_COLOR_EDGE_BG    0xEDEDEDu  /* light grey — edge */

/* -----------------------------------------------------------------------
   Enumerations
   ----------------------------------------------------------------------- */

typedef enum {
    FAULT_NONE                   = 0,
    FAULT_SEU                    = 1,
    FAULT_TORN_WRITE             = 2,
    FAULT_ZERO_FILL              = 3,
    FAULT_BLACKLIST_BEFORE_WRITE = 4
} fault_type_t;

typedef enum {
    ACTION_CHECK_ONLY = 0,
    ACTION_RECOVER    = 1,
    ACTION_RAID_READ  = 2
} action_t;

/* -----------------------------------------------------------------------
   Scenario struct  (one row from fault_scenarios.csv or generated)
   ----------------------------------------------------------------------- */

typedef struct {
    int           id;
    char          name[MAX_FIELD];
    uint8_t       mirror_count;
    fault_type_t  fault_type;
    char          affected_mirrors[MAX_FIELD];
    uint32_t      byte_offset;
    uint32_t      byte_count;
    uint8_t       corruption_byte;
    action_t      action;
    uint8_t       expected_write_rc;
    uint8_t       expected_action_rc;
    uint8_t       expected_valid_before;
    uint8_t       expected_valid_after;
    char          description[MAX_FIELD * 2];
} scenario_t;

/* -----------------------------------------------------------------------
   Result struct — one entry per run (scenario × repeat)
   ----------------------------------------------------------------------- */

typedef struct {
    scenario_t sc;
    int        is_fuzz;
    int        is_edge;
    int        run_number;
    uint32_t   rng_seed;
    char       timestamp[32];
    uint8_t    write_rc_actual;
    uint8_t    before_status[MAX_MIRRORS];
    uint8_t    before_valid;
    uint8_t    action_rc_actual;
    uint8_t    after_status[MAX_MIRRORS];
    uint8_t    after_valid;
    uint8_t    bad_count_after;
    uint64_t   bad_lbas[MAX_BAD_SECTORS];
    int        pass_write_rc;
    int        pass_action_rc;
    int        pass_valid_before;
    int        pass_valid_after;
    int        overall_pass;
    char       failure_detail[512];
} result_t;

/* -----------------------------------------------------------------------
   CLI options
   ----------------------------------------------------------------------- */

typedef struct {
    const char *scenarios_path;
    const char *output_path;
    int         fuzz_count;
    int         fixed_repeat;
    int         fuzz_repeat;
    int         skip_edge;
    int         skip_fuzz;
    uint32_t    rng_seed;
    int         verbose;
} cli_opts_t;

/* -----------------------------------------------------------------------
   Excel format bundle
   ----------------------------------------------------------------------- */

typedef struct {
    lxw_format *header;
    lxw_format *row_pass;
    lxw_format *row_warn;
    lxw_format *row_fail;
    lxw_format *row_fuzz;
    lxw_format *row_edge;
    lxw_format *status_pass;
    lxw_format *status_warn;
    lxw_format *status_fail;
    lxw_format *cell_crc;
    lxw_format *cell_bl;
    lxw_format *cell_ioerr;
    lxw_format *cell_na;
    lxw_format *cell_pass;
    lxw_format *cell_fail;
    lxw_format *normal;
} xl_fmts_t;

/* -----------------------------------------------------------------------
   Global state
   ----------------------------------------------------------------------- */

static zinf_ctx_t g_ctx;
static result_t   g_results[MAX_RESULTS];
static int        g_result_count = 0;
static uint32_t   g_rng_state    = 0;

/* -----------------------------------------------------------------------
   Setup / teardown
   ----------------------------------------------------------------------- */

static int csv_setup(uint8_t mirror_count) {
    int fd = open(CSV_IMG_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) { perror("open csv img"); return -1; }
    if (ftruncate(fd, (off_t)CSV_IMG_SECTS * SECTOR_SIZE) != 0) {
        perror("ftruncate"); close(fd); return -1;
    }
    close(fd);

    linux_driver_set_path(CSV_IMG_PATH);

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.driver           = &linux_driver;
    g_ctx.sector_size      = SECTOR_SIZE;
    g_ctx.mirror_count     = mirror_count;
    g_ctx.metadata_sectors = 2;
    g_ctx.mirror_offset    = (CSV_IMG_SECTS - 2u) / (uint32_t)mirror_count;
    g_ctx.log_sector       = 0;
    g_ctx.raid_offset      = g_ctx.mirror_offset;

    if (linux_driver.init(&linux_driver) != DRIVER_OK) return -1;
    if (init_log_sector(&g_ctx) != STORAGE_OK) return -1;
    return 0;
}

static void csv_teardown(void) {
    if (linux_driver.deinit) linux_driver.deinit(&linux_driver);
    unlink(CSV_IMG_PATH);
}

/* -----------------------------------------------------------------------
   Corruption helper
   ----------------------------------------------------------------------- */

static int corrupt_bytes(uint64_t lba, uint32_t byte_off,
                          uint32_t count, uint8_t pattern) {
    if (count == 0) return 0;
    if (byte_off >= SECTOR_SIZE) return 0;
    if (byte_off + count > SECTOR_SIZE) count = SECTOR_SIZE - byte_off;

    int fd = open(CSV_IMG_PATH, O_RDWR);
    if (fd < 0) return -1;

    uint8_t buf[SECTOR_SIZE];
    memset(buf, pattern, sizeof(buf));

    off_t   off = (off_t)lba * SECTOR_SIZE + (off_t)byte_off;
    ssize_t w   = pwrite(fd, buf, count, off);
    close(fd);
    return (w == (ssize_t)count) ? 0 : -1;
}

/* -----------------------------------------------------------------------
   String ↔ constant converters
   ----------------------------------------------------------------------- */

static const char *rc_str(uint8_t rc) {
    switch (rc) {
        case STORAGE_OK:                return "STORAGE_OK";
        case STORAGE_ERR_PARAM:         return "STORAGE_ERR_PARAM";
        case STORAGE_ERR_DRIVER:        return "STORAGE_ERR_DRIVER";
        case STORAGE_ERR_LOG_FULL:      return "STORAGE_ERR_LOG_FULL";
        case STORAGE_ERR_UNRECOVERABLE: return "STORAGE_ERR_UNRECOVERABLE";
        case STORAGE_WARN_DEGRADED:     return "STORAGE_WARN_DEGRADED";
        default:                        return "UNKNOWN";
    }
}

static const char *mirror_status_str(uint8_t s) {
    switch (s) {
        case ZINF_MIRROR_OK:        return "OK";
        case ZINF_MIRROR_IO_ERR:    return "IO_ERR";
        case ZINF_MIRROR_CRC_FAIL:  return "CRC_FAIL";
        case ZINF_MIRROR_BLACKLIST: return "BLACKLIST";
        default:                    return "UNKNOWN";
    }
}

static const char *fault_type_str(fault_type_t f) {
    switch (f) {
        case FAULT_SEU:                    return "SEU";
        case FAULT_TORN_WRITE:             return "TORN_WRITE";
        case FAULT_ZERO_FILL:              return "ZERO_FILL";
        case FAULT_BLACKLIST_BEFORE_WRITE: return "BLACKLIST_BEFORE_WRITE";
        default:                           return "NONE";
    }
}

static const char *action_str(action_t a) {
    switch (a) {
        case ACTION_RECOVER:   return "RECOVER";
        case ACTION_RAID_READ: return "RAID_READ";
        default:               return "CHECK_ONLY";
    }
}

static uint8_t str_to_rc(const char *s) {
    if (!s) return 0xFFu;
    if (strcmp(s, "STORAGE_OK")                == 0) return STORAGE_OK;
    if (strcmp(s, "STORAGE_ERR_PARAM")         == 0) return STORAGE_ERR_PARAM;
    if (strcmp(s, "STORAGE_ERR_DRIVER")        == 0) return STORAGE_ERR_DRIVER;
    if (strcmp(s, "STORAGE_ERR_LOG_FULL")      == 0) return STORAGE_ERR_LOG_FULL;
    if (strcmp(s, "STORAGE_ERR_UNRECOVERABLE") == 0) return STORAGE_ERR_UNRECOVERABLE;
    if (strcmp(s, "STORAGE_WARN_DEGRADED")     == 0) return STORAGE_WARN_DEGRADED;
    return 0xFFu;
}

static fault_type_t str_to_fault(const char *s) {
    if (!s)                                       return FAULT_NONE;
    if (strcmp(s, "SEU")                    == 0) return FAULT_SEU;
    if (strcmp(s, "TORN_WRITE")             == 0) return FAULT_TORN_WRITE;
    if (strcmp(s, "ZERO_FILL")              == 0) return FAULT_ZERO_FILL;
    if (strcmp(s, "BLACKLIST_BEFORE_WRITE") == 0) return FAULT_BLACKLIST_BEFORE_WRITE;
    return FAULT_NONE;
}

static action_t str_to_action(const char *s) {
    if (!s)                            return ACTION_CHECK_ONLY;
    if (strcmp(s, "RECOVER")   == 0)   return ACTION_RECOVER;
    if (strcmp(s, "RAID_READ") == 0)   return ACTION_RAID_READ;
    return ACTION_CHECK_ONLY;
}

static uint8_t str_to_pattern(const char *s) {
    if (!s || strcmp(s, "NONE") == 0) return 0x00u;
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
        return (uint8_t)strtoul(s, NULL, 16);
    return 0x00u;
}

/* -----------------------------------------------------------------------
   CSV line splitter
   ----------------------------------------------------------------------- */

static int split_csv(char *line, char **fields, int max_fields) {
    char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
    nl = strchr(line, '\r');       if (nl) *nl = '\0';

    int   n = 0;
    char *p = line;
    while (n < max_fields) {
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return n;
}

/* -----------------------------------------------------------------------
   Parse one CSV data line into scenario_t
   ----------------------------------------------------------------------- */

static int parse_scenario(char *line, scenario_t *sc) {
    char *f[14];
    int nf = split_csv(line, f, 14);
    if (nf < 13) return -1;

    sc->id = atoi(f[0]);
    if (sc->id <= 0) return -1;

    strncpy(sc->name,             f[1],  MAX_FIELD     - 1);
    sc->mirror_count          = (uint8_t)atoi(f[2]);
    sc->fault_type            = str_to_fault(f[3]);
    strncpy(sc->affected_mirrors, f[4],  MAX_FIELD     - 1);
    sc->byte_offset           = (uint32_t)atoi(f[5]);
    sc->byte_count            = (uint32_t)atoi(f[6]);
    sc->corruption_byte       = str_to_pattern(f[7]);
    sc->action                = str_to_action(f[8]);
    sc->expected_write_rc     = str_to_rc(f[9]);
    sc->expected_action_rc    = str_to_rc(f[10]);
    sc->expected_valid_before = (uint8_t)atoi(f[11]);
    sc->expected_valid_after  = (uint8_t)atoi(f[12]);
    if (nf >= 14)
        strncpy(sc->description, f[13], MAX_FIELD * 2 - 1);
    else
        sc->description[0] = '\0';

    return 0;
}

/* -----------------------------------------------------------------------
   Parse affected_mirrors field into physical LBA list
   ----------------------------------------------------------------------- */

static int parse_affected_lbas(const char *affected, uint64_t logical,
                                uint8_t mirror_count, uint64_t *lba_out) {
    int count = 0;
    if (!affected || strcmp(affected, "NONE") == 0) return 0;

    if (strcmp(affected, "ALL") == 0) {
        for (uint8_t m = 0; m < mirror_count && m < MAX_MIRRORS; m++)
            lba_out[count++] = logical + (uint64_t)m * g_ctx.mirror_offset;
        return count;
    }

    char tmp[MAX_FIELD];
    strncpy(tmp, affected, MAX_FIELD - 1);
    char *tok = strtok(tmp, ":");
    while (tok && count < MAX_MIRRORS) {
        int m = atoi(tok);
        if (m >= 0 && m < (int)mirror_count)
            lba_out[count++] = logical + (uint64_t)m * g_ctx.mirror_offset;
        tok = strtok(NULL, ":");
    }
    return count;
}

/* -----------------------------------------------------------------------
   Count affected mirrors from the affected_mirrors string
   ----------------------------------------------------------------------- */

static int count_affected_mirrors(const char *affected, uint8_t mirror_count) {
    if (!affected || strcmp(affected, "NONE") == 0) return 0;
    if (strcmp(affected, "ALL") == 0) return (int)mirror_count;
    int count = 0;
    char tmp[MAX_FIELD];
    strncpy(tmp, affected, MAX_FIELD - 1);
    char *tok = strtok(tmp, ":");
    while (tok) { count++; tok = strtok(NULL, ":"); }
    return count;
}

/* -----------------------------------------------------------------------
   Derive expected values from scenario parameters
   (used by fuzz and edge-case generators)
   ----------------------------------------------------------------------- */

static void derive_expected(scenario_t *sc) {
    int n = count_affected_mirrors(sc->affected_mirrors, sc->mirror_count);

    if (sc->fault_type == FAULT_NONE) {
        sc->expected_write_rc     = STORAGE_OK;
        sc->expected_valid_before = sc->mirror_count;
        sc->expected_action_rc    = STORAGE_OK;
        sc->expected_valid_after  = sc->mirror_count;
        return;
    }

    if (sc->fault_type == FAULT_BLACKLIST_BEFORE_WRITE) {
        if (n >= (int)sc->mirror_count) {
            sc->expected_write_rc     = STORAGE_ERR_DRIVER;
            sc->expected_valid_before = 0;
        } else {
            sc->expected_write_rc     = STORAGE_WARN_DEGRADED;
            sc->expected_valid_before = sc->mirror_count - (uint8_t)n;
        }
        sc->expected_action_rc   = STORAGE_OK;
        sc->expected_valid_after = sc->expected_valid_before;
        return;
    }

    /* SEU, TORN_WRITE, ZERO_FILL */
    sc->expected_write_rc     = STORAGE_OK;
    sc->expected_valid_before = sc->mirror_count - (uint8_t)n;

    if (sc->action == ACTION_RECOVER) {
        if (n < (int)sc->mirror_count) {
            sc->expected_action_rc   = STORAGE_OK;
            sc->expected_valid_after = sc->mirror_count;
        } else {
            sc->expected_action_rc   = STORAGE_ERR_UNRECOVERABLE;
            sc->expected_valid_after = 0;
        }
    } else if (sc->action == ACTION_RAID_READ) {
        int valid = (int)sc->mirror_count - n;
        if (valid <= 0) {
            sc->expected_action_rc = STORAGE_ERR_UNRECOVERABLE;
        } else if (valid == 1 || sc->mirror_count < 3) {
            /* raid_read returns immediately for single valid or 2-mirror config */
            sc->expected_action_rc = STORAGE_OK;
        } else {
            /* Majority voting: need votes >= mirror_count/2+1.
               All valid mirrors carry identical data, so votes == valid. */
            int threshold = (int)sc->mirror_count / 2 + 1;
            sc->expected_action_rc = (valid >= threshold)
                                     ? STORAGE_OK
                                     : STORAGE_ERR_UNRECOVERABLE;
        }
        sc->expected_valid_after = sc->expected_valid_before;
    } else {
        /* CHECK_ONLY */
        sc->expected_action_rc   = STORAGE_OK;
        sc->expected_valid_after = sc->expected_valid_before;
    }
}

/* -----------------------------------------------------------------------
   LCG RNG
   ----------------------------------------------------------------------- */

static uint32_t lcg_rand(void) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state;
}

/* -----------------------------------------------------------------------
   Edge-case scenario generator
   Sweeps mirror_count × fault_type × boundary_offset systematically.
   ----------------------------------------------------------------------- */

static void gen_edge_cases(scenario_t *buf, int *count, int max) {
    static const uint8_t     mc_opts[] = {2, 3};
    static const fault_type_t ft_opts[] = {FAULT_SEU, FAULT_TORN_WRITE, FAULT_ZERO_FILL};
    static const uint32_t  off_opts[] = {0, 1, 253, 506, 507, 508, 511};

    int id = 1000;
    for (int mi = 0; mi < 2 && *count < max; mi++) {
        for (int fi = 0; fi < 3 && *count < max; fi++) {
            for (int oi = 0; oi < 7 && *count < max; oi++) {
                scenario_t *sc = &buf[(*count)++];
                memset(sc, 0, sizeof(*sc));
                sc->id           = id++;
                sc->mirror_count = mc_opts[mi];
                sc->fault_type   = ft_opts[fi];
                strncpy(sc->affected_mirrors, "0", MAX_FIELD - 1);
                sc->byte_offset  = off_opts[oi];
                sc->action       = ACTION_RECOVER;

                if (ft_opts[fi] == FAULT_SEU) {
                    sc->byte_count     = 1;
                    sc->corruption_byte = 0xDEu;
                } else {
                    /* TORN_WRITE and ZERO_FILL: corrupt from offset to end of sector */
                    uint32_t rem = SECTOR_SIZE - off_opts[oi];
                    sc->byte_count     = (rem > 0) ? rem : 1u;
                    sc->corruption_byte = 0x00u;
                }

                snprintf(sc->name, MAX_FIELD, "edge_%um_%s_off%u",
                         mc_opts[mi], fault_type_str(ft_opts[fi]), off_opts[oi]);
                snprintf(sc->description, MAX_FIELD * 2 - 1,
                         "Edge sweep: %s at offset %u, mirror_count=%u",
                         fault_type_str(ft_opts[fi]), off_opts[oi], mc_opts[mi]);

                derive_expected(sc);
            }
        }
    }
}

/* -----------------------------------------------------------------------
   Fuzz scenario generator
   Produces N random scenarios with expected values derived automatically.
   ----------------------------------------------------------------------- */

static void gen_fuzz_scenarios(scenario_t *buf, int *count, int n, uint32_t seed) {
    g_rng_state = seed;

    static const uint8_t      mc_opts[] = {2, 3, 4, 5};
    static const fault_type_t ft_opts[] = {
        FAULT_NONE, FAULT_SEU, FAULT_TORN_WRITE,
        FAULT_ZERO_FILL, FAULT_BLACKLIST_BEFORE_WRITE
    };
    static const action_t act_opts[] = {
        ACTION_CHECK_ONLY, ACTION_RECOVER, ACTION_RAID_READ
    };

    for (int i = 0; i < n && *count < MAX_RESULTS; i++) {
        scenario_t *sc = &buf[(*count)++];
        memset(sc, 0, sizeof(*sc));

        sc->id           = 2000 + i;
        sc->mirror_count = mc_opts[lcg_rand() % 4];
        sc->fault_type   = ft_opts[lcg_rand() % 5];

        /* BLACKLIST only makes sense with CHECK_ONLY action */
        if (sc->fault_type == FAULT_BLACKLIST_BEFORE_WRITE ||
            sc->fault_type == FAULT_NONE)
            sc->action = ACTION_CHECK_ONLY;
        else
            sc->action = act_opts[lcg_rand() % 3];

        /* Pick affected mirrors */
        if (sc->fault_type == FAULT_NONE) {
            strncpy(sc->affected_mirrors, "NONE", MAX_FIELD - 1);
        } else {
            int n_aff = (int)(lcg_rand() % sc->mirror_count) + 1;
            if (n_aff == (int)sc->mirror_count) {
                strncpy(sc->affected_mirrors, "ALL", MAX_FIELD - 1);
            } else {
                uint8_t used[MAX_MIRRORS] = {0};
                char aff[MAX_FIELD]       = "";
                int picked = 0;
                while (picked < n_aff) {
                    uint8_t m = (uint8_t)(lcg_rand() % sc->mirror_count);
                    if (!used[m]) {
                        used[m] = 1;
                        if (picked > 0)
                            strncat(aff, ":", MAX_FIELD - strlen(aff) - 1);
                        char tmp[4];
                        snprintf(tmp, sizeof(tmp), "%u", m);
                        strncat(aff, tmp, MAX_FIELD - strlen(aff) - 1);
                        picked++;
                    }
                }
                strncpy(sc->affected_mirrors, aff, MAX_FIELD - 1);
            }
        }

        sc->byte_offset    = lcg_rand() % SECTOR_SIZE;
        uint32_t max_count = SECTOR_SIZE - sc->byte_offset;
        sc->byte_count     = (max_count > 0)
                             ? (lcg_rand() % max_count) + 1u
                             : 1u;
        sc->corruption_byte = (uint8_t)(lcg_rand() % 256u);

        snprintf(sc->name, MAX_FIELD, "fuzz_%04d", i + 1);
        snprintf(sc->description, MAX_FIELD * 2 - 1,
                 "Fuzz mc=%u ft=%s aff=%s off=%u cnt=%u pat=0x%02X act=%s",
                 sc->mirror_count, fault_type_str(sc->fault_type),
                 sc->affected_mirrors, sc->byte_offset, sc->byte_count,
                 sc->corruption_byte, action_str(sc->action));

        derive_expected(sc);
    }
}

/* -----------------------------------------------------------------------
   Execute one scenario and store result in g_results[]
   ----------------------------------------------------------------------- */

static void run_scenario_to_result(const scenario_t *sc, int run_number,
                                   int is_fuzz, int is_edge,
                                   uint32_t rng_seed, int verbose) {
    if (g_result_count >= MAX_RESULTS) {
        fprintf(stderr, "[csv] result buffer full, skipping scenario %d\n", sc->id);
        return;
    }

    if (csv_setup(sc->mirror_count) != 0) {
        fprintf(stderr, "[csv] SKIP scenario %d (%s): setup failed\n",
                sc->id, sc->name);
        return;
    }

    result_t *r = &g_results[g_result_count];
    memset(r, 0, sizeof(*r));
    r->sc         = *sc;
    r->run_number = run_number;
    r->is_fuzz    = is_fuzz;
    r->is_edge    = is_edge;
    r->rng_seed   = rng_seed;

    /* Timestamp */
    time_t     now     = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(r->timestamp, sizeof(r->timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const uint64_t logical = 1u;

    /* Step 1: pre-write blacklist */
    if (sc->fault_type == FAULT_BLACKLIST_BEFORE_WRITE) {
        uint64_t lbas[MAX_MIRRORS];
        int n = parse_affected_lbas(sc->affected_mirrors, logical,
                                    sc->mirror_count, lbas);
        for (int i = 0; i < n; i++)
            zinf_mark_bad_sector(&g_ctx, lbas[i]);
    }

    /* Step 2: write */
    sensor_t s = { .temp = 23.5f, .humidity = 65.0f };
    r->write_rc_actual = raid_sensor_values(&g_ctx, &s, 1);

    /* Step 3: inject fault */
    if (sc->fault_type == FAULT_SEU       ||
        sc->fault_type == FAULT_TORN_WRITE ||
        sc->fault_type == FAULT_ZERO_FILL) {
        uint64_t lbas[MAX_MIRRORS];
        int n = parse_affected_lbas(sc->affected_mirrors, logical,
                                    sc->mirror_count, lbas);
        for (int i = 0; i < n; i++)
            corrupt_bytes(lbas[i], sc->byte_offset,
                          sc->byte_count, sc->corruption_byte);
    }

    /* Step 4: health check BEFORE action */
    zinf_sector_health_t before;
    memset(&before, ZINF_MIRROR_IO_ERR, sizeof(before));
    zinf_check_sector(&g_ctx, logical, &before);
    memcpy(r->before_status, before.status, MAX_MIRRORS);
    r->before_valid = before.valid_count;

    /* Step 5: execute test action */
    switch (sc->action) {
        case ACTION_RECOVER:
            r->action_rc_actual = zinf_recover_sector(&g_ctx, logical);
            break;
        case ACTION_RAID_READ: {
            uint8_t payload[PAYLOAD_SIZE];
            r->action_rc_actual = raid_read(&g_ctx, logical, payload);
            break;
        }
        default:
            r->action_rc_actual = STORAGE_OK;
            break;
    }

    /* Step 6: health check AFTER action */
    zinf_sector_health_t after;
    memset(&after, ZINF_MIRROR_IO_ERR, sizeof(after));
    zinf_check_sector(&g_ctx, logical, &after);
    memcpy(r->after_status, after.status, MAX_MIRRORS);
    r->after_valid = after.valid_count;

    /* Step 7: collect blacklist state */
    r->bad_count_after = g_ctx.bad_sector_count;
    for (uint8_t i = 0; i < g_ctx.bad_sector_count && i < MAX_BAD_SECTORS; i++)
        r->bad_lbas[i] = g_ctx.bad_sectors[i];

    /* Step 8: evaluate checkpoints */
    r->pass_write_rc     = (r->write_rc_actual  == sc->expected_write_rc)  ? 1 : 0;
    r->pass_action_rc    = (r->action_rc_actual  == sc->expected_action_rc) ? 1 : 0;
    r->pass_valid_before = (r->before_valid == sc->expected_valid_before) ? 1 : 0;
    r->pass_valid_after  = (r->after_valid  == sc->expected_valid_after)  ? 1 : 0;
    r->overall_pass      = r->pass_write_rc && r->pass_action_rc &&
                           r->pass_valid_before && r->pass_valid_after;

    /* Build failure detail */
    if (r->overall_pass) {
        strcpy(r->failure_detail, "NONE");
    } else {
        r->failure_detail[0] = '\0';
        if (!r->pass_write_rc)
            snprintf(r->failure_detail + strlen(r->failure_detail),
                     sizeof(r->failure_detail) - strlen(r->failure_detail),
                     "write_rc: exp %s got %s; ",
                     rc_str(sc->expected_write_rc), rc_str(r->write_rc_actual));
        if (!r->pass_action_rc)
            snprintf(r->failure_detail + strlen(r->failure_detail),
                     sizeof(r->failure_detail) - strlen(r->failure_detail),
                     "action_rc: exp %s got %s; ",
                     rc_str(sc->expected_action_rc), rc_str(r->action_rc_actual));
        if (!r->pass_valid_before)
            snprintf(r->failure_detail + strlen(r->failure_detail),
                     sizeof(r->failure_detail) - strlen(r->failure_detail),
                     "valid_before: exp %u got %u; ",
                     (unsigned)sc->expected_valid_before,
                     (unsigned)r->before_valid);
        if (!r->pass_valid_after)
            snprintf(r->failure_detail + strlen(r->failure_detail),
                     sizeof(r->failure_detail) - strlen(r->failure_detail),
                     "valid_after: exp %u got %u",
                     (unsigned)sc->expected_valid_after,
                     (unsigned)r->after_valid);
    }

    csv_teardown();
    g_result_count++;

    if (verbose) {
        printf("  [%s] sc=%d run=%d %s\n",
               r->overall_pass ? " OK " : "FAIL",
               sc->id, run_number, sc->name);
        if (!r->overall_pass)
            printf("       %s\n", r->failure_detail);
    }
}

/* -----------------------------------------------------------------------
   Excel format creation
   ----------------------------------------------------------------------- */

static void xl_create_formats(lxw_workbook *wb, xl_fmts_t *f) {
    /* Header */
    f->header = workbook_add_format(wb);
    format_set_bg_color(f->header, XL_COLOR_HDR_BG);
    format_set_font_color(f->header, XL_COLOR_HDR_FG);
    format_set_bold(f->header);
    format_set_border(f->header, LXW_BORDER_THIN);

    /* Row backgrounds */
    f->row_pass = workbook_add_format(wb);
    format_set_bg_color(f->row_pass, XL_COLOR_PASS_ROW);
    format_set_border(f->row_pass, LXW_BORDER_THIN);

    f->row_warn = workbook_add_format(wb);
    format_set_bg_color(f->row_warn, XL_COLOR_WARN_ROW);
    format_set_border(f->row_warn, LXW_BORDER_THIN);

    f->row_fail = workbook_add_format(wb);
    format_set_bg_color(f->row_fail, XL_COLOR_FAIL_ROW);
    format_set_border(f->row_fail, LXW_BORDER_THIN);

    f->row_fuzz = workbook_add_format(wb);
    format_set_bg_color(f->row_fuzz, XL_COLOR_FUZZ_BG);
    format_set_border(f->row_fuzz, LXW_BORDER_THIN);

    f->row_edge = workbook_add_format(wb);
    format_set_bg_color(f->row_edge, XL_COLOR_EDGE_BG);
    format_set_border(f->row_edge, LXW_BORDER_THIN);

    /* Status cell formats (row bg + bold text) */
    f->status_pass = workbook_add_format(wb);
    format_set_bg_color(f->status_pass, XL_COLOR_PASS_ROW);
    format_set_font_color(f->status_pass, XL_COLOR_PASS_DARK);
    format_set_bold(f->status_pass);
    format_set_border(f->status_pass, LXW_BORDER_THIN);

    f->status_warn = workbook_add_format(wb);
    format_set_bg_color(f->status_warn, XL_COLOR_WARN_ROW);
    format_set_font_color(f->status_warn, XL_COLOR_WARN_DARK);
    format_set_bold(f->status_warn);
    format_set_border(f->status_warn, LXW_BORDER_THIN);

    f->status_fail = workbook_add_format(wb);
    format_set_bg_color(f->status_fail, XL_COLOR_FAIL_ROW);
    format_set_font_color(f->status_fail, XL_COLOR_FAIL_DARK);
    format_set_bold(f->status_fail);
    format_set_border(f->status_fail, LXW_BORDER_THIN);

    /* Mirror status cell colours */
    f->cell_crc = workbook_add_format(wb);
    format_set_bg_color(f->cell_crc, XL_COLOR_CRC_CELL);
    format_set_border(f->cell_crc, LXW_BORDER_THIN);

    f->cell_bl = workbook_add_format(wb);
    format_set_bg_color(f->cell_bl, XL_COLOR_BL_CELL);
    format_set_border(f->cell_bl, LXW_BORDER_THIN);

    f->cell_ioerr = workbook_add_format(wb);
    format_set_bg_color(f->cell_ioerr, XL_COLOR_IOERR_CELL);
    format_set_border(f->cell_ioerr, LXW_BORDER_THIN);

    f->cell_na = workbook_add_format(wb);
    format_set_bg_color(f->cell_na, XL_COLOR_NA_CELL);
    format_set_font_color(f->cell_na, 0x808080u);
    format_set_border(f->cell_na, LXW_BORDER_THIN);

    /* Pass / fail text formats for RC columns */
    f->cell_pass = workbook_add_format(wb);
    format_set_font_color(f->cell_pass, XL_COLOR_PASS_DARK);
    format_set_bold(f->cell_pass);
    format_set_border(f->cell_pass, LXW_BORDER_THIN);

    f->cell_fail = workbook_add_format(wb);
    format_set_font_color(f->cell_fail, XL_COLOR_FAIL_DARK);
    format_set_bold(f->cell_fail);
    format_set_border(f->cell_fail, LXW_BORDER_THIN);

    /* Normal */
    f->normal = workbook_add_format(wb);
    format_set_border(f->normal, LXW_BORDER_THIN);
}

/* -----------------------------------------------------------------------
   Helper: choose row background format for a result
   ----------------------------------------------------------------------- */

static lxw_format *row_fmt(const result_t *r, const xl_fmts_t *f) {
    if (r->is_fuzz) return f->row_fuzz;
    if (r->is_edge) return f->row_edge;
    if (!r->overall_pass) return f->row_fail;
    if (r->sc.expected_write_rc  == STORAGE_WARN_DEGRADED ||
        r->sc.expected_action_rc == STORAGE_WARN_DEGRADED)
        return f->row_warn;
    return f->row_pass;
}

/* -----------------------------------------------------------------------
   Helper: format for a mirror status cell
   ----------------------------------------------------------------------- */

static lxw_format *mirror_fmt(uint8_t status, int used, const xl_fmts_t *f) {
    if (!used) return f->cell_na;
    switch (status) {
        case ZINF_MIRROR_CRC_FAIL:  return f->cell_crc;
        case ZINF_MIRROR_BLACKLIST: return f->cell_bl;
        case ZINF_MIRROR_IO_ERR:    return f->cell_ioerr;
        default:                    return f->normal;
    }
}

/* -----------------------------------------------------------------------
   Overview sheet — 1 row per unique scenario, aggregated over K runs
   ----------------------------------------------------------------------- */

static void xl_write_overview(lxw_workbook *wb, lxw_worksheet *ws,
                               const xl_fmts_t *f) {
    (void)wb;
    /* Column headers */
    static const char *hdr[] = {
        "ID", "Name", "Type", "FaultType", "Mirrors", "Action",
        "WriteRC_Exp", "ActionRC_Exp", "ValidBefore_Exp", "ValidAfter_Exp",
        "Runs", "Passed", "Failed", "Warn/Degraded", "Pass%", "Status"
    };
    int ncols = 16;
    for (int c = 0; c < ncols; c++)
        worksheet_write_string(ws, 0, (lxw_col_t)c, hdr[c], f->header);

    worksheet_freeze_panes(ws, 1, 0);
    worksheet_autofilter(ws, 0, 0, 0, (lxw_col_t)(ncols - 1));

    /* Set column widths */
    worksheet_set_column(ws, 0, 0, 5,  NULL);
    worksheet_set_column(ws, 1, 1, 32, NULL);
    worksheet_set_column(ws, 2, 2, 8,  NULL);
    worksheet_set_column(ws, 3, 3, 22, NULL);
    worksheet_set_column(ws, 4, 4, 7,  NULL);
    worksheet_set_column(ws, 5, 5, 12, NULL);
    worksheet_set_column(ws, 6, 7, 24, NULL);
    worksheet_set_column(ws, 8, 9, 14, NULL);
    worksheet_set_column(ws, 10, 14, 8, NULL);
    worksheet_set_column(ws, 15, 15, 8, NULL);

    /* One row per unique scenario: group consecutive results with same id */
    int row = 1;
    int i   = 0;
    while (i < g_result_count) {
        int sc_id  = g_results[i].sc.id;
        int passes = 0, fails = 0, warns = 0, total = 0;
        int j = i;
        while (j < g_result_count && g_results[j].sc.id == sc_id) {
            total++;
            if (g_results[j].overall_pass)
                passes++;
            else
                fails++;
            if (g_results[j].sc.expected_write_rc  == STORAGE_WARN_DEGRADED ||
                g_results[j].sc.expected_action_rc == STORAGE_WARN_DEGRADED)
                warns++;
            j++;
        }
        const result_t *r0 = &g_results[i];
        const scenario_t *sc = &r0->sc;

        /* Determine row status */
        const char *status_text;
        lxw_format *status_f;
        lxw_format *rf;
        if (fails > 0) {
            status_text = "FAIL"; status_f = f->status_fail; rf = f->row_fail;
        } else if (warns > 0) {
            status_text = "WARN"; status_f = f->status_warn; rf = f->row_warn;
        } else {
            status_text = "PASS"; status_f = f->status_pass; rf = f->row_pass;
        }
        if (r0->is_fuzz) rf = f->row_fuzz;
        if (r0->is_edge) rf = f->row_edge;

        const char *type_str = r0->is_fuzz ? "Fuzz"
                             : r0->is_edge ? "Edge"
                             : "Fixed";

        double pass_pct = (total > 0) ? (100.0 * passes / total) : 0.0;

        lxw_row_t xlrow = (lxw_row_t)row;
        worksheet_write_number(ws, xlrow, 0,  sc->id, rf);
        worksheet_write_string(ws, xlrow, 1,  sc->name, rf);
        worksheet_write_string(ws, xlrow, 2,  type_str, rf);
        worksheet_write_string(ws, xlrow, 3,  fault_type_str(sc->fault_type), rf);
        worksheet_write_number(ws, xlrow, 4,  sc->mirror_count, rf);
        worksheet_write_string(ws, xlrow, 5,  action_str(sc->action), rf);
        worksheet_write_string(ws, xlrow, 6,  rc_str(sc->expected_write_rc), rf);
        worksheet_write_string(ws, xlrow, 7,  rc_str(sc->expected_action_rc), rf);
        worksheet_write_number(ws, xlrow, 8,  sc->expected_valid_before, rf);
        worksheet_write_number(ws, xlrow, 9,  sc->expected_valid_after, rf);
        worksheet_write_number(ws, xlrow, 10, total, rf);
        worksheet_write_number(ws, xlrow, 11, passes, rf);
        worksheet_write_number(ws, xlrow, 12, fails, rf);
        worksheet_write_number(ws, xlrow, 13, warns, rf);
        worksheet_write_number(ws, xlrow, 14, pass_pct, rf);
        worksheet_write_string(ws, xlrow, 15, status_text, status_f);

        row++;
        i = j;
    }
}

/* -----------------------------------------------------------------------
   Details sheet — 1 row per run
   ----------------------------------------------------------------------- */

static void xl_write_details(lxw_workbook *wb, lxw_worksheet *ws,
                              const xl_fmts_t *f) {
    (void)wb;
    static const char *hdr[] = {
        "RunID", "ScenID", "ScenName", "Type", "FaultType",
        "Mirrors", "Action", "Run#",
        "WriteRC_Exp", "WriteRC_Act", "WriteRC_Pass",
        "ValidBef_Exp", "ValidBef_Act", "ValidBef_Pass",
        "ActionRC_Exp", "ActionRC_Act", "ActionRC_Pass",
        "ValidAft_Exp", "ValidAft_Act", "ValidAft_Pass",
        "Bef_M0", "Bef_M1", "Bef_M2", "Bef_M3", "Bef_M4",
        "Aft_M0", "Aft_M1", "Aft_M2", "Aft_M3", "Aft_M4",
        "BadSectors", "BadLBAs", "Overall", "FailureDetail"
    };
    int ncols = 34;
    for (int c = 0; c < ncols; c++)
        worksheet_write_string(ws, 0, (lxw_col_t)c, hdr[c], f->header);

    worksheet_freeze_panes(ws, 1, 0);
    worksheet_autofilter(ws, 0, 0, 0, (lxw_col_t)(ncols - 1));

    /* Column widths */
    worksheet_set_column(ws, 0, 1, 7,   NULL);
    worksheet_set_column(ws, 2, 2, 30,  NULL);
    worksheet_set_column(ws, 3, 3, 7,   NULL);
    worksheet_set_column(ws, 4, 4, 22,  NULL);
    worksheet_set_column(ws, 5, 7, 8,   NULL);
    worksheet_set_column(ws, 8, 16, 22, NULL);
    worksheet_set_column(ws, 17, 19, 14, NULL);
    worksheet_set_column(ws, 20, 29, 10, NULL);
    worksheet_set_column(ws, 30, 30, 10, NULL);
    worksheet_set_column(ws, 31, 31, 20, NULL);
    worksheet_set_column(ws, 32, 32, 8,  NULL);
    worksheet_set_column(ws, 33, 33, 48, NULL);

    for (int i = 0; i < g_result_count; i++) {
        const result_t *r = &g_results[i];
        const scenario_t *sc = &r->sc;
        lxw_row_t xlrow = (lxw_row_t)(i + 1);

        /* Choose row background based on overall pass */
        lxw_format *rf = row_fmt(r, f);

        const char *type_str = r->is_fuzz ? "Fuzz"
                             : r->is_edge ? "Edge"
                             : "Fixed";

        /* Build bad LBAs string */
        char bad_lba_str[256] = "NONE";
        if (r->bad_count_after > 0) {
            bad_lba_str[0] = '\0';
            for (uint8_t k = 0; k < r->bad_count_after; k++) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%s%" PRIu64,
                         k > 0 ? ";" : "", r->bad_lbas[k]);
                strncat(bad_lba_str, tmp,
                        sizeof(bad_lba_str) - strlen(bad_lba_str) - 1);
            }
        }

        worksheet_write_number(ws, xlrow, 0, i + 1, rf);
        worksheet_write_number(ws, xlrow, 1, sc->id, rf);
        worksheet_write_string(ws, xlrow, 2, sc->name, rf);
        worksheet_write_string(ws, xlrow, 3, type_str, rf);
        worksheet_write_string(ws, xlrow, 4, fault_type_str(sc->fault_type), rf);
        worksheet_write_number(ws, xlrow, 5, sc->mirror_count, rf);
        worksheet_write_string(ws, xlrow, 6, action_str(sc->action), rf);
        worksheet_write_number(ws, xlrow, 7, r->run_number, rf);

        /* Write RC */
        worksheet_write_string(ws, xlrow, 8, rc_str(sc->expected_write_rc), rf);
        worksheet_write_string(ws, xlrow, 9, rc_str(r->write_rc_actual), rf);
        worksheet_write_string(ws, xlrow, 10,
            r->pass_write_rc ? "PASS" : "FAIL",
            r->pass_write_rc ? f->cell_pass : f->cell_fail);

        /* Valid before */
        worksheet_write_number(ws, xlrow, 11, sc->expected_valid_before, rf);
        worksheet_write_number(ws, xlrow, 12, r->before_valid, rf);
        worksheet_write_string(ws, xlrow, 13,
            r->pass_valid_before ? "PASS" : "FAIL",
            r->pass_valid_before ? f->cell_pass : f->cell_fail);

        /* Action RC */
        worksheet_write_string(ws, xlrow, 14, rc_str(sc->expected_action_rc), rf);
        worksheet_write_string(ws, xlrow, 15, rc_str(r->action_rc_actual), rf);
        worksheet_write_string(ws, xlrow, 16,
            r->pass_action_rc ? "PASS" : "FAIL",
            r->pass_action_rc ? f->cell_pass : f->cell_fail);

        /* Valid after */
        worksheet_write_number(ws, xlrow, 17, sc->expected_valid_after, rf);
        worksheet_write_number(ws, xlrow, 18, r->after_valid, rf);
        worksheet_write_string(ws, xlrow, 19,
            r->pass_valid_after ? "PASS" : "FAIL",
            r->pass_valid_after ? f->cell_pass : f->cell_fail);

        /* Per-mirror status before/after */
        for (int m = 0; m < MAX_MIRRORS; m++) {
            int used = (m < (int)sc->mirror_count);
            lxw_format *mf_bef = mirror_fmt(r->before_status[m], used, f);
            lxw_format *mf_aft = mirror_fmt(r->after_status[m],  used, f);
            if (used) {
                worksheet_write_string(ws, xlrow, (lxw_col_t)(20 + m),
                    mirror_status_str(r->before_status[m]), mf_bef);
                worksheet_write_string(ws, xlrow, (lxw_col_t)(25 + m),
                    mirror_status_str(r->after_status[m]),  mf_aft);
            } else {
                worksheet_write_string(ws, xlrow, (lxw_col_t)(20 + m), "N/A", mf_bef);
                worksheet_write_string(ws, xlrow, (lxw_col_t)(25 + m), "N/A", mf_aft);
            }
        }

        worksheet_write_number(ws, xlrow, 30, r->bad_count_after, rf);
        worksheet_write_string(ws, xlrow, 31, bad_lba_str, rf);

        lxw_format *overall_f = r->overall_pass ? f->status_pass : f->status_fail;
        worksheet_write_string(ws, xlrow, 32,
            r->overall_pass ? "PASS" : "FAIL", overall_f);
        worksheet_write_string(ws, xlrow, 33, r->failure_detail, rf);
    }
}

/* -----------------------------------------------------------------------
   _ChartData sheet — aggregated tables for chart series
   ----------------------------------------------------------------------- */

static void xl_write_chart_data(lxw_workbook *wb, lxw_worksheet *ws,
                                 const xl_fmts_t *f) {
    (void)wb;
    /* ---- Table A: Pass/Fail by Fault Type (rows 0-7) ---- */
    static const char *ft_names[] = {
        "NONE", "SEU", "TORN_WRITE", "ZERO_FILL", "BLACKLIST", "Edge", "Fuzz"
    };
    worksheet_write_string(ws, 0, 0, "FaultType", f->header);
    worksheet_write_string(ws, 0, 1, "Total",     f->header);
    worksheet_write_string(ws, 0, 2, "Passed",    f->header);
    worksheet_write_string(ws, 0, 3, "Failed",    f->header);

    int ft_total[7] = {0}, ft_pass[7] = {0}, ft_fail[7] = {0};
    for (int i = 0; i < g_result_count; i++) {
        const result_t *r = &g_results[i];
        int idx;
        if      (r->is_edge)                                    idx = 5;
        else if (r->is_fuzz)                                    idx = 6;
        else    idx = (int)r->sc.fault_type; /* 0..4 */
        if (idx < 0 || idx > 6) idx = 0;
        ft_total[idx]++;
        if (r->overall_pass) ft_pass[idx]++; else ft_fail[idx]++;
    }
    for (int k = 0; k < 7; k++) {
        worksheet_write_string(ws, (lxw_row_t)(k + 1), 0, ft_names[k], f->normal);
        worksheet_write_number(ws, (lxw_row_t)(k + 1), 1, ft_total[k], f->normal);
        worksheet_write_number(ws, (lxw_row_t)(k + 1), 2, ft_pass[k],  f->normal);
        worksheet_write_number(ws, (lxw_row_t)(k + 1), 3, ft_fail[k],  f->normal);
    }

    /* ---- Table B: Recovery Rate by Mirror Count (rows 9-15) ---- */
    worksheet_write_string(ws, 9,  0, "Mirrors",   f->header);
    worksheet_write_string(ws, 9,  1, "Scenarios", f->header);
    worksheet_write_string(ws, 9,  2, "PctPassed", f->header);

    int mc_total[6] = {0}, mc_pass[6] = {0};
    for (int i = 0; i < g_result_count; i++) {
        const result_t *r = &g_results[i];
        int mc = (int)r->sc.mirror_count;
        if (mc >= 1 && mc <= 5) {
            mc_total[mc]++;
            if (r->overall_pass) mc_pass[mc]++;
        }
    }
    for (int mc = 1; mc <= 5; mc++) {
        double pct = (mc_total[mc] > 0)
                     ? (100.0 * mc_pass[mc] / mc_total[mc]) : 0.0;
        worksheet_write_number(ws, (lxw_row_t)(9 + mc), 0, mc, f->normal);
        worksheet_write_number(ws, (lxw_row_t)(9 + mc), 1, mc_total[mc], f->normal);
        worksheet_write_number(ws, (lxw_row_t)(9 + mc), 2, pct,          f->normal);
    }

    /* ---- Table C: Avg Valid Before/After — first 30 unique scenarios (rows 17-48) ---- */
    worksheet_write_string(ws, 17, 0, "Scenario",     f->header);
    worksheet_write_string(ws, 17, 1, "AvgValidBef",  f->header);
    worksheet_write_string(ws, 17, 2, "AvgValidAft",  f->header);

    int seen_ids[30], n_seen = 0;
    double sum_bef[30] = {0}, sum_aft[30] = {0};
    int cnt_sc[30] = {0};
    char sc_names[30][MAX_FIELD];

    for (int i = 0; i < g_result_count && n_seen < 30; i++) {
        const result_t *r = &g_results[i];
        int found = -1;
        for (int k = 0; k < n_seen; k++)
            if (seen_ids[k] == r->sc.id) { found = k; break; }
        if (found < 0) {
            found = n_seen++;
            seen_ids[found] = r->sc.id;
            strncpy(sc_names[found], r->sc.name, MAX_FIELD - 1);
        }
        sum_bef[found] += r->before_valid;
        sum_aft[found] += r->after_valid;
        cnt_sc[found]++;
    }
    for (int k = 0; k < n_seen; k++) {
        double avg_bef = (cnt_sc[k] > 0) ? sum_bef[k] / cnt_sc[k] : 0.0;
        double avg_aft = (cnt_sc[k] > 0) ? sum_aft[k] / cnt_sc[k] : 0.0;
        worksheet_write_string(ws, (lxw_row_t)(18 + k), 0, sc_names[k], f->normal);
        worksheet_write_number(ws, (lxw_row_t)(18 + k), 1, avg_bef, f->normal);
        worksheet_write_number(ws, (lxw_row_t)(18 + k), 2, avg_aft, f->normal);
    }

    /* ---- Table D: Failure Checkpoint Breakdown (rows 50-56) ---- */
    static const char *cp_names[] = {
        "write_rc_mismatch", "action_rc_mismatch",
        "valid_before_low", "valid_after_low", "all_passed"
    };
    worksheet_write_string(ws, 50, 0, "Checkpoint", f->header);
    worksheet_write_string(ws, 50, 1, "Count",      f->header);

    int cp_counts[5] = {0};
    for (int i = 0; i < g_result_count; i++) {
        const result_t *r = &g_results[i];
        if (!r->pass_write_rc)     cp_counts[0]++;
        if (!r->pass_action_rc)    cp_counts[1]++;
        if (!r->pass_valid_before) cp_counts[2]++;
        if (!r->pass_valid_after)  cp_counts[3]++;
        if (r->overall_pass)       cp_counts[4]++;
    }
    for (int k = 0; k < 5; k++) {
        worksheet_write_string(ws, (lxw_row_t)(51 + k), 0, cp_names[k], f->normal);
        worksheet_write_number(ws, (lxw_row_t)(51 + k), 1, cp_counts[k], f->normal);
    }
}

/* -----------------------------------------------------------------------
   Charts sheet — 4 embedded charts
   ----------------------------------------------------------------------- */

static void xl_write_charts(lxw_workbook *wb, lxw_worksheet *ws_charts,
                             int n_seen_scenarios) {
    /* Clamp n_seen_scenarios to [1..30] for Table C range */
    if (n_seen_scenarios < 1)  n_seen_scenarios = 1;
    if (n_seen_scenarios > 30) n_seen_scenarios = 30;

    char cat[128], val1[128], val2[128];

    /* Chart 1: Pass/Fail by Fault Type (Bar clustered) */
    lxw_chart *c1 = workbook_add_chart(wb, LXW_CHART_BAR);
    snprintf(cat,  sizeof(cat),  "=_ChartData!$A$2:$A$8");
    snprintf(val1, sizeof(val1), "=_ChartData!$C$2:$C$8");
    snprintf(val2, sizeof(val2), "=_ChartData!$D$2:$D$8");
    lxw_chart_series *s1a = chart_add_series(c1, cat, val1);
    lxw_chart_series *s1b = chart_add_series(c1, cat, val2);
    chart_series_set_name(s1a, "Passed");
    chart_series_set_name(s1b, "Failed");
    chart_title_set_name(c1, "Pass / Fail by Fault Type");
    worksheet_insert_chart(ws_charts, 1, 1, c1);

    /* Chart 2: Recovery Rate by Mirror Count (Column) */
    lxw_chart *c2 = workbook_add_chart(wb, LXW_CHART_COLUMN);
    snprintf(cat,  sizeof(cat),  "=_ChartData!$A$11:$A$15");
    snprintf(val1, sizeof(val1), "=_ChartData!$C$11:$C$15");
    lxw_chart_series *s2 = chart_add_series(c2, cat, val1);
    chart_series_set_name(s2, "Pass %");
    chart_title_set_name(c2, "Pass Rate by Mirror Count");
    worksheet_insert_chart(ws_charts, 1, 9, c2);

    /* Chart 3: Avg Valid Before/After (Bar clustered) */
    lxw_chart *c3 = workbook_add_chart(wb, LXW_CHART_BAR);
    int c3_end = 18 + n_seen_scenarios; /* exclusive, 1-based */
    snprintf(cat,  sizeof(cat),
             "=_ChartData!$A$19:$A$%d", c3_end);
    snprintf(val1, sizeof(val1),
             "=_ChartData!$B$19:$B$%d", c3_end);
    snprintf(val2, sizeof(val2),
             "=_ChartData!$C$19:$C$%d", c3_end);
    lxw_chart_series *s3a = chart_add_series(c3, cat, val1);
    lxw_chart_series *s3b = chart_add_series(c3, cat, val2);
    chart_series_set_name(s3a, "Avg Valid Before");
    chart_series_set_name(s3b, "Avg Valid After");
    chart_title_set_name(c3, "Avg Valid Mirrors Before vs After");
    worksheet_insert_chart(ws_charts, 18, 1, c3);

    /* Chart 4: Failure Checkpoint Breakdown (Pie) */
    lxw_chart *c4 = workbook_add_chart(wb, LXW_CHART_PIE);
    snprintf(cat,  sizeof(cat),  "=_ChartData!$A$52:$A$56");
    snprintf(val1, sizeof(val1), "=_ChartData!$B$52:$B$56");
    lxw_chart_series *s4 = chart_add_series(c4, cat, val1);
    chart_series_set_name(s4, "Checkpoints");
    chart_title_set_name(c4, "Failure Checkpoint Breakdown");
    worksheet_insert_chart(ws_charts, 18, 9, c4);
}

/* -----------------------------------------------------------------------
   Main Excel writer
   ----------------------------------------------------------------------- */

static void write_excel(const char *path) {
    lxw_workbook *wb = workbook_new(path);
    if (!wb) {
        fprintf(stderr, "error: cannot create workbook: %s\n", path);
        return;
    }

    lxw_worksheet *ws_overview   = workbook_add_worksheet(wb, "Overview");
    lxw_worksheet *ws_details    = workbook_add_worksheet(wb, "Details");
    lxw_worksheet *ws_chartdata  = workbook_add_worksheet(wb, "_ChartData");
    lxw_worksheet *ws_charts     = workbook_add_worksheet(wb, "Charts");

    worksheet_hide(ws_chartdata);

    xl_fmts_t f;
    xl_create_formats(wb, &f);

    xl_write_overview(wb, ws_overview, &f);
    xl_write_details(wb, ws_details, &f);
    xl_write_chart_data(wb, ws_chartdata, &f);

    /* Count unique scenarios for chart range */
    int n_seen = 0;
    int seen_ids[30];
    for (int i = 0; i < g_result_count && n_seen < 30; i++) {
        int found = 0;
        for (int k = 0; k < n_seen; k++)
            if (seen_ids[k] == g_results[i].sc.id) { found = 1; break; }
        if (!found) seen_ids[n_seen++] = g_results[i].sc.id;
    }

    xl_write_charts(wb, ws_charts, n_seen);

    workbook_close(wb);
}

/* -----------------------------------------------------------------------
   Console summary
   ----------------------------------------------------------------------- */

static void print_console_summary(void) {
    int total = g_result_count, passed = 0, failed = 0;
    for (int i = 0; i < g_result_count; i++) {
        if (g_results[i].overall_pass) passed++; else failed++;
    }

    /* Count unique scenarios */
    int unique = 0, seen_ids[MAX_RESULTS];
    for (int i = 0; i < g_result_count; i++) {
        int found = 0;
        int sc_id = g_results[i].sc.id;
        for (int k = 0; k < unique; k++)
            if (seen_ids[k] == sc_id) { found = 1; break; }
        if (!found) seen_ids[unique++] = sc_id;
    }

    printf("\n=== Results: %d/%d runs passed (%d unique scenarios) ===\n",
           passed, total, unique);
    if (failed)
        printf("    %d runs FAILED\n", failed);
}

/* -----------------------------------------------------------------------
   CLI argument parser
   ----------------------------------------------------------------------- */

static void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -s <file>    fixed scenarios CSV  (default: fault_scenarios.csv)\n");
    printf("  -o <file>    output xlsx          (default: fault_results.xlsx)\n");
    printf("  -n <N>       fuzz scenario count  (default: 50)\n");
    printf("  -k <K>       fixed repeat count   (default: 1)\n");
    printf("  -K <K>       fuzz repeat count    (default: 3)\n");
    printf("  -E           skip edge-case gen\n");
    printf("  -F           skip fuzz gen\n");
    printf("  -r <seed>    RNG seed             (default: time())\n");
    printf("  -v           verbose per-run output\n");
}

static int parse_args(int argc, char *argv[], cli_opts_t *opts) {
    opts->scenarios_path = "fault_scenarios.csv";
    opts->output_path    = "fault_results.xlsx";
    opts->fuzz_count     = 50;
    opts->fixed_repeat   = 1;
    opts->fuzz_repeat    = 3;
    opts->skip_edge      = 0;
    opts->skip_fuzz      = 0;
    opts->rng_seed       = (uint32_t)time(NULL);
    opts->verbose        = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            opts->scenarios_path = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            opts->output_path = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opts->fuzz_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            opts->fixed_repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
            opts->fuzz_repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-E") == 0) {
            opts->skip_edge = 1;
        } else if (strcmp(argv[i], "-F") == 0) {
            opts->skip_fuzz = 1;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            opts->rng_seed = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return -1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
   Main
   ----------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    cli_opts_t opts;
    if (parse_args(argc, argv, &opts) != 0) return 1;

    printf("=== ZINF CSV Fault Test ===\n");
    printf("  scenarios : %s\n", opts.scenarios_path);
    printf("  output    : %s\n", opts.output_path);
    printf("  rng seed  : %u\n\n", opts.rng_seed);

    /* --- Load fixed scenarios from CSV --- */
    static scenario_t all_scenarios[MAX_SCENARIOS];
    int n_scenarios = 0;

    FILE *scen_fp = fopen(opts.scenarios_path, "r");
    if (!scen_fp) {
        fprintf(stderr, "error: cannot open scenarios file: %s\n",
                opts.scenarios_path);
        return 1;
    }
    char line[MAX_LINE];
    int line_no = 0, header_skipped = 0;
    while (fgets(line, sizeof(line), scen_fp) && n_scenarios < MAX_SCENARIOS) {
        line_no++;
        if (!header_skipped) { header_skipped = 1; continue; }
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') continue;
        scenario_t sc;
        memset(&sc, 0, sizeof(sc));
        if (parse_scenario(line, &sc) == 0)
            all_scenarios[n_scenarios++] = sc;
        else
            fprintf(stderr, "warning: skipping malformed line %d\n", line_no);
    }
    fclose(scen_fp);
    printf("Loaded %d fixed scenarios.\n", n_scenarios);

    /* --- Generate edge-case scenarios --- */
    int n_edge_start = n_scenarios;
    if (!opts.skip_edge) {
        gen_edge_cases(all_scenarios, &n_scenarios, MAX_SCENARIOS);
        printf("Generated %d edge-case scenarios.\n",
               n_scenarios - n_edge_start);
    }

    /* --- Generate fuzz scenarios --- */
    int n_fuzz_start = n_scenarios;
    if (!opts.skip_fuzz) {
        gen_fuzz_scenarios(all_scenarios, &n_scenarios,
                           opts.fuzz_count, opts.rng_seed);
        printf("Generated %d fuzz scenarios.\n",
               n_scenarios - n_fuzz_start);
    }

    printf("Total scenarios: %d\n\n", n_scenarios);

    /* --- Run all scenarios × K repeats --- */
    for (int i = 0; i < n_scenarios; i++) {
        int is_edge = (i >= n_edge_start && i < n_fuzz_start);
        int is_fuzz = (i >= n_fuzz_start);
        int repeats = is_fuzz ? opts.fuzz_repeat : opts.fixed_repeat;

        if (!opts.verbose) {
            printf("[%3d/%3d] %s\n", i + 1, n_scenarios, all_scenarios[i].name);
        }

        for (int k = 1; k <= repeats; k++) {
            run_scenario_to_result(&all_scenarios[i], k,
                                   is_fuzz, is_edge,
                                   opts.rng_seed, opts.verbose);
        }
    }

    /* --- Write Excel output --- */
    printf("\nWriting %s ...\n", opts.output_path);
    write_excel(opts.output_path);

    print_console_summary();

    printf("  full results: %s\n", opts.output_path);
    return 0;
}
