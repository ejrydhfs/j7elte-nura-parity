# driver slice: host-side only (never builds the kernel tree)
# - submit_fix_test: pure-C models of the SUBMIT-deadlock rules
#   (resv-lock discipline, resume-error abort, timeout-fence signal,
#   bounded recovery, signal idempotence). cc + ./run, zero warnings.

CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

all: check

check: submit_fix_test
	./submit_fix_test

submit_fix_test: submit_fix_test.c
	$(CC) $(CFLAGS) -o $@ submit_fix_test.c

clean:
	rm -f submit_fix_test

.PHONY: all check clean
