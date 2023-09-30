#pragma once

#ifdef KLEE_VERIFICATION
#  define NF_INFO(text, ...)
#else // KLEE_VERIFICATION
#  include <stdio.h>
#  include <inttypes.h>
#  define NF_INFO(text, ...)                                                   \
    printf(text "\n", ##__VA_ARGS__);                                          \
    fflush(stdout);
#  define NF_PROFILE(text, ...)                                                   \
    printf("[Profile]" text "\n", ##__VA_ARGS__);                                          \
    fflush(stdout);
#endif // KLEE_VERIFICATION

#ifdef ENABLE_LOG
#  include <stdio.h>
#  include <inttypes.h>
#  include <rte_lcore.h>
#  include <rte_cycles.h>

extern FILE** vigor_logs;
RTE_DECLARE_PER_LCORE(int, _core_ind);

#  define NF_DEBUG(text, ...)                                                  \
    fprintf(vigor_logs[RTE_PER_LCORE(_core_ind)], "<%ld> " text "\n", rte_get_tsc_cycles(), ##__VA_ARGS__)

int logging_init(int num_cores);
int logging_fini(int num_cores);

#else // ENABLE_LOG
#  define NF_DEBUG(...)
#endif // ENABLE_LOG
