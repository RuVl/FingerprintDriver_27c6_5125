#pragma once
#include <stdio.h>
#define ALGO_LOG_VA 0x1800088e0ULL   // AlgoMilan internal printf-like logger
// out=NULL silences; filter(line) returning 0 drops the line.
void algolog_set(FILE *out, int (*filter)(const char *line));
void *algolog_fn(void);             // ms_abi function to pass to winpe_hook
