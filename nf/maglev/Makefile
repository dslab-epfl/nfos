# Need 3 devices otherwise the concept of a load-balancer doesn't make sense
NF_DEVICES := 3

NF_FILES := main.c

NF_LAYER := 3

NF_BENCH_NEEDS_REVERSE_TRAFFIC := true

NF_INCLUDE_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/../../Makefile
