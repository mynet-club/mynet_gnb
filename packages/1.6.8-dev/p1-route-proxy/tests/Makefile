# p1-route-proxy 单元测试
# 编译真实的 gnb_proxy.c + 测试替身，不复制算法。
#
#   make          编译并运行
#   make clean
#
SRC_ROOT := ../../../..
CC       ?= cc
CFLAGS   ?= -O0 -g -Wall
CPPFLAGS += -I$(SRC_ROOT)/src -I$(SRC_ROOT)/libs
CPPFLAGS += -D NO_GZIP=1
# 与 Makefile.Darwin 的警告策略对齐，避免无关告警淹没结果
CFLAGS   += -Wno-deprecated-non-prototype -Wno-macro-redefined

PROXY_SRC := $(SRC_ROOT)/src/gnb_proxy.c

all: run

test_proxy_conf: test_proxy_conf.c stubs.c $(PROXY_SRC) $(SRC_ROOT)/src/gnb_proxy.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_proxy_conf.c stubs.c $(PROXY_SRC) -pthread

run: test_proxy_conf
	./test_proxy_conf

clean:
	rm -f test_proxy_conf

.PHONY: all run clean
