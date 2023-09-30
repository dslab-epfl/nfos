calibration: calibration.c
	gcc -Wall -Wextra -O3 -g -ggdb -o $@ $<

do-calibration: calibration
	@sudo perf record -e intel_pt/cyc/u --filter 'filter rdtsc @ ./calibration' ./calibration; \
	sudo perf script --itrace=bep --ns -F+ipc,-period,-dso,-comm,+flags,+addr,-pid,-tid

all: do-calibration
