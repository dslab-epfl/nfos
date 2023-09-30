#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t __attribute__((noinline)) rdtsc(void)
{
        unsigned int low, high;

        asm volatile("rdtsc" : "=a" (low), "=d" (high));

        return low | ((uint64_t)high) << 32;
}

volatile int x = 0;

static void __attribute__((noinline)) f(void)
{
	x = x * x + x + 1;
}

int main(void)
{
	unsigned long long tsc1;

    for (int i = 0; i < 10; i++) {
	    tsc1 = rdtsc();
        printf("%llu\n", tsc1);
        sleep(1);
    }

	return 0;
}
