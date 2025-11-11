CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wundef \
         -std=c11 -O2 \
         -I./drivers \
         -I./config \
         -I./core \
         -I./core/storage \
         -I./core/helper \
         -I./drivers/linux \
         -I./include


SRC = main.c \
    $(wildcard core/**/*.c) \
    $(wildcard config/*.c) \
    $(wildcard drivers/linux/*.c)
OUT = zinf

all:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: all
	sudo ./$(OUT)

clean:
	rm -f $(OUT)
