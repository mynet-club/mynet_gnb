CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L
GNB_HEADER_FLAGS := -std=c11 -Wno-unused-variable
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
GNB_HEADER_FLAGS += -Wno-deprecated-non-prototype
endif
.PHONY: check
# The utc-v2 contract fixture lives in the UTC repo; running this gate with the
# fixture missing used to leave the cross-language cases silently unexecuted.
# Resolve the sibling workspace by default and fail loudly if it is absent.
HUB_V2_DIR ?= $(abspath $(CURDIR)/../../mynet_utc/tests/fixtures/hub_v2)
export HUB_V2_FIXTURE ?= $(HUB_V2_DIR)/projection.txt
export HUB_V2_NEGATIVES ?= $(HUB_V2_DIR)/negatives.txt
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
	$(CC) $(CFLAGS) $(GNB_HEADER_FLAGS) -I../src test_utc_hub_contract.c ../src/gnb_utc_hub.c -o ../build/tests/test_utc_hub_contract
	../build/tests/test_utc_hub_contract
