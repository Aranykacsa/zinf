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
         -I$(SRCDIR)/drivers/linux \
         -I$(SRCDIR)/drivers/mock

SRC = runner.c \
      test_crc.c \
      test_storage.c \
      test_fault.c \
      $(SRCDIR)/config/config.c \
      $(SRCDIR)/core/api/api.c \
      $(SRCDIR)/core/storage/storage.c \
      $(SRCDIR)/core/helper/helper.c \
      $(SRCDIR)/drivers/linux/linux_driver.c \
      $(SRCDIR)/drivers/mock/ram_driver.c \
      $(SRCDIR)/platform/platform_linux.c

OUT = run_tests

CSV_SRC = test_fault_csv.c \
          $(SRCDIR)/config/config.c \
          $(SRCDIR)/core/api/api.c \
          $(SRCDIR)/core/storage/storage.c \
          $(SRCDIR)/core/helper/helper.c \
          $(SRCDIR)/drivers/linux/linux_driver.c \
          $(SRCDIR)/drivers/mock/ram_driver.c \
          $(SRCDIR)/platform/platform_linux.c

CSV_OUT = run_fault_csv

XLSX_LIBS := $(shell pkg-config --libs xlsxwriter 2>/dev/null || echo -lxlsxwriter)

REAL_SRC = test_realistic.c \
           $(SRCDIR)/config/config.c \
           $(SRCDIR)/core/api/api.c \
           $(SRCDIR)/core/storage/storage.c \
           $(SRCDIR)/core/helper/helper.c \
           $(SRCDIR)/drivers/linux/linux_driver.c \
           $(SRCDIR)/drivers/mock/ram_driver.c \
           $(SRCDIR)/platform/platform_linux.c

REAL_OUT = run_realistic

.PHONY: all run csv csv-fuzz realistic realistic-brutal clean

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: $(OUT)
	./$(OUT)

$(CSV_OUT): $(CSV_SRC)
	$(CC) $(CFLAGS) $(CSV_SRC) -o $(CSV_OUT) $(XLSX_LIBS)

csv: $(CSV_OUT)
	./$(CSV_OUT) -M -n 100000 -o fault_results

csv-fuzz: $(CSV_OUT)
	./$(CSV_OUT) -M -R -n 1000000 -r 42 -o fault_results_fuzz

$(REAL_OUT): $(REAL_SRC)
	$(CC) $(CFLAGS) $(REAL_SRC) -o $(REAL_OUT) -lm

realistic: $(REAL_OUT)
	./$(REAL_OUT) -n 1000 -v

realistic-brutal: $(REAL_OUT)
	./$(REAL_OUT) -n 5000 -e 0.05 -v

clean:
	rm -f $(OUT) $(CSV_OUT) $(REAL_OUT) *.csv.gz \
	      /var/tmp/zinf_test.img /var/tmp/zinf_fault.img /var/tmp/zinf_csv_fault.img
