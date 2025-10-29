# zinf
Zinf is not FAT

```bash
myfs/
├── Makefile                # top-level dispatcher
├── README.md
├── docs/
│   ├── design.md
│   ├── api.md
│   └── fs_layout.svg
│
├── src/
│   ├── core/
│   │   ├── myfs.c
│   │   ├── myfs_block.c
│   │   ├── myfs_crc.c
│   │   └── myfs_config.c
│   │
│   ├── include/
│   │   ├── myfs.h
│   │   ├── myfs_config.h
│   │   ├── myfs_platform.h
│   │   └── myfs_types.h
│   │
│   ├── platform/
│   │   ├── embedded/
│   │   │   ├── myfs_platform.c
│   │   │   └── myfs_hal.c
│   │   └── desktop/
│   │       ├── myfs_platform.c
│   │       └── mock_disk.c
│   │
│   └── utils/
│       ├── log.c
│       └── hexdump.c
│
├── tests/
│   ├── test_crc.c
│   ├── test_mount.c
│   ├── test_rw.c
│   └── Makefile
│
├── examples/
│   ├── embedded/
│   │   ├── main.c
│   │   └── Makefile
│   └── desktop/
│       ├── simulate.c
│       └── Makefile
│
├── tools/
│   ├── mkfs_myfs.c
│   ├── dump_myfs.c
│   └── Makefile
│
└── scripts/
    ├── build.sh
    ├── run_tests.sh
    └── ci_check.sh

```
