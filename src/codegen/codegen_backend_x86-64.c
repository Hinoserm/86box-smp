#if defined __amd64__ || defined _M_X64

#    include <stddef.h>
#    include <stdlib.h>
#    include <stdint.h>
#    include <86box/86box.h>
#    include "cpu.h"
#    include <86box/mem.h>
#    include <86box/plat.h>

#    include "codegen.h"
#    include "codegen_allocator.h"
#    include "codegen_backend.h"
#    include "codegen_backend_x86-64_defs.h"
#    include "codegen_backend_x86-64_ops.h"
#    include "codegen_backend_x86-64_ops_helpers.h"
#    include "codegen_backend_x86-64_ops_sse.h"
#    include "codegen_ir_defs.h"
#    include "codegen_reg.h"
#    include "x86.h"
#    include "x86seg_common.h"
#    include "x86seg.h"
#    include "x87_host.h"

#    if defined(__linux__) || defined(__APPLE__)
#        include <sys/mman.h>
#        include <unistd.h>
#    endif
#    if defined WIN32 || defined _WIN32 || defined _WIN32
#        include <windows.h>
#    endif
#    include <string.h>

void *codegen_mem_load_byte;
void *codegen_mem_load_word;
void *codegen_mem_load_long;
void *codegen_mem_load_quad;
void *codegen_mem_load_single;
void *codegen_mem_load_double;

void *codegen_mem_store_byte;
void *codegen_mem_store_word;
void *codegen_mem_store_long;
void *codegen_mem_load_word_ea;
void *codegen_mem_load_long_ea;

void *codegen_mem_store_quad;
void *codegen_mem_store_single;
void *codegen_mem_store_double;

void *codegen_gpf_rout;
void *codegen_ss_rout;
void *codegen_exit_rout;

host_reg_def_t codegen_host_reg_list[CODEGEN_HOST_REGS] = {
  /*R13-R15 are callee-saved and already saved by the block prologue. They
  come first so that registers never accessed by byte settle there, leaving
  the three with byte forms to the rest*/
    {REG_R13,  HOST_REG_FLAG_NO_BYTE},
    { REG_R14, HOST_REG_FLAG_NO_BYTE},
    { REG_R15, HOST_REG_FLAG_NO_BYTE},
  /*Note: while EAX and EDX are normally volatile registers under x86
  calling conventions, the recompiler will explicitly save and restore
  them across funcion calls*/
    { REG_EAX, 0},
    { REG_EBX, 0},
    { REG_EDX, 0}
};

/* The Windows x64 frame: the 32 bytes of home space a called function may
   store its arguments in, the block temporaries above them (codegen_reg.c,
   ending at 0x50), then XMM6 and XMM7 as the caller left them. 0x78 keeps
   RSP 16-byte aligned after the eight pushes and the return address. */
#define CODEGEN_WIN64_FRAME 0x78
#define CODEGEN_XMM6_SAVE   0x50
#define CODEGEN_XMM7_SAVE   0x60

host_reg_def_t codegen_host_fp_reg_list[CODEGEN_HOST_FP_REGS] = {
#    if _WIN64
  /*Windows x86-64 calling convention preserves XMM6-XMM15*/
    {REG_XMM6,  0                     },
    { REG_XMM7, 0                     },
#    else
    /*System V AMD64 calling convention does not preserve any XMM registers*/
    { REG_XMM6, HOST_REG_FLAG_VOLATILE },
    { REG_XMM7, HOST_REG_FLAG_VOLATILE },
#    endif
    { REG_XMM1, HOST_REG_FLAG_VOLATILE},
    { REG_XMM2, HOST_REG_FLAG_VOLATILE},
    { REG_XMM3, HOST_REG_FLAG_VOLATILE},
    { REG_XMM4, HOST_REG_FLAG_VOLATILE},
    { REG_XMM5, HOST_REG_FLAG_VOLATILE}
};

/*A memory routine's call into C keeps the recompiler's FP registers
  (XMM1-XMM7, CODEGEN_BACKEND_SLOW_KEEPS_FP): their low quadwords, the
  doubles and MMX values the IR keeps there, saved on the stack around it,
  so a memory access need not write back and drop the x87 stack held in
  them, though System V's convention (and Windows' for XMM1-XMM5) lets a
  call change them.*/
#    define ROUTINE_FP_SAVE (7 * 8)
static void
routine_save_fp(codeblock_t *block, int offset)
{
    for (int c = 1; c <= 7; c++)
        host_x86_MOVQ_BASE_OFFSET_XREG(block, REG_RSP, offset + (c - 1) * 8, REG_XMM0 + c);
}
static void
routine_restore_fp(codeblock_t *block, int offset)
{
    for (int c = 1; c <= 7; c++)
        host_x86_MOVQ_XREG_BASE_OFFSET(block, REG_XMM0 + c, REG_RSP, offset + (c - 1) * 8);
}

/*ea_rule: take the direct load whenever the address is at most 0xffc into
  its page, aligned or not, as the interpreter's eal_r path does; readmemwl
  and readmemll charge timing_misaligned only on the slow path.*/
static void
build_load_routine(codeblock_t *block, int size, int is_float, int ea_rule)
{
    uint8_t *branch_offset;
    uint8_t *misaligned_offset = NULL;

    /*In - ESI = address
      Out - ECX = data, ESI = abrt*/
    /*MOV ECX, ESI
      SHR ESI, 12
      MOV RDI, [&readlookup2]
      MOV RSI, [RDI+ESI*8]
      CMP ESI, -1
      JNZ +
      MOVZX ECX, B[RSI+RCX]
      XOR ESI,ESI
      RET
    * PUSH EAX
      PUSH EDX
      PUSH ECX
      CALL readmembl
      POP ECX
      POP EDX
      POP EAX
      MOVZX ECX, AL
      RET
    */
    host_x86_MOV32_REG_REG(block, REG_ECX, REG_ESI);
    host_x86_SHR32_IMM(block, REG_ESI, 12);
    /* The table is the set in place (mem_tlb_select): fetch where it is
       now, not where it was when this routine was built. */
    host_x86_MOV64_REG_IMM(block, REG_RDI, (uint64_t) (uintptr_t) &readlookup2);
    host_x86_MOV64_REG_DEREF(block, REG_RDI, REG_RDI);
    host_x86_MOV64_REG_BASE_INDEX_SHIFT(block, REG_RSI, REG_RDI, REG_RSI, 3);
    if (ea_rule) {
        host_x86_MOV32_REG_REG(block, REG_EDI, REG_ECX);
        host_x86_AND32_REG_IMM(block, REG_EDI, 0xfff);
        host_x86_CMP32_REG_IMM(block, REG_EDI, 0xffc);
        misaligned_offset = host_x86_JNBE_short(block);
    } else if (size != 1) {
        host_x86_TEST32_REG_IMM(block, REG_ECX, size - 1);
        misaligned_offset = host_x86_JNZ_short(block);
    }
    host_x86_CMP64_REG_IMM(block, REG_RSI, (uint32_t) -1);
    branch_offset = host_x86_JZ_short(block);
    if (size == 1 && !is_float)
        host_x86_MOVZX_BASE_INDEX_32_8(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 2 && !is_float)
        host_x86_MOVZX_BASE_INDEX_32_16(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 4 && !is_float)
        host_x86_MOV32_REG_BASE_INDEX(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 4 && is_float)
        host_x86_CVTSS2SD_XREG_BASE_INDEX(block, REG_XMM_TEMP, REG_RSI, REG_RCX);
    else if (size == 8)
        host_x86_MOVQ_XREG_BASE_INDEX(block, REG_XMM_TEMP, REG_RSI, REG_RCX);
    else
        fatal("build_load_routine: size=%i\n", size);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
    host_x86_RET(block);

    *branch_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) branch_offset) - 1;
    if (ea_rule || size != 1)
        *misaligned_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) misaligned_offset) - 1;
    host_x86_PUSH(block, REG_RAX);
    host_x86_PUSH(block, REG_RDX);
#    if _WIN64
    /* The callee's 32 bytes, then the FP registers kept */
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x28 + ROUTINE_FP_SAVE + 8);
    routine_save_fp(block, 0x28);
    // host_x86_MOV32_REG_REG(block, REG_ECX, uop->imm_data);
#    else
    /* Align RSP to 16: entry RSP%16=8 (after CALL from JIT block), two PUSHes
       leave it at 8; subtract 8 more to satisfy the SysV ABI before calling C,
       and room for the FP registers kept, a multiple of 16. */
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x8 + ROUTINE_FP_SAVE + 8);
    routine_save_fp(block, 0);
    host_x86_MOV32_REG_REG(block, REG_EDI, REG_ECX);
#    endif
    if (size == 1 && !is_float) {
        host_x86_CALL(block, (void *) readmembl);
        host_x86_MOVZX_REG_32_8(block, REG_ECX, REG_EAX);
    } else if (size == 2 && !is_float) {
        host_x86_CALL(block, (void *) readmemwl);
        host_x86_MOVZX_REG_32_16(block, REG_ECX, REG_EAX);
    } else if (size == 4 && !is_float) {
        host_x86_CALL(block, (void *) readmemll);
        host_x86_MOV32_REG_REG(block, REG_ECX, REG_EAX);
    } else if (size == 4 && is_float) {
        host_x86_CALL(block, (void *) readmemll);
        host_x86_MOVD_XREG_REG(block, REG_XMM_TEMP, REG_EAX);
        host_x86_CVTSS2SD_XREG_XREG(block, REG_XMM_TEMP, REG_XMM_TEMP);
    } else if (size == 8) {
        host_x86_CALL(block, (void *) readmemql);
        host_x86_MOVQ_XREG_REG(block, REG_XMM_TEMP, REG_RAX);
    }
#    if _WIN64
    routine_restore_fp(block, 0x28);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x28 + ROUTINE_FP_SAVE + 8);
#    else
    routine_restore_fp(block, 0);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x8 + ROUTINE_FP_SAVE + 8);
#    endif
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_MOVZX_REG_ABS_32_8(block, REG_ESI, &cpu_state.abrt);
    host_x86_RET(block);
}

static void
build_store_routine(codeblock_t *block, int size, int is_float)
{
    uint8_t *branch_offset;
    uint8_t *misaligned_offset = NULL;

    /*In - ECX = data, ESI = address
      Out - ESI = abrt
      Corrupts EDI*/
    /*MOV EDI, ESI
      SHR ESI, 12
      MOV R8, [&writelookup2]
      MOV RSI, [R8+ESI*8]
      CMP ESI, -1
      JNZ +
      MOV [RSI+RDI], ECX
      XOR ESI,ESI
      RET
    * PUSH EAX
      PUSH EDX
      PUSH ECX
      CALL writemembl
      POP ECX
      POP EDX
      POP EAX
      MOVZX ECX, AL
      RET
    */
    host_x86_MOV32_REG_REG(block, REG_EDI, REG_ESI);
    host_x86_SHR32_IMM(block, REG_ESI, 12);
    host_x86_MOV64_REG_IMM(block, REG_R8, (uint64_t) (uintptr_t) &writelookup2);
    host_x86_MOV64_REG_DEREF(block, REG_R8, REG_R8);
    host_x86_MOV64_REG_BASE_INDEX_SHIFT(block, REG_RSI, REG_R8, REG_RSI, 3);
    if (size != 1) {
        host_x86_TEST32_REG_IMM(block, REG_EDI, size - 1);
        misaligned_offset = host_x86_JNZ_short(block);
    }
    host_x86_CMP64_REG_IMM(block, REG_RSI, (uint32_t) -1);
    branch_offset = host_x86_JZ_short(block);
    if (size == 1 && !is_float)
        host_x86_MOV8_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 2 && !is_float)
        host_x86_MOV16_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 4 && !is_float)
        host_x86_MOV32_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 4 && is_float)
        host_x86_MOVD_BASE_INDEX_XREG(block, REG_RSI, REG_RDI, REG_XMM_TEMP);
    else if (size == 8)
        host_x86_MOVQ_BASE_INDEX_XREG(block, REG_RSI, REG_RDI, REG_XMM_TEMP);
    else
        fatal("build_store_routine: size=%i\n", size);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
    host_x86_RET(block);

    *branch_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) branch_offset) - 1;
    if (size != 1)
        *misaligned_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) misaligned_offset) - 1;
    host_x86_PUSH(block, REG_RAX);
    host_x86_PUSH(block, REG_RDX);
#    if _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x28 + ROUTINE_FP_SAVE + 8);
    routine_save_fp(block, 0x28);
    if (size == 4 && is_float)
        host_x86_MOVD_REG_XREG(block, REG_EDX, REG_XMM_TEMP); // data
    else if (size == 8)
        host_x86_MOVQ_REG_XREG(block, REG_RDX, REG_XMM_TEMP); // data
    else
        host_x86_MOV32_REG_REG(block, REG_EDX, REG_ECX); // data
    host_x86_MOV32_REG_REG(block, REG_ECX, REG_EDI);     // address
#    else
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x8 + ROUTINE_FP_SAVE + 8);
    routine_save_fp(block, 0);
    // host_x86_MOV32_REG_REG(block, REG_EDI, REG_ECX);  //address
    if (size == 4 && is_float)
        host_x86_MOVD_REG_XREG(block, REG_ESI, REG_XMM_TEMP); // data
    else if (size == 8)
        host_x86_MOVQ_REG_XREG(block, REG_RSI, REG_XMM_TEMP); // data
    else
        host_x86_MOV32_REG_REG(block, REG_ESI, REG_ECX); // data
#    endif
    if (size == 1)
        host_x86_CALL(block, (void *) writemembl);
    else if (size == 2)
        host_x86_CALL(block, (void *) writememwl);
    else if (size == 4)
        host_x86_CALL(block, (void *) writememll);
    else if (size == 8)
        host_x86_CALL(block, (void *) writememql);
#    if _WIN64
    routine_restore_fp(block, 0x28);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x28 + ROUTINE_FP_SAVE + 8);
#    else
    routine_restore_fp(block, 0);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x8 + ROUTINE_FP_SAVE + 8);
#    endif
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_MOVZX_REG_ABS_32_8(block, REG_ESI, &cpu_state.abrt);
    host_x86_RET(block);
}

/*The routines span more than one code chunk. Each routine starts in a chunk
  with room for all of it, since its short branches cannot cross the jump
  that links two chunks, and its address is taken from the chunk being
  written, not from the first one.*/
static void *
routine_start(codeblock_t *block)
{
    codegen_alloc(block, 0x140);
    return &block_write_data[block_pos];
}

static void
build_loadstore_routines(codeblock_t *block)
{
    codegen_mem_load_byte = routine_start(block);
    build_load_routine(block, 1, 0, 0);
    codegen_mem_load_word = routine_start(block);
    build_load_routine(block, 2, 0, 0);
    codegen_mem_load_long = routine_start(block);
    build_load_routine(block, 4, 0, 0);
    codegen_mem_load_quad = routine_start(block);
    build_load_routine(block, 8, 0, 0);
    codegen_mem_load_single = routine_start(block);
    build_load_routine(block, 4, 1, 0);
    codegen_mem_load_double = routine_start(block);
    build_load_routine(block, 8, 1, 0);
    codegen_mem_load_word_ea = routine_start(block);
    build_load_routine(block, 2, 0, 1);
    codegen_mem_load_long_ea = routine_start(block);
    build_load_routine(block, 4, 0, 1);

    codegen_mem_store_byte = routine_start(block);
    build_store_routine(block, 1, 0);
    codegen_mem_store_word = routine_start(block);
    build_store_routine(block, 2, 0);
    codegen_mem_store_long = routine_start(block);
    build_store_routine(block, 4, 0);
    codegen_mem_store_quad = routine_start(block);
    build_store_routine(block, 8, 0);
    codegen_mem_store_single = routine_start(block);
    build_store_routine(block, 4, 1);
    codegen_mem_store_double = routine_start(block);
    build_store_routine(block, 8, 1);
}

/* ------------------------------------------------------------------ */
/* The host-x87 FPU's kernels (x87_host.h)                            */
/* ------------------------------------------------------------------ */

x87_host_kernel_t x87_host_kernel[X87_HOST_COUNT];

/* The frame pointer: the kernels' one argument. */
#    ifdef _WIN64
#        define X87_HOST_ARG 1 /* RCX */
#    else
#        define X87_HOST_ARG 7 /* RDI */
#    endif

/* An x87 instruction on a field of the frame: opcode, /digit, [arg + disp8]. */
static void
x87_host_emit_m(uint8_t op, uint8_t digit, uint8_t disp)
{
    codegen_addbyte3(NULL, op, 0x40 | (digit << 3) | X87_HOST_ARG, disp);
}

/* FLDCW [from], unless [hcw] (the host's own) is [cw] already. EAX is
   volatile in both ABIs. */
static void
x87_host_emit_cw(uint8_t from)
{
    codegen_addbyte4(NULL, 0x0f, 0xb7, 0x40 | X87_HOST_ARG, X87_HOST_CW);  /* MOVZX EAX, word [cw] */
    codegen_addbyte4(NULL, 0x66, 0x3b, 0x40 | X87_HOST_ARG, X87_HOST_HCW); /* CMP AX, [hcw] */
    codegen_addbyte2(NULL, 0x74, 0x03);                                    /* JE past the FLDCW */
    x87_host_emit_m(0xd9, 5, from);                                        /* FLDCW [from] */
}

/* One kernel: the host's control word saved and the guest's loaded, the
   status word as it stands, the operands loaded as they are in the guest's
   registers, the operation, the status word taken at once (the store after
   it changes C1), the result stored, and the host's control word put back
   (it is preserved across calls in both ABIs). The flags are the caller's
   (x87_host_run()). The host x87 is empty on entry and on return, as the ABI
   has it at a call. */
static x87_host_kernel_t
x87_host_emit(int k)
{
    x87_host_kernel_t start = (x87_host_kernel_t) (void *) &block_write_data[block_pos];

    if (k == X87_HOST_CLEAR) {
        codegen_addbyte2(NULL, 0xdb, 0xe2); /* FNCLEX */
        codegen_addbyte(NULL, 0xc3);        /* RET */
        return start;
    }

    x87_host_emit_m(0xd9, 7, X87_HOST_HCW); /* FNSTCW [hcw] */
    x87_host_emit_cw(X87_HOST_CW);
    x87_host_emit_m(0xdd, 7, X87_HOST_PRE); /* FNSTSW [pre] */
    x87_host_emit_m(0xdb, 5, X87_HOST_A);   /* FLD m80 [a] */

    switch (k) {
        case X87_HOST_ADD:
        case X87_HOST_SUB:
        case X87_HOST_MUL:
        case X87_HOST_DIV:
            /* ST(0) = b, ST(1) = a: FADDP, FSUBP, FMULP and FDIVP ST(1), ST(0)
               leave a + b, a - b, a * b and a / b. */
            x87_host_emit_m(0xdb, 5, X87_HOST_B); /* FLD m80 [b] */
            codegen_addbyte2(NULL, 0xde, (k == X87_HOST_ADD) ? 0xc1 : (k == X87_HOST_SUB) ? 0xe9 : (k == X87_HOST_MUL) ? 0xc9 : 0xf9);
            x87_host_emit_m(0xdd, 7, X87_HOST_SW); /* FNSTSW [sw] */
            x87_host_emit_m(0xdb, 7, X87_HOST_R);  /* FSTP m80 [r] */
            break;
        case X87_HOST_SQRT:
        case X87_HOST_RNDINT:
            codegen_addbyte2(NULL, 0xd9, (k == X87_HOST_SQRT) ? 0xfa : 0xfc); /* FSQRT, FRNDINT */
            x87_host_emit_m(0xdd, 7, X87_HOST_SW);
            x87_host_emit_m(0xdb, 7, X87_HOST_R);
            break;
        case X87_HOST_TO_F32:
            x87_host_emit_m(0xd9, 3, X87_HOST_I); /* FSTP m32 [i] */
            x87_host_emit_m(0xdd, 7, X87_HOST_SW);
            break;
        case X87_HOST_TO_F64:
            x87_host_emit_m(0xdd, 3, X87_HOST_I); /* FSTP m64 [i] */
            x87_host_emit_m(0xdd, 7, X87_HOST_SW);
            break;
        case X87_HOST_TO_I32:
            x87_host_emit_m(0xdb, 3, X87_HOST_I); /* FISTP m32 [i] */
            x87_host_emit_m(0xdd, 7, X87_HOST_SW);
            break;
        case X87_HOST_TO_I64:
            x87_host_emit_m(0xdf, 7, X87_HOST_I); /* FISTP m64 [i] */
            x87_host_emit_m(0xdd, 7, X87_HOST_SW);
            break;
        default:
            break;
    }

    x87_host_emit_cw(X87_HOST_HCW);
    codegen_addbyte(NULL, 0xc3); /* RET */
    return start;
}

/* Written once, into a page of their own: the kernels are called from C, so
   they need not live among the blocks. The emitter writes at most BLOCK_MAX
   bytes from its base, so the base moves up to where each kernel ended. */
static void
build_x87_host_routines(void)
{
    uint8_t *page = plat_mmap(0x2000, 1, NULL);
    uint8_t *saved_data;
    int      saved_pos;

    if (page == NULL) {
        pclog("x87: no executable memory for the host-x87 kernels; the host-x87 FPU is off\n");
        return;
    }
    saved_data       = block_write_data;
    saved_pos        = block_pos;
    block_write_data = page;
    block_pos        = 0;
    for (int k = 0; k < X87_HOST_COUNT; k++) {
        x87_host_kernel[k] = x87_host_emit(k);
        block_write_data += block_pos;
        block_pos = 0;
        if ((block_write_data + BLOCK_MAX) > (page + 0x2000))
            fatal("x87: the host-x87 kernels outgrew their pages\n");
    }
    block_write_data = saved_data;
    block_pos        = saved_pos;
}

void
codegen_backend_init(void)
{
    codeblock_t *block;
    int          c;
    uint8_t      large_block = 0;
    uint8_t      large_hash = 0;

    codeblock      = plat_mmap(BLOCK_SIZE * sizeof(codeblock_t), 0, &large_block);
    codeblock_hash = plat_mmap(HASH_SIZE * sizeof(codeblock_t *), 0, &large_hash);

    if (large_block)
        pclog("Allocated %llu bytes of large pages for codeblock pointers\n", (unsigned long long) (BLOCK_SIZE * sizeof(codeblock_t)));
    if (large_hash)
        pclog("Allocated %llu bytes of large pages for codeblock hashes\n", (unsigned long long) (HASH_SIZE * sizeof(codeblock_t *)));

    for (c = 0; c < BLOCK_SIZE; c++)
        codeblock[c].valid = 0;

    block_current                           = 0;
    block_pos                               = 0;
    block                                   = &codeblock[block_current];
    codeblock[block_current].head_mem_block = codegen_allocator_allocate(NULL, block_current);
    codeblock[block_current].data           = codeblock_allocator_get_ptr(codeblock[block_current].head_mem_block);
    block_write_data                        = codeblock[block_current].data;
    build_loadstore_routines(&codeblock[block_current]);

    codegen_gpf_rout = routine_start(block);
#    if _WIN64
    host_x86_XOR32_REG_REG(block, REG_ECX, REG_ECX);
    host_x86_XOR32_REG_REG(block, REG_EDX, REG_EDX);
#    else
    host_x86_XOR32_REG_REG(block, REG_EDI, REG_EDI);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
#    endif
    host_x86_CALL(block, (void *) x86gpf);
    codegen_exit_rout = &block_write_data[block_pos];
#ifdef _WIN64
    /* XMM6 and XMM7 hold guest FPU/MMX values in blocks, and the Windows
       x64 ABI makes them the caller's: put back what the caller had. */
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM6, REG_RSP, CODEGEN_XMM6_SAVE);
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM7, REG_RSP, CODEGEN_XMM7_SAVE);
    host_x86_ADD64_REG_IMM(block, REG_RSP, CODEGEN_WIN64_FRAME);
#else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x48);
#endif
    host_x86_POP(block, REG_R15);
    host_x86_POP(block, REG_R14);
    host_x86_POP(block, REG_R13);
    host_x86_POP(block, REG_R12);
#ifdef _WIN64
    host_x86_POP(block, REG_RDI);
    host_x86_POP(block, REG_RSI);
#endif
    host_x86_POP(block, REG_RBP);
    host_x86_POP(block, REG_RBX);
    host_x86_RET(block);

    /*#SS(0): an operand through SS outside its limit (CHECK_SEG_LIMITS)*/
    codegen_ss_rout = routine_start(block);
#    if _WIN64
    host_x86_XOR32_REG_REG(block, REG_ECX, REG_ECX);
    host_x86_XOR32_REG_REG(block, REG_EDX, REG_EDX);
#    else
    host_x86_XOR32_REG_REG(block, REG_EDI, REG_EDI);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
#    endif
    host_x86_CALL(block, (void *) x86ss);
    host_x86_JMP(block, codegen_exit_rout);

    build_x87_host_routines();

    block_write_data = NULL;

    asm(
        "stmxcsr %0\n"
        : "=m"(cpu_state.old_fp_control));
    cpu_state.trunc_fp_control = cpu_state.old_fp_control | 0x6000;
}

void
codegen_set_rounding_mode(int mode)
{
    cpu_state.new_fp_control = (cpu_state.old_fp_control & ~0x6000) | (mode << 13);
}

void
codegen_backend_prologue(codeblock_t *block)
{
    block_pos = BLOCK_START; /*Entry code*/
    host_x86_PUSH(block, REG_RBX);
    host_x86_PUSH(block, REG_RBP);
#ifdef _WIN64
    host_x86_PUSH(block, REG_RSI);
    host_x86_PUSH(block, REG_RDI);
#endif
    host_x86_PUSH(block, REG_R12);
    host_x86_PUSH(block, REG_R13);
    host_x86_PUSH(block, REG_R14);
    host_x86_PUSH(block, REG_R15);
#ifdef _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, CODEGEN_WIN64_FRAME);
    host_x86_MOVDQU_BASE_OFFSET_XREG(block, REG_RSP, CODEGEN_XMM6_SAVE, REG_XMM6);
    host_x86_MOVDQU_BASE_OFFSET_XREG(block, REG_RSP, CODEGEN_XMM7_SAVE, REG_XMM7);
#else
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x48);
#endif
    host_x86_MOV64_REG_IMM(block, REG_RBP, ((uintptr_t) &cpu_state) + 128);
#ifdef CODEGEN_CHAIN
    /* A chain enters here: the frame and RBP are every block's; what
       follows is this block's own. */
    block->chain_entry = block_pos;
#endif
    if (block->flags & CODEBLOCK_HAS_FPU) {
        host_x86_MOV32_REG_ABS(block, REG_EAX, &cpu_state.TOP);
        host_x86_SUB32_REG_IMM(block, REG_EAX, block->TOP);
        host_x86_MOV32_BASE_OFFSET_REG(block, REG_RSP, IREG_TOP_diff_stack_offset, REG_EAX);
#ifdef CODEGEN_CHAIN
        /* Compiled for one top of stack, which the dispatcher checked; a
           chain arriving with another leaves for the dispatcher, which
           compiles the block again for a moving top. */
        if (block->flags & CODEBLOCK_STATIC_TOP) {
            codegen_alloc_bytes(block, 2);
            codegen_addbyte2(block, 0xa8, 0x07); /*TEST AL, 7*/
            host_x86_JNZ(block, codegen_exit_rout);
        }
#endif
    }
    if (block->flags & CODEBLOCK_NO_IMMEDIATES)
        host_x86_MOV64_REG_IMM(block, REG_R12, ((uintptr_t) ram) + 2147483648ULL);
}

#ifdef CODEGEN_CHAIN
/* A link stub, at a block exit whose PC is already stored: it goes
   straight to the next block while nothing needs the dispatcher. The
   offsets of the fields codegen_chain_patch() writes: */
#    define LINK_GENPTR 49 /* &target->link_gen */
#    define LINK_GEN    59 /* its value when linked */
#    define LINK_GWORD  78 /* its link group's word (codegen_link_group_word) */
#    define LINK_GVAL   88 /* its value when linked */
#    define LINK_STATUS 134
#    define LINK_JMP    144
#    define LINK_MISS   149
#    define LINK_BACK   16 /* past the signal test: where a serviced chain goes on */
/* The miss path (23 bytes), the exit's JMP and the service's CALL at their
   longest (13 each), TEST, JNZ and the JMP back. */
#    define LINK_SIZE (LINK_MISS + 23 + 13 + 13 + 2 + 6 + 5)

static void
link_rel32(uint8_t *at, const uint8_t *dest)
{
    *(int32_t *) at = (int32_t) (dest - (at + 4));
}

void
codegen_backend_link_stub(codeblock_t *block)
{
    const int32_t sig    = (int32_t) ((uintptr_t) &cpu_state.chain_signal - ((uintptr_t) &cpu_state + 128));
    const int32_t cyc    = (int32_t) ((uintptr_t) &cpu_state._cycles - ((uintptr_t) &cpu_state + 128));
    const int32_t flr    = (int32_t) ((uintptr_t) &cpu_state.chain_floor - ((uintptr_t) &cpu_state + 128));
    const int32_t flg_hi = (int32_t) ((uintptr_t) &cpu_state.flags + 1 - ((uintptr_t) &cpu_state + 128));
    const int32_t acc    = (int32_t) ((uintptr_t) &cpu_state.seg_cs.access - ((uintptr_t) &cpu_state + 128));
    uint8_t      *stub;
    uint8_t      *exit_at;
    uint8_t      *service_at;

    /* One piece, so the fields keep their offsets. */
    codegen_alloc_bytes(block, LINK_SIZE);
    stub = &block_write_data[block_pos];
#    define LINK_AT(off) \
        if (&block_write_data[block_pos] != stub + (off)) \
            fatal("codegen_backend_link_stub: field at %i, not %i\n", (int) (&block_write_data[block_pos] - stub), off)

    codegen_addbyte3(block, 0x48, 0x8b, 0x85); /*MOV RAX, [RBP+sig]*/
    codegen_addlong(block, (uint32_t) sig);
    codegen_addbyte3(block, 0x83, 0x38, 0x00); /*CMP DWORD [RAX], 0*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE service*/
    codegen_addlong(block, 0);
    LINK_AT(LINK_BACK);
    codegen_addbyte2(block, 0x8b, 0x85); /*MOV EAX, [RBP+cycles]*/
    codegen_addlong(block, (uint32_t) cyc);
    codegen_addbyte2(block, 0x3b, 0x85); /*CMP EAX, [RBP+chain_floor]*/
    codegen_addlong(block, (uint32_t) flr);
    codegen_addbyte2(block, 0x0f, 0x8e); /*JLE exit*/
    codegen_addlong(block, 0);
    codegen_addbyte2(block, 0xf6, 0x85); /*TEST BYTE [RBP+flags+1], 1 (TF)*/
    codegen_addlong(block, (uint32_t) flg_hi);
    codegen_addbyte(block, 0x01);
    codegen_addbyte2(block, 0x0f, 0x85); /*JNE exit*/
    codegen_addlong(block, 0);
    /* The target's link generation: anything that changes what the link
       reaches (its code written, the block gone, its translation changed)
       moves it (codegen_block.c). */
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, &target->link_gen*/
    LINK_AT(LINK_GENPTR);
    codegen_addquad(block, (uint64_t) (uintptr_t) &codegen_link_unlinked);
    codegen_addbyte2(block, 0x48, 0xb9); /*MOV RCX, GEN*/
    LINK_AT(LINK_GEN);
    codegen_addquad(block, 0);
    codegen_addbyte3(block, 0x48, 0x39, 0x08); /*CMP [RAX], RCX*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE miss*/
    codegen_addlong(block, 0);
    /* The target's link group: held while its pages translate elsewhere
       (another address space), and good again when they come back. */
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, &group word*/
    LINK_AT(LINK_GWORD);
    codegen_addquad(block, (uint64_t) (uintptr_t) &codegen_lg_solo_word);
    codegen_addbyte2(block, 0x48, 0xb9); /*MOV RCX, VALUE*/
    LINK_AT(LINK_GVAL);
    codegen_addquad(block, 0);
    codegen_addbyte3(block, 0x48, 0x39, 0x08); /*CMP [RAX], RCX*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE miss*/
    codegen_addlong(block, 0);
    /* The key: the status word, and the CPL in bits 17:16 (CODEGEN_CHAIN_KEY). */
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, &cpu_cur_status*/
    codegen_addquad(block, (uint64_t) (uintptr_t) &cpu_cur_status);
    codegen_addbyte3(block, 0x0f, 0xb7, 0x00); /*MOVZX EAX, WORD [RAX]*/
    codegen_addbyte3(block, 0x0f, 0xb6, 0x8d); /*MOVZX ECX, BYTE [RBP+cs access]*/
    codegen_addlong(block, (uint32_t) acc);
    codegen_addbyte3(block, 0x83, 0xe1, 0x60); /*AND ECX, 60h (DPL = CPL)*/
    codegen_addbyte3(block, 0xc1, 0xe1, 0x0b); /*SHL ECX, 11*/
    codegen_addbyte2(block, 0x09, 0xc8);       /*OR EAX, ECX*/
    codegen_addbyte(block, 0x3d);              /*CMP EAX, KEY*/
    LINK_AT(LINK_STATUS);
    codegen_addlong(block, 0xffffffff);  /*never matches until linked*/
    codegen_addbyte2(block, 0x0f, 0x85); /*JNE miss*/
    codegen_addlong(block, 0);
    LINK_AT(LINK_JMP);
    codegen_addbyte(block, 0xe9); /*JMP TARGET (miss until linked)*/
    codegen_addlong(block, 0);
    /*miss:*/
    LINK_AT(LINK_MISS);
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, stub*/
    codegen_addquad(block, (uint64_t) (uintptr_t) stub);
    codegen_addbyte2(block, 0x48, 0xb9); /*MOV RCX, &codegen_chain_last_exit*/
    codegen_addquad(block, (uint64_t) (uintptr_t) &codegen_chain_last_exit);
    codegen_addbyte3(block, 0x48, 0x89, 0x01); /*MOV [RCX], RAX*/
    /*exit:*/
    exit_at = &block_write_data[block_pos];
    host_x86_JMP(block, codegen_exit_rout);
    /*service: something was signalled -- the service routine looks at it
      (386_dynarec.c codegen_chain_service) and the chain goes on unless
      the dispatcher must look*/
    service_at = &block_write_data[block_pos];
    host_x86_CALL(block, (void *) codegen_chain_service);
    codegen_addbyte2(block, 0x85, 0xc0); /*TEST EAX, EAX*/
    codegen_addbyte2(block, 0x0f, 0x85); /*JNZ exit*/
    codegen_addlong(block, 0);
    link_rel32(&block_write_data[block_pos - 4], exit_at);
    codegen_addbyte(block, 0xe9); /*JMP back*/
    codegen_addlong(block, 0);
    link_rel32(&block_write_data[block_pos - 4], stub + LINK_BACK);
#    undef LINK_AT

    if ((&block_write_data[block_pos] - stub) > LINK_SIZE)
        fatal("codegen_backend_link_stub: %i bytes\n", (int) (&block_write_data[block_pos] - stub));

    link_rel32(stub + 12, service_at);                    /*JNE service (signal)*/
    link_rel32(stub + 30, exit_at);                       /*JLE exit (cycles)*/
    link_rel32(stub + 43, exit_at);                       /*JNE exit (TF)*/
    link_rel32(stub + 72, stub + LINK_MISS);              /*JNE miss (generation)*/
    link_rel32(stub + LINK_GVAL + 13, stub + LINK_MISS);  /*JNE miss (group)*/
    link_rel32(stub + LINK_STATUS + 6, stub + LINK_MISS); /*JNE miss (key)*/
    link_rel32(stub + LINK_JMP + 1, stub + LINK_MISS);
}

/* An indirect exit: a near RET, CALL or indirect JMP has stored a PC known
   only now. The same checks as a link, then the indirect link table
   looked up by the linear PC; a hit that holds for the CS base, the key,
   and its block's link generation and group word goes straight to the
   block, a miss leaves for the dispatcher, which fills the entry
   (codegen_ind_fill). At its longest: the tests and the lookup (186
   bytes), the miss path (16), the exit's JMP and the service's CALL at 13
   bytes each, TEST, JNZ and the JMP back. */
#    define IND_SIZE (186 + 16 + 13 + 13 + 2 + 6 + 5)

void
codegen_backend_indirect_stub(codeblock_t *block)
{
    const int32_t sig    = (int32_t) ((uintptr_t) &cpu_state.chain_signal - ((uintptr_t) &cpu_state + 128));
    const int32_t cyc    = (int32_t) ((uintptr_t) &cpu_state._cycles - ((uintptr_t) &cpu_state + 128));
    const int32_t flr    = (int32_t) ((uintptr_t) &cpu_state.chain_floor - ((uintptr_t) &cpu_state + 128));
    const int32_t flg_hi = (int32_t) ((uintptr_t) &cpu_state.flags + 1 - ((uintptr_t) &cpu_state + 128));
    const int32_t pcoff  = (int32_t) ((uintptr_t) &cpu_state.pc - ((uintptr_t) &cpu_state + 128));
    const int32_t csoff  = (int32_t) ((uintptr_t) &cpu_state.seg_cs.base - ((uintptr_t) &cpu_state + 128));
    const int32_t acc    = (int32_t) ((uintptr_t) &cpu_state.seg_cs.access - ((uintptr_t) &cpu_state + 128));
    uint8_t      *stub;
    uint8_t      *exit_at;
    uint8_t      *service_at;
    uint8_t      *miss_at;
    uint8_t      *j_sig, *j_cyc, *j_tf, *j_pc, *j_cs, *j_st, *j_gen, *j_grp;

    codegen_alloc_bytes(block, IND_SIZE);
    stub = &block_write_data[block_pos];

#    define HERE (&block_write_data[block_pos])
    codegen_addbyte3(block, 0x48, 0x8b, 0x85); /*MOV RAX, [RBP+sig]*/
    codegen_addlong(block, (uint32_t) sig);
    codegen_addbyte3(block, 0x83, 0x38, 0x00); /*CMP DWORD [RAX], 0*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE service*/
    codegen_addlong(block, 0);
    j_sig = HERE - 4;
    /*back:*/
    codegen_addbyte2(block, 0x8b, 0x85); /*MOV EAX, [RBP+cycles]*/
    codegen_addlong(block, (uint32_t) cyc);
    codegen_addbyte2(block, 0x3b, 0x85); /*CMP EAX, [RBP+chain_floor]*/
    codegen_addlong(block, (uint32_t) flr);
    codegen_addbyte2(block, 0x0f, 0x8e); /*JLE exit*/
    codegen_addlong(block, 0);
    j_cyc = HERE - 4;
    codegen_addbyte2(block, 0xf6, 0x85); /*TEST BYTE [RBP+flags+1], 1 (TF)*/
    codegen_addlong(block, (uint32_t) flg_hi);
    codegen_addbyte(block, 0x01);
    codegen_addbyte2(block, 0x0f, 0x85); /*JNE exit*/
    codegen_addlong(block, 0);
    j_tf = HERE - 4;
    codegen_addbyte2(block, 0x8b, 0x85); /*MOV EAX, [RBP+pc]*/
    codegen_addlong(block, (uint32_t) pcoff);
    codegen_addbyte2(block, 0x03, 0x85); /*ADD EAX, [RBP+cs base]: the linear PC*/
    codegen_addlong(block, (uint32_t) csoff);
    codegen_addbyte2(block, 0x89, 0xc1);       /*MOV ECX, EAX*/
    codegen_addbyte3(block, 0xc1, 0xe9, 0x0c); /*SHR ECX, 12*/
    codegen_addbyte2(block, 0x31, 0xc1);       /*XOR ECX, EAX*/
    codegen_addbyte2(block, 0x81, 0xe1);       /*AND ECX, SIZE-1*/
    codegen_addlong(block, CODEGEN_IND_SIZE - 1);
    codegen_addbyte4(block, 0x48, 0xc1, 0xe1, 0x06); /*SHL RCX, 6 (64-byte entries)*/
    codegen_addbyte2(block, 0x48, 0xba);             /*MOV RDX, &codegen_ind_cache*/
    codegen_addquad(block, (uint64_t) (uintptr_t) codegen_ind_cache);
    codegen_addbyte3(block, 0x48, 0x01, 0xca); /*ADD RDX, RCX*/
    codegen_addbyte2(block, 0x3b, 0x02);       /*CMP EAX, [RDX] (pc)*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE miss*/
    codegen_addlong(block, 0);
    j_pc = HERE - 4;
    codegen_addbyte2(block, 0x8b, 0x85); /*MOV EAX, [RBP+cs base]*/
    codegen_addlong(block, (uint32_t) csoff);
    codegen_addbyte3(block, 0x3b, 0x42, 0x04); /*CMP EAX, [RDX+4] (cs)*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE miss*/
    codegen_addlong(block, 0);
    j_cs = HERE - 4;
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, &cpu_cur_status*/
    codegen_addquad(block, (uint64_t) (uintptr_t) &cpu_cur_status);
    codegen_addbyte3(block, 0x0f, 0xb7, 0x00); /*MOVZX EAX, WORD [RAX]*/
    codegen_addbyte3(block, 0x0f, 0xb6, 0x8d); /*MOVZX ECX, BYTE [RBP+cs access]*/
    codegen_addlong(block, (uint32_t) acc);
    codegen_addbyte3(block, 0x83, 0xe1, 0x60); /*AND ECX, 60h (DPL = CPL)*/
    codegen_addbyte3(block, 0xc1, 0xe1, 0x0b); /*SHL ECX, 11*/
    codegen_addbyte2(block, 0x09, 0xc8);       /*OR EAX, ECX: CODEGEN_CHAIN_KEY*/
    codegen_addbyte3(block, 0x3b, 0x42, 0x08); /*CMP EAX, [RDX+8] (key)*/
    codegen_addbyte2(block, 0x0f, 0x85);       /*JNE miss*/
    codegen_addlong(block, 0);
    j_st = HERE - 4;
    /* The block's link generation, as a link's (codegen_block.c). */
    codegen_addbyte4(block, 0x48, 0x8b, 0x42, 0x20); /*MOV RAX, [RDX+32] (&link_gen)*/
    codegen_addbyte3(block, 0x48, 0x8b, 0x00);       /*MOV RAX, [RAX]*/
    codegen_addbyte4(block, 0x48, 0x3b, 0x42, 0x10); /*CMP RAX, [RDX+16] (gen)*/
    codegen_addbyte2(block, 0x0f, 0x85);             /*JNE miss*/
    codegen_addlong(block, 0);
    j_gen = HERE - 4;
    /* And its group's word, as a link's. */
    codegen_addbyte4(block, 0x48, 0x8b, 0x42, 0x28); /*MOV RAX, [RDX+40] (group word)*/
    codegen_addbyte3(block, 0x48, 0x8b, 0x00);       /*MOV RAX, [RAX]*/
    codegen_addbyte4(block, 0x48, 0x3b, 0x42, 0x30); /*CMP RAX, [RDX+48] (value)*/
    codegen_addbyte2(block, 0x0f, 0x85);             /*JNE miss*/
    codegen_addlong(block, 0);
    j_grp = HERE - 4;
    codegen_addbyte3(block, 0xff, 0x62, 0x18); /*JMP [RDX+24]*/
    /*miss: have the dispatcher fill the entry*/
    miss_at = HERE;
    codegen_addbyte2(block, 0x48, 0xb8); /*MOV RAX, &codegen_ind_miss*/
    codegen_addquad(block, (uint64_t) (uintptr_t) &codegen_ind_miss);
    codegen_addbyte2(block, 0xc7, 0x00); /*MOV DWORD [RAX], 1*/
    codegen_addlong(block, 1);
    /*exit:*/
    exit_at = HERE;
    host_x86_JMP(block, codegen_exit_rout);
    /*service: as a link's*/
    service_at = HERE;
    host_x86_CALL(block, (void *) codegen_chain_service);
    codegen_addbyte2(block, 0x85, 0xc0); /*TEST EAX, EAX*/
    codegen_addbyte2(block, 0x0f, 0x85); /*JNZ exit*/
    codegen_addlong(block, 0);
    link_rel32(HERE - 4, exit_at);
    codegen_addbyte(block, 0xe9); /*JMP back*/
    codegen_addlong(block, 0);
    link_rel32(HERE - 4, stub + 16);
#    undef HERE

    if ((&block_write_data[block_pos] - stub) > IND_SIZE)
        fatal("codegen_backend_indirect_stub: %i bytes\n", (int) (&block_write_data[block_pos] - stub));

    link_rel32(j_sig, service_at);
    link_rel32(j_cyc, exit_at);
    link_rel32(j_tf, exit_at);
    link_rel32(j_pc, miss_at);
    link_rel32(j_cs, miss_at);
    link_rel32(j_st, miss_at);
    link_rel32(j_gen, miss_at);
    link_rel32(j_grp, miss_at);
}

/* The dispatcher found `target` valid where a chain left through `stub`
   for want of a link: the stub goes straight there from now on, for as
   long as the target's link generation and the key it saw hold. */
int
codegen_chain_patch(uint8_t *stub, codeblock_t *target, uint64_t gen)
{
    uint8_t *dest = &target->data[target->chain_entry];
    intptr_t diff = (intptr_t) dest - (intptr_t) (stub + LINK_JMP + 5);

    if ((diff < INT32_MIN) || (diff > INT32_MAX))
        return 0;

    uint64_t  gval;
    uint64_t *gword = codegen_link_group_word(target, &gval);

    *(uint64_t *) (stub + LINK_GENPTR) = (uint64_t) (uintptr_t) &CODEBLOCK_LINK_GEN(target);
    *(uint64_t *) (stub + LINK_GEN)    = gen;
    *(uint64_t *) (stub + LINK_GWORD)  = (uint64_t) (uintptr_t) gword;
    *(uint64_t *) (stub + LINK_GVAL)   = gval;
    *(uint32_t *) (stub + LINK_STATUS) = CODEGEN_CHAIN_KEY();
    link_rel32(stub + LINK_JMP + 1, dest);
    return 1;
}
#endif

/* x86 keeps one processor's loads and stores in order (TSO) for another
   to see: nothing to change. */
void
codegen_mem_order(int ordered)
{
    (void) ordered;
}

void
codegen_backend_epilogue(codeblock_t *block)
{
#ifdef _WIN64
    /* XMM6 and XMM7 hold guest FPU/MMX values in blocks, and the Windows
       x64 ABI makes them the caller's: put back what the caller had. */
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM6, REG_RSP, CODEGEN_XMM6_SAVE);
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM7, REG_RSP, CODEGEN_XMM7_SAVE);
    host_x86_ADD64_REG_IMM(block, REG_RSP, CODEGEN_WIN64_FRAME);
#else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x48);
#endif
    host_x86_POP(block, REG_R15);
    host_x86_POP(block, REG_R14);
    host_x86_POP(block, REG_R13);
    host_x86_POP(block, REG_R12);
#ifdef _WIN64
    host_x86_POP(block, REG_RDI);
    host_x86_POP(block, REG_RSI);
#endif
    host_x86_POP(block, REG_RBP);
    host_x86_POP(block, REG_RBX);
    host_x86_RET(block);
}
#endif
