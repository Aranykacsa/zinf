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

OUT = zinf

PREFIX ?= /usr/local

PROBE_SRC = ../tools/zinf-probe.c
PROBE_OUT = zinf_probe

.PHONY: all gen run probe install install-probe install-blkid uninstall clean

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

probe: $(PROBE_OUT)

$(PROBE_OUT): $(PROBE_SRC)
	$(CC) -std=c11 -O2 -Wall $(PROBE_SRC) -o $(PROBE_OUT)

install-probe: probe
	install -m 755 $(PROBE_OUT) /usr/local/bin/zinf-probe
	install -m 644 $(TOOLS)/99-zinf.rules /etc/udev/rules.d/
	udevadm control --reload-rules
	@echo "zinf-probe installed. Re-insert your SD card or run: udevadm trigger --subsystem-match=block"

install: all probe
	install -m 755 $(OUT)       $(PREFIX)/bin/zinf
	install -m 755 $(PROBE_OUT) $(PREFIX)/bin/zinf-probe
	install -m 644 $(TOOLS)/99-zinf.rules /etc/udev/rules.d/
	udevadm control --reload-rules
	@echo ""
	@echo "ZINF installed. Quick start:"
	@echo "  sudo zinf format /dev/sdX"
	@echo "  zinf info /dev/sdX"
	@echo "  lsblk --properties-by file,udev -f    (shows FSTYPE=zinf)"
	@echo "  sudo make install-blkid               (for standard lsblk -f)"

uninstall:
	rm -f $(PREFIX)/bin/zinf $(PREFIX)/bin/zinf-probe
	rm -f /etc/udev/rules.d/99-zinf.rules
	udevadm control --reload-rules
	@echo "ZINF uninstalled."

install-blkid:
	sudo bash $(TOOLS)/install-libblkid-zinf.sh

clean:
	rm -f $(OUT) $(PROBE_OUT)
