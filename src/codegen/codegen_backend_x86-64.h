#include "codegen_backend_x86-64_defs.h"

/* Raised from 0x4000 - too small to hold both status variants (16/32-bit,
   flat/non-flat) of hot code at once, causing recompile thrash - from
   0x10000, which Windows 2000's boot overran: it evicted and compiled again
   15-30 thousand blocks a second - and from 0x20000, which a guest with
   large kernels (CORSAC under Half-Life, about 130 thousand blocks live)
   filled, evicting and compiling again some two thousand blocks a second at
   two thirds of its speed. Block indices are 32-bit. */
#define BLOCK_SIZE  0x40000
#define BLOCK_MASK  0x3ffff
#define BLOCK_START 0

/* As many slots as BLOCK_SIZE has blocks: at half as many, with the
   table full (CORSAC under Half-Life), the lookups that missed their slot
   and walked the page's tree took 5% of the machine thread. */
#define HASH_SIZE   0x40000
#define HASH_MASK   0x3ffff

/* The block hash takes the physical address's bits above 256K too: with the
   low bits alone, code at the same offset in different regions (the
   kernel's and a driver's, two processes' copies) fought for one slot and
   went to the tree walk each time. */
#define HASH(l)     (((l) ^ ((l) >> 18)) & HASH_MASK)

#define BLOCK_MAX   0x3c0

#define CODEGEN_BACKEND_HAS_MOV_IMM
/* A memory access's fast path is inline and calls nothing: the stores a
   flush would make before it go in its slow path (codegen_reg.c). */
#define CODEGEN_BACKEND_SLOW_STORES
/* And so do the compares that leave for an exception routine. */
#define CODEGEN_BACKEND_SLOW_EXITS
/* The memory routines keep the FP registers across their call into C: an
   access writes them back on its slow path only, and keeps them. */
#define CODEGEN_BACKEND_SLOW_KEEPS_FP
/* UOP_CMP_IMM_JB: an unsigned compare with a constant that leaves when below. */
#define CODEGEN_BACKEND_HAS_CMP_IMM_JB
/* UOP_HEAT_DEC_JNZ_DEST: short blocks count their runs, to be compiled long once hot. */
#define CODEGEN_BACKEND_HAS_HEAT
/* The integer uops of the SETcc, BSWAP, BSF/BSR, CMPXCHG and multiply
   handlers: CMP_SET, TEST_SET, BSF, BSR, BSWAP, IMUL, IMUL_HI, IMUL_OV and
   MUL_HI. A backend without them leaves those instructions to the
   interpreter. */
#define CODEGEN_BACKEND_HAS_INT_UOPS

void codegen_backend_cold_reset(void);
