CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 \
         -I./drivers \
         -I./drivers/linux \
         -I./core \
         -I./include

SRC = main.c ./core/storage.c ./core/helper.c ./drivers/linux/linux_driver.c
OUT = zinf

all:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: all
	sudo ./$(OUT)

clean:
	rm -f $(OUT)

