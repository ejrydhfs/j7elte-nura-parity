# driver slice: host-side only (never builds the kernel tree)
# - submit_fix_test: pure-C models of the SUBMIT-deadlock rules
#   (resv-lock discipline, resume-error abort, timeout-fence signal,
#   bounded recovery, signal idempotence). cc + ./run, zero warnings.
# - chainsplit_test: real jm_hw chain helpers + a model of the split
#   submit (per-node queue order, previous-fence dependencies, single
#   out-fence on the last node, error unwinding).

CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

all: check

check: submit_fix_test chainsplit_test
	./submit_fix_test
	./chainsplit_test

submit_fix_test: submit_fix_test.c
	$(CC) $(CFLAGS) -o $@ submit_fix_test.c

chainsplit_test: chainsplit_test.c ../jobs/panfrost7580_jm_hw.h
	$(CC) $(CFLAGS) -o $@ chainsplit_test.c

clean:
	rm -f submit_fix_test chainsplit_test

.PHONY: all check clean
