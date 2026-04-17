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

CSV_SRC = test_fault_csv.c \
          $(SRCDIR)/config/config.c \
          $(SRCDIR)/core/api/api.c \
          $(SRCDIR)/core/storage/storage.c \
          $(SRCDIR)/core/helper/helper.c \
          $(SRCDIR)/drivers/linux/linux_driver.c \
          $(SRCDIR)/platform/platform_linux.c

CSV_OUT = run_fault_csv

XLSX_LIBS := $(shell pkg-config --libs xlsxwriter 2>/dev/null || echo -lxlsxwriter)

.PHONY: all run csv csv-fuzz clean

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: $(OUT)
	./$(OUT)

$(CSV_OUT): $(CSV_SRC)
	$(CC) $(CFLAGS) $(CSV_SRC) -o $(CSV_OUT) $(XLSX_LIBS)

csv: $(CSV_OUT)
	./$(CSV_OUT) -s fault_scenarios.csv -o fault_results.xlsx

csv-fuzz: $(CSV_OUT)
	./$(CSV_OUT) -s fault_scenarios.csv -o fault_results_fuzz.xlsx -n 100 -K 5 -v

clean:
	rm -f $(OUT) $(CSV_OUT) fault_results.xlsx fault_results_fuzz.xlsx \
	      /var/tmp/zinf_test.img /var/tmp/zinf_fault.img /var/tmp/zinf_csv_fault.img
