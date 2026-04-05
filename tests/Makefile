CC     = gcc
SRCDIR = ../src

CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wundef \
         -std=c11 -O2 -g \
         -I. \
         -I$(SRCDIR)/config \
         -I$(SRCDIR)/core \
         -I$(SRCDIR)/core/api \
         -I$(SRCDIR)/core/storage \
         -I$(SRCDIR)/core/helper \
         -I$(SRCDIR)/platform \
         -I$(SRCDIR)/drivers \
         -I$(SRCDIR)/drivers/linux

SRC = runner.c \
      test_crc.c \
      test_storage.c \
      test_fault.c \
      $(SRCDIR)/config/config.c \
      $(SRCDIR)/core/api/api.c \
      $(SRCDIR)/core/storage/storage.c \
      $(SRCDIR)/core/helper/helper.c \
      $(SRCDIR)/drivers/linux/linux_driver.c \
      $(SRCDIR)/platform/platform_linux.c

OUT = run_tests

.PHONY: all run clean

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: $(OUT)
	./$(OUT)

clean:
	rm -f $(OUT) /var/tmp/zinf_test.img /var/tmp/zinf_fault.img
