# ===== Tests =====
CC := gcc
CFLAGS := -Wall -Wextra -I../src/include -std=c11 -O2
SRC := test_crc.c test_mount.c test_rw.c ../src/core/*.c ../src/platform/desktop/*.c
OUT := ../build/bin/tests

$(OUT): $(SRC)
	mkdir -p ../build/bin
	$(CC) $(CFLAGS) $^ -o $@

run: $(OUT)
	@echo "🧪 Running tests..."
	@./$(OUT)

clean:
	rm -f $(OUT)

