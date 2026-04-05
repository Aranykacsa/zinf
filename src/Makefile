PYTHON ?= python3
YAML   ?= ../zinf.yaml
TOOLS  ?= ../tools
CFGDIR  = config

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

.PHONY: all gen run clean

all: $(OUT)

# Regenerate config.h / config.c from zinf.yaml whenever the YAML changes
$(CFGDIR)/config.h $(CFGDIR)/config.c: $(YAML) $(TOOLS)/zinf_gen.py
	$(PYTHON) $(TOOLS)/zinf_gen.py $(YAML) $(CFGDIR)/

$(OUT): $(CFGDIR)/config.h
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

gen:
	$(PYTHON) $(TOOLS)/zinf_gen.py $(YAML) $(CFGDIR)/

run: all
	sudo ./$(OUT) bench

clean:
	rm -f $(OUT)
