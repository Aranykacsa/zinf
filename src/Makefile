CC = gcc

CFLAGS_COMMON = -Wall -Wextra -Wpedantic -Wshadow -Wundef \
                -std=c11 -O2 \
                -I. \
                -I./include \
                -I./config \
                -I./core \
                -I./core/cli \
                -I./core/data \
                -I./core/log \
                -I./core/helper \
                -I./platform \
                -I./apps \
                -I./drivers \
                -I./drivers/linux \
                -I./drivers/embedded

# ----------------------------
# Shared core sources
# ----------------------------
CORE_SRC = \
    config/config.c \
    core/cli/cli.c \
    core/data/data.c \
    core/log/log.c \
    core/helper/helper.c

# ----------------------------
# PC / Linux flavor
# ----------------------------
LINUX_PLATFORM_SRC = platform/platform_linux.c
LINUX_DRIVER_SRC   = drivers/linux/*.c

# Apps (PC)
APP_CLI_SRC   = apps/zinf_cli_main.c
APP_READER_SRC= apps/zinf_reader_main.c
APP_BENCH_SRC = apps/zinf_bench_main.c

# Outputs
OUT_CLI   = zinf
OUT_READER= reader
OUT_BENCH = bench

# Default: build PC tools
all: cli reader bench

cli:
	$(CC) $(CFLAGS_COMMON) -DZINF_PLATFORM_LINUX=1 \
		$(APP_CLI_SRC) $(LINUX_PLATFORM_SRC) $(CORE_SRC) $(LINUX_DRIVER_SRC) \
		-o $(OUT_CLI)

reader:
	$(CC) $(CFLAGS_COMMON) -DZINF_PLATFORM_LINUX=1 \
		$(APP_READER_SRC) $(LINUX_PLATFORM_SRC) $(CORE_SRC) $(LINUX_DRIVER_SRC) \
		-o $(OUT_READER)

bench:
	$(CC) $(CFLAGS_COMMON) -DZINF_PLATFORM_LINUX=1 \
		$(APP_BENCH_SRC) $(LINUX_PLATFORM_SRC) $(CORE_SRC) $(LINUX_DRIVER_SRC) \
		-o $(OUT_BENCH)

run: cli
	sudo ./$(OUT_CLI) cli

run-bench: bench
	sudo ./$(OUT_BENCH)

clean:
	rm -f $(OUT_CLI) $(OUT_READER) $(OUT_BENCH)
