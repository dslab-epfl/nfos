// Per core logging utilities
#include "stdio.h"
#include "stdlib.h"

#include <rte_lcore.h>

#include "nf-log.h"

#define BUF_SIZE 1024 * 1024

FILE** vigor_logs;
char** vigor_log_bufs;

RTE_DEFINE_PER_LCORE(int, _core_ind);

int logging_init(int num_cores) {
    int ret = 0;

    vigor_logs = calloc(num_cores, sizeof(FILE*));
    vigor_log_bufs = calloc(num_cores, sizeof(char*));

    for (int i = 0; i < num_cores; i++) {
        char fn[20];
        sprintf(fn, "log.core%d", i);
        vigor_logs[i] = fopen(fn, "w");
        vigor_log_bufs[i] = malloc(BUF_SIZE);
        if (setvbuf(vigor_logs[i], vigor_log_bufs[i], _IOFBF, BUF_SIZE)) {
            ret = 1;
            break;
        }
    }
    return ret;
}

int logging_fini(int num_cores) {
    int ret = 0;

    for (int i = 0; i < num_cores; i++) {
        fflush(vigor_logs[i]);
        fclose(vigor_logs[i]);
        free(vigor_log_bufs[i]);
    }

    free(vigor_log_bufs);
    free(vigor_logs);
    return ret;
}
