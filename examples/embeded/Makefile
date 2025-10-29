# ===== Embedded firmware build =====
CC := arm-none-eabi-gcc
CFLAGS := -Wall -Wextra -std=c11 -O2 \
          -mcpu=cortex-m0plus -mthumb \
          -I../../src/include
SRC := ../../src/core/*.c ../../src/platform/embedded/*.c main.c
OUT := ../../build/bin/myfs.elf

$(OUT): $(SRC)
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(OUT)

