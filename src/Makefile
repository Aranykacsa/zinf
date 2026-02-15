CC = gcc

CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wundef \
         -std=c11 -O2 \
         -I. \
         -I./config \
         -I./core \
         -I./core/api \
         -I./core/storage \
         -I./core/helper \
         -I./platform \
         -I./drivers \
         -I./drivers/linux

SRC = \
    apps/zinf_main.c \
    platform/platform_linux.c \
    config/config.c \
    core/api/api.c \
    core/storage/storage.c \
    core/helper/helper.c \
    drivers/linux/*.c

OUT = zinf_cli

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: all
	sudo ./$(OUT) bench

clean:
	rm -f $(OUT)
