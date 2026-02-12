CC = gcc

CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wundef \
         -std=c11 -O2 \
         -I. \
         -I./include \
         -I./config \
         -I./core \
         -I./core/cli \
         -I./core/data \
         -I./core/log \
         -I./core/helper \
         -I./drivers \
         -I./drivers/linux

CORE_SRC = \
    platform_linux.c \
    config/config.c \
    core/cli/cli.c \
    core/data/data.c \
    core/log/log.c \
    core/helper/helper.c \
    drivers/linux/*.c


ZINF_SRC   = main.c   $(CORE_SRC)
READER_SRC = reader.c $(CORE_SRC)

ZINF_OUT   = zinf
READER_OUT = reader

all: $(ZINF_OUT) $(READER_OUT)

$(ZINF_OUT): $(ZINF_SRC)
	$(CC) $(CFLAGS) $(ZINF_SRC) -o $(ZINF_OUT)

$(READER_OUT): $(READER_SRC)
	$(CC) $(CFLAGS) $(READER_SRC) -o $(READER_OUT)

run: $(ZINF_OUT)
	sudo ./$(ZINF_OUT)

run-reader: $(READER_OUT)
	./$(READER_OUT)

clean:
	rm -f $(ZINF_OUT) $(READER_OUT)
