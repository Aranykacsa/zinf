# ===== Desktop simulator build =====
CC := gcc
CFLAGS := -Wall -Wextra -I../../src/include -std=c11 -O2
SRC := ../../src/core/*.c ../../src/platform/desktop/*.c ../../src/utils/*.c simulate.c
OUT := ../../build/bin/simulate

$(OUT): $(SRC)
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $^ -o $@

run: $(OUT)
	@echo "🏃 Running simulator..."
	@./$(OUT)

clean:
	rm -f $(OUT)

