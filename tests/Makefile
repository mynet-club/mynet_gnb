CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L
.PHONY: check
check:
	@mkdir -p ../build/tests
	$(CC) $(CFLAGS) -I../src test_utc_policy.c ../src/gnb_utc.c -pthread -o ../build/tests/test_utc_policy
	../build/tests/test_utc_policy
