# ZINF — top-level Makefile
#
#   make                  build zinf and zinf-probe
#   sudo make install     build + install to /usr/local/bin
#   sudo make uninstall   remove installed files
#   make clean            remove build artifacts
#
# After install, use:
#   sudo zinf format /dev/sdX
#   zinf info /dev/sdX
#   sudo zinf bench /dev/sdX > results.csv

.PHONY: all install uninstall clean install-studio

all:
	$(MAKE) -C src

install:
	$(MAKE) -C src install

uninstall:
	$(MAKE) -C src uninstall

clean:
	$(MAKE) -C src clean

install-studio:
	@echo "Building ZINF Studio..."
	cd zinf-studio && bun install && bun run tauri build
	tools/install.sh
