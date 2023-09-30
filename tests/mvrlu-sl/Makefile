# This Makefile expects to be included from the shared one
# Skeleton Makefile for NFOS NFs

## Paths
# get current dir, see https://stackoverflow.com/a/8080530
SELF_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

## DPDK stuff
# DPDK uses pkg-config to simplify app building process since version 20.11
# check existance of the DPDK pkg-config
ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

PKGCONF ?= pkg-config
PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

# allow the use of advanced globs in paths
SHELL := /bin/bash -O extglob -O globstar -c

## Source files
SRCS-y += $(shell echo $(SELF_DIR)/../../src/vector.c)
SRCS-y += $(shell echo $(SELF_DIR)/*.c)

## Compiler flags
CFLAGS += -I $(SELF_DIR) -I $(SELF_DIR)/../../src/include -I $(SELF_DIR)/../../deps
CFLAGS += -std=gnu11
CFLAGS += -O3 -flto -g -ggdb
#CFLAGS += -O0 -g -rdynamic -DENABLE_LOG -Wfatal-errors
# GCC optimizes a checksum check in rte_ip.h into a CMOV, which is a very poor choice
# that causes 99th percentile latency to go through the roof;
# force it to not do that with no-if-conversion
ifeq ($(CC),gcc)
CFLAGS += -fno-if-conversion -fno-if-conversion2
endif

## Link flags
LDFLAGS += -L$(SELF_DIR)/../../deps/mv-rlu/lib -lmvrlu-ordo

## Targets
.PHONY: run-test clean
# NF binary target,
# make it clean every time because our dependency tracking is nonexistent...
test: clean $(SRCS-y)
	$(CC) $(CFLAGS) $(SRCS-y) -o test $(LDFLAGS) $(LDFLAGS_STATIC)

clean:
	rm -f test

run-test: test
	sudo ./test --no-shconf -l 8,10
