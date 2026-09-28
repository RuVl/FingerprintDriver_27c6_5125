// Minimal Win64 DLL loader (see winpe.c).
#pragma once
#include <stdint.h>
void winpe_set_verbose(int v);
void *winpe_load(const char *path);   // returns image base or NULL
void *winpe_getproc(const char *name);
void *winpe_base(void);
int winpe_hook(uint64_t va, void *fn);  // patch VA (preferred base) -> jmp fn
