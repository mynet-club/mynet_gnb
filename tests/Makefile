CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L
GNB_HEADER_FLAGS := -std=c11 -Wno-unused-variable
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
GNB_HEADER_FLAGS += -Wno-deprecated-non-prototype
endif
.PHONY: check
check:
	@mkdir -p ../build/tests
	$(CC) $(CFLAGS) $(GNB_HEADER_FLAGS) -I../src -I../libs test_utc_policy.c ../src/gnb_utc.c ../src/gnb_utc_service.c ../libs/ed25519/sha512.c -pthread -o ../build/tests/test_utc_policy
	../build/tests/test_utc_policy
	$(CC) $(CFLAGS) $(GNB_HEADER_FLAGS) -I../src -I../libs test_utc_runtime.c ../src/gnb_utc.c ../src/gnb_utc_service.c ../libs/ed25519/sha512.c -pthread -o ../build/tests/test_utc_runtime
	../build/tests/test_utc_runtime
	$(CC) $(CFLAGS) $(GNB_HEADER_FLAGS) -I../src -I../libs test_utc_service.c ../src/gnb_utc.c ../src/gnb_utc_service.c ../libs/ed25519/sha512.c -pthread -o ../build/tests/test_utc_service
	../build/tests/test_utc_service
	$(CC) $(CFLAGS) $(GNB_HEADER_FLAGS) -DGNB_UTC_TESTING -I../src -I../libs test_utc_crash.c ../src/gnb_utc.c ../src/gnb_utc_service.c ../libs/ed25519/sha512.c -pthread -o ../build/tests/test_utc_crash
	../build/tests/test_utc_crash
