# Skeleton Makefile for NFs
# This file dispatches the build process to the other Makefiles
#
# Variables that should be defined by inheriting Makefiles:
# - NF_FILES := <NF files for runtime>
# - NF_LAYER := <network stack layer at which the NF operates, default 2>
# - NF_BENCH_NEEDS_REVERSE_TRAFFIC := <whether the NF needs reverse traffic
#                                      for meaningful benchmarks, default false>
# See Makefile for the rest of the variables

SELF_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Default values for arguments
NF_LAYER ?= 2
NF_BENCH_NEEDS_REVERSE_TRAFFIC ?= false

include $(SELF_DIR)/Makefile.dpdk
