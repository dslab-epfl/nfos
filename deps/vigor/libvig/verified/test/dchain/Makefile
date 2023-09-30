APP = dchain_test
# get current dir, see https://stackoverflow.com/a/8080530
SELF_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# DPDK stuff
include $(RTE_SDK)/mk/rte.vars.mk

SRCS-y += $(shell echo $(SELF_DIR)/../../double-chain*.c)
SRCS-y += $(shell echo $(SELF_DIR)/../../vigor-time*.c)
SRCS-y += $(shell echo $(SELF_DIR)/main.c)
# Compiler flags
CFLAGS += -I $(SELF_DIR)/../../../..
CFLAGS += -std=gnu11
CFLAGS += -O3
#CFLAGS += -O0 -g -rdynamic -DENABLE_LOG -Wfatal-errors
include $(RTE_SDK)/mk/rte.extapp.mk

test: .PHONY
	sudo ./build/app/dchain_test --no-shconf -l 0-4 -w 81:00.1
