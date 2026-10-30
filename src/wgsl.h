/* The WGSL back end (S10 B1): `langc build PROG --wgsl OUT.wgsl` writes a WGSL
   module of compute shaders in place of the target. See src/wgsl.c. */
#ifndef LANG_WGSL_H
#define LANG_WGSL_H

#include <stdio.h>

#include "ir.h"

/* Writes the WGSL module of PROG. Returns 1, or 0 after a message on ERR. */
int wgsl_write(const IrProgram *prog, FILE *out, FILE *err);

#endif
