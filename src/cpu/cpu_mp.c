/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          More than one processor on one board, taking turns on the one
 *          emulation thread: with one core, the state copied in and out,
 *          or with a core of their own each (copy mode, cpu_core.h). The
 *          first way is described here; the second keeps its rules for the
 *          state.
 *
 *          HOW. Every piece of processor state lives in globals the
 *          interpreter and the recompiler address directly -- the
 *          recompiler bakes &cpu_state into the code it generates -- so the
 *          state is never moved. Each processor has a context, a copy of
 *          every such global, and the one that runs is the one whose copy is
 *          in place. A switch copies the running processor's globals out and
 *          the next one's in, at a block boundary and never from inside a
 *          callback. Compiled blocks do not depend on which processor
 *          compiled them, so with one core both share one code cache;
 *          with a core each, each has its own.
 *
 *          The processors take turns over the same stretch of time: the
 *          first runs a slice and advances the machine's clock as it always
 *          has, then each other one runs the same slice with the clock wound
 *          back to its start and timers held, and the clock is put back
 *          where the first left it. Each processor sees its own time move
 *          forward and the two stay within a slice of each other.
 *
 *          The software TLB is per processor too, as a processor's TLB is
 *          its own: each has a set of lookup tables (mem_tlb_t), and a
 *          switch puts the arriving one's set in place by pointer
 *          (mem_tlb_select), copying nothing. A flush in one set reaches the
 *          others when they are next in place; a page that gains code loses
 *          its direct write entries in every set at once (mem_cpu.c).
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x87_sf.h"
#include "x86seg.h"
#include <86box/mem.h>
#include <86box/smram.h>
#include <86box/nmi.h>
#include <86box/cpu_mp.h>
#include "cpu_core.h"

#ifdef ENABLE_CPU_MP_LOG
int cpu_mp_do_log = ENABLE_CPU_MP_LOG;

static void
cpu_mp_log(const char *fmt, ...)
{
    va_list ap;

    if (cpu_mp_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define cpu_mp_log(fmt, ...)
#endif

extern int  ioapic_external_interrupt_pending_at(int index);
extern void lapic_index_board_reset(uint8_t index);

/* What a processor is doing. */
enum {
    CPU_MP_RUNNING = 0,
    CPU_MP_WAIT_SIPI, /* after RESET or INIT, until a STARTUP arrives */
    CPU_MP_STARTING,  /* a STARTUP has arrived; it starts at its next turn */
    /* An SMI reached it waiting for a STARTUP: it runs, only to take the
       SMI at once and run the handler, and its RSM returns it to waiting
       (cpu_core_waiting). Waiting again once it is out of SMM with no SMI
       raised and not yet taken (cpu_mp_smm_wait_over). A P6 BIOS moves
       each processor's SMBASE so: INIT, then an SMI IPI to the one
       waiting. */
    CPU_MP_SMM_WAIT
};

#define CPU_MP_VARS_MAX 64

typedef struct cpu_mp_var_t {
    void    *addr;
    uint32_t size;
    uint8_t  init_keeps; /* INIT leaves it as it is (cpu_mp_init_image) */
} cpu_mp_var_t;

typedef struct cpu_mp_ctx_t {
    uint8_t   *blob;
    mem_tlb_t *tlb;
    int        state;
    uint8_t    sipi_vector;
    uint8_t    halted;       /* its last slice ended in HLT */
    uint8_t    nmi_pending;  /* an NMI arrived while it was not in place: 1 the pin's, 2 the APIC's */
    uint8_t    smi_pending;  /* and an SMI */
    uint8_t    init_pending; /* an INIT to it while it ran, or in SMM: taken at a slice's end */
} cpu_mp_ctx_t;

/* Memory read-modify-writes are compare-and-swaps (386_common.h) while
   another processor could be running beside this one: never while they
   take turns. */
int cpu_mp_atomic = 0;

int cpu_mp_count         = 1;
int cpu_mp_cur           = 0;
int cpu_mp_timers_frozen = 0;

int cpu_count        = 1;
int cpu_smp_drift_us = CPU_SMP_DRIFT_DEFAULT;

/* Slices adapt between the shortest and the SMP drift setting: they double
   while the processors leave each other alone and drop back to the
   shortest the moment they interact (an IPI, a halted one woken, INIT or
   STARTUP), so an exchange is answered within the shortest slice. */
#define CPU_MP_SLICE_MIN_US 5
static int cpu_mp_slice_now_us = CPU_MP_SLICE_MIN_US;
static int cpu_mp_interacted;

/* Each processor core's list of its per-processor variables: the same
   variables in the same order, at its own addresses. The images hold them
   in that order. */
static cpu_mp_var_t cpu_mp_vars[CPU_MP_MAX][CPU_MP_VARS_MAX];
static int          cpu_mp_var_counts[CPU_MP_MAX];
static uint32_t     cpu_mp_blob_sizes[CPU_MP_MAX];
static int          cpu_mp_reg_copy; /* whose list cpu_mp_register() adds to */
#define cpu_mp_var_count cpu_mp_var_counts[0]
#define cpu_mp_blob_size cpu_mp_blob_sizes[0]
static cpu_mp_ctx_t cpu_mp_ctx[CPU_MP_MAX];
static int          cpu_mp_in_smm(int index);
static int          cpu_mp_smm_wait_over(int index);
static uint8_t     *cpu_mp_reset_blob; /* a processor straight out of RESET */
/* Room to build what INIT leaves: the processor's state before it, and the
   image made from it. */
static uint8_t     *cpu_mp_init_cur;
static uint8_t     *cpu_mp_init_img;
/* A reset asked for by a processor other than the first, for the first to
   do (cpu_mp_run_pending_reset). */
int                 cpu_mp_reset_pending;
static uint64_t     cpu_mp_slice_tsc;

void
cpu_mp_register(void *addr, uint32_t size)
{
    cpu_mp_var_t *vars = cpu_mp_vars[cpu_mp_reg_copy];
    int          *n    = &cpu_mp_var_counts[cpu_mp_reg_copy];

    for (int i = 0; i < *n; i++)
        if (vars[i].addr == addr)
            return;

    if (*n >= CPU_MP_VARS_MAX)
        fatal("cpu_mp: too many per-processor variables\n");

    vars[*n].addr       = addr;
    vars[*n].size       = size;
    vars[*n].init_keeps = 0;
    (*n)++;
    cpu_mp_blob_sizes[cpu_mp_reg_copy] += size;
}

/* A per-processor variable INIT leaves as it is: the floating-point state,
   the model-specific registers, the time-stamp counter (Pentium Vol. 3,
   16.1 and Table 16-1). */
void
cpu_mp_register_keep(void *addr, uint32_t size)
{
    cpu_mp_register(addr, size);
    for (int i = 0; i < cpu_mp_var_counts[cpu_mp_reg_copy]; i++)
        if (cpu_mp_vars[cpu_mp_reg_copy][i].addr == addr)
            cpu_mp_vars[cpu_mp_reg_copy][i].init_keeps = 1;
}

/* What INIT leaves, into img, from the processor's state cur: what RESET
   leaves, but "it does not disturb the internal caches, model specific
   registers, or floating point state" (Pentium Vol. 3, 16.1): the time-
   stamp counter, the event counters and every other MSR are unchanged, and
   of CR0, CD and NW are unchanged, bit 4 set and the rest cleared
   (Table 16-1). cpu_state is the first variable of every image. */
static void
cpu_mp_init_image(uint8_t *img, const uint8_t *cur, const uint8_t *reset)
{
    uint32_t           off = 0;
    cpu_state_t       *s   = (cpu_state_t *) img;
    const cpu_state_t *c   = (const cpu_state_t *) cur;

    memcpy(img, reset, cpu_mp_blob_size);
    for (int i = 0; i < cpu_mp_var_count; i++) {
        if (cpu_mp_vars[0][i].init_keeps)
            memcpy(img + off, cur + off, cpu_mp_vars[0][i].size);
        off += cpu_mp_vars[0][i].size;
    }

    /* The floating-point registers, and where the last FPU instruction and
       its operand were. */
    memcpy(s->tag, c->tag, sizeof(s->tag));
    s->TOP  = c->TOP;
    s->npxs = c->npxs;
    s->npxc = c->npxc;
    memcpy(s->ST, c->ST, sizeof(s->ST));
    memcpy(s->MM_w4, c->MM_w4, sizeof(s->MM_w4));
    memcpy(s->MM, c->MM, sizeof(s->MM));
    s->ismmx          = c->ismmx;
    s->old_fp_control = c->old_fp_control;
    s->new_fp_control = c->new_fp_control;
#if defined __amd64__ || defined _M_X64
    s->trunc_fp_control = c->trunc_fp_control;
#endif
#pragma push_macro("x87_op")
#undef x87_op /* cpu.h names cpu_state's own */
    s->x87_op = c->x87_op;
#pragma pop_macro("x87_op")
    s->fpu_CS = c->fpu_CS;
    s->fpu_cs = c->fpu_cs;
    s->fpu_pc = c->fpu_pc;
    s->fpu_op = c->fpu_op;
    s->fpu_DS = c->fpu_DS;
    s->fpu_ds = c->fpu_ds;
    s->fpu_ea = c->fpu_ea;

    s->CR0.l = (c->CR0.l & 0x60000000) | 0x00000010;

    /* "SMBASE is set to the default value of 30000H on RESET, but is not
       changed on INIT" (Pentium Vol. 3, 20.1.5.3). */
    s->_smbase = c->_smbase;
}

/* ---- processor cores ------------------------------------------------------

   Where the build has it (CPU_MP_SECOND_CORE, src/CMakeLists.txt), the
   core is built a second time with its globals renamed (cpu_core.h), and
   the second processor runs in that copy: its state stays where it is
   between turns and nothing is copied to switch. Otherwise the processors
   share the one core and their state is swapped in and out of it. */
static const cpu_core_ops_t *cpu_mp_core[CPU_MP_MAX];
static int                   cpu_mp_copy_mode;
#ifdef CPU_MP_SECOND_CORE
static int cpu_mp_core_inited[CPU_MP_MAX];
#endif
static int32_t cpu_mp_c0_cycles; /* the first core's, while another runs */

int
cpu_mp_copy_mode_active(void)
{
    return cpu_mp_copy_mode;
}

/* The core the processor in place runs in. */
static const cpu_core_ops_t *
cpu_mp_cur_core(void)
{
    return cpu_mp_copy_mode ? cpu_mp_core[cpu_mp_cur] : cpu_core_ops();
}

/* CR0 of the processor in place, for a device that asks (in copy mode
   another processor's is in its own core). */
uint32_t
cpu_mp_current_cr0(void)
{
    if (cpu_mp_copy_mode && (cpu_mp_cur > 0) && (cpu_mp_cur < cpu_mp_count))
        return ((const cpu_state_t *) cpu_mp_vars[cpu_mp_cur][0].addr)->CR0.l;
    return cr0;
}

/* IGNNE# to every processor: the board drives one line to all of them, and
   a processor with a core of its own keeps its own copy of the pin. Taking
   turns in one core, they share it already. */
void
cpu_mp_ignne_assert(void)
{
    if (!cpu_mp_copy_mode)
        return;
    for (int i = 1; i < cpu_mp_count; i++)
        cpu_mp_core[i]->ignne_assert();
}

#ifdef CPU_MP_SECOND_CORE
/* Processor index's core, when it has one of its own. */
static const cpu_core_ops_t *
cpu_mp_ap_core_ops(int index)
{
    return (index == 1) ? s1_cpu_core_ops() : NULL;
}
#endif

/* Every core's state list, the first's first. */
static void
cpu_mp_register_all(void)
{
    cpu_mp_reg_copy = 0;
    cpu_core_ops()->register_state();
    /* cpu_mp_init_image finds cpu_state at the start of every image. */
    if ((cpu_mp_var_counts[0] < 1) || (cpu_mp_vars[0][0].size != sizeof(cpu_state_t)))
        fatal("cpu_mp: cpu_state is not the first per-processor variable\n");
#ifdef CPU_MP_SECOND_CORE
    for (int c = 1; c < CPU_MP_MAX; c++) {
        cpu_mp_reg_copy = c;
        cpu_mp_ap_core_ops(c)->register_state();

        /* One image fits every core: the same variables, the same sizes,
           the same order, each at the core's own address. */
        if ((cpu_mp_var_counts[c] != cpu_mp_var_counts[0]) || (cpu_mp_blob_sizes[c] != cpu_mp_blob_sizes[0]))
            fatal("cpu_mp: core %i's state list differs from the first's\n", c);
        for (int i = 0; i < cpu_mp_var_counts[0]; i++)
            if ((cpu_mp_vars[c][i].size != cpu_mp_vars[0][i].size) || (cpu_mp_vars[c][i].addr == cpu_mp_vars[0][i].addr))
                fatal("cpu_mp: core %i's state list differs from the first's at %i\n", c, i);
    }
    cpu_mp_reg_copy = 0;
#endif
}

static void
cpu_mp_save_blob_of(int copy, uint8_t *blob)
{
    uint32_t off = 0;

    for (int i = 0; i < cpu_mp_var_counts[copy]; i++) {
        memcpy(blob + off, cpu_mp_vars[copy][i].addr, cpu_mp_vars[copy][i].size);
        off += cpu_mp_vars[copy][i].size;
    }
}

static void
cpu_mp_load_blob_into(int copy, const uint8_t *blob)
{
    uint32_t off = 0;

    for (int i = 0; i < cpu_mp_var_counts[copy]; i++) {
        memcpy(cpu_mp_vars[copy][i].addr, blob + off, cpu_mp_vars[copy][i].size);
        off += cpu_mp_vars[copy][i].size;
    }
}

#define cpu_mp_save_blob(b) cpu_mp_save_blob_of(0, (b))
#define cpu_mp_load_blob(b) cpu_mp_load_blob_into(0, (b))

/* Processor `index` back in the state an image holds, its TLB empty:
   loaded into its own core in copy mode, or into its saved context for its
   next turn. */
static void
cpu_mp_load_state(int index, const uint8_t *blob)
{
    if (cpu_mp_copy_mode) {
        cpu_mp_load_blob_into(index, blob);
        cpu_mp_core[index]->after_load();
    } else {
        memcpy(cpu_mp_ctx[index].blob, blob, cpu_mp_blob_size);
        cpu_mp_ctx[index].tlb->stale = MEM_TLB_STALE_ALL;
    }
}

/* Processor index's state as it stands, as an image, into img. */
static void
cpu_mp_state_image(int index, uint8_t *img)
{
    if (cpu_mp_copy_mode)
        cpu_mp_save_blob_of(index, img);
    else
        memcpy(img, cpu_mp_ctx[index].blob, cpu_mp_blob_size);
}

/* End the slice of the processor in place now. */
static void
cpu_mp_stop_current(void)
{
    cpu_mp_cur_core()->stop();
}

/* ---- switching ---------------------------------------------------------- */

static void
cpu_mp_switch(int index)
{
    if ((cpu_mp_count < 2) || (index == cpu_mp_cur) || (index < 0) || (index >= cpu_mp_count))
        return;

    cpu_mp_save_blob(cpu_mp_ctx[cpu_mp_cur].blob);
    cpu_mp_load_blob(cpu_mp_ctx[index].blob);
    mem_tlb_select(cpu_mp_ctx[index].tlb);
    /* Its own view of SMRAM; its TLB set was filled through that view. */
    mem_smm_view(!!(cpu_cur_status & CPU_STATUS_SMM));
    /* Its own CR0.CD, and the memory timings with it. */
    if (cpu_cache_int_enabled != (hascache && !(cr0 & (1 << 30)))) {
        cpu_cache_int_enabled = hascache && !(cr0 & (1 << 30));
        cpu_update_waitstates();
    }

    cpu_mp_cur = index;

    if (cpu_mp_ctx[index].nmi_pending) {
        const int apic = (cpu_mp_ctx[index].nmi_pending == 2);

        cpu_mp_ctx[index].nmi_pending = 0;
        cpu_nmi_raise_core(apic);
    }
    if (cpu_mp_ctx[index].smi_pending) {
        cpu_mp_ctx[index].smi_pending = 0;
        cpu_smi_raise_direct();
    }
}

/* Whether a halted processor's maskable interrupts could end its HLT:
   "An enabled interrupt, NMI, or a reset will resume execution" (Pentium
   Vol. 3, HLT). Its IF, from its own core or its saved state (cpu_state is
   the first thing saved). */
static int
cpu_mp_interrupts_enabled(int index)
{
    const cpu_state_t *st = cpu_mp_copy_mode ? (const cpu_state_t *) cpu_mp_vars[index][0].addr :
                                               (const cpu_state_t *) cpu_mp_ctx[index].blob;

    return (st == NULL) || !!(st->flags & I_FLAG);
}

static int
cpu_mp_external_wakes(int index)
{
    return cpu_mp_interrupts_enabled(index) && ioapic_external_interrupt_pending_at(index);
}

static int
cpu_mp_runnable(int index)
{
    if ((index <= 0) || (index >= cpu_mp_count))
        return 0;

    /* Waiting for a STARTUP, only an SMI runs it (CPU_MP_SMM_WAIT). */
    if ((cpu_mp_ctx[index].state == CPU_MP_WAIT_SIPI) && !cpu_mp_ctx[index].smi_pending)
        return 0;

    /* A processor halted with nothing to wake it costs nothing: its slice
       would only run HLT again. Anything that could end the HLT -- an
       interrupt its local APIC would deliver, ExtINT through LINT0, an
       NMI or SMI -- gives it its turn back. */
    if (cpu_mp_ctx[index].halted && !cpu_mp_ctx[index].nmi_pending &&
        !cpu_mp_ctx[index].smi_pending &&
        !cpu_mp_external_wakes(index))
        return 0;

    if (cpu_mp_ctx[index].halted)
        cpu_mp_interacted = 1; /* woken: it is being answered */

    return 1;
}

void
cpu_mp_note_interaction(void)
{
    cpu_mp_interacted = 1;
}

/* How long the next slice runs, in microseconds. */
int
cpu_mp_next_slice_us(void)
{
    if (cpu_mp_interacted) {
        cpu_mp_interacted   = 0;
        cpu_mp_slice_now_us = CPU_MP_SLICE_MIN_US;
    } else if (cpu_mp_slice_now_us < cpu_smp_drift_us) {
        cpu_mp_slice_now_us *= 2;
        if (cpu_mp_slice_now_us > cpu_smp_drift_us)
            cpu_mp_slice_now_us = cpu_smp_drift_us;
    }

    return cpu_mp_slice_now_us;
}

/* An NMI for one processor: taken now if it is the one in place, else
   when it next is. */
static void
cpu_mp_nmi_ex(int index, int apic)
{
    if ((cpu_mp_count < 2) || (index < 0) || (index >= cpu_mp_count) || (index == cpu_mp_cur)) {
        cpu_mp_cur_core()->nmi_raise(apic);
        return;
    }

    /* 1 the NMI pin's, 2 the APIC's (not gated by port 70h), kept at the
       higher if both come. */
    if (apic)
        cpu_mp_ctx[index].nmi_pending = 2;
    else if (!cpu_mp_ctx[index].nmi_pending)
        cpu_mp_ctx[index].nmi_pending = 1;
}

/* An NMI for one processor from its NMI pin (nmi_raise), or from its
   local APIC. */
void
cpu_mp_nmi(int index)
{
    cpu_mp_nmi_ex(index, 0);
}

void
cpu_mp_nmi_apic(int index)
{
    cpu_mp_nmi_ex(index, 1);
}

/* An SMI for one processor, the same way. */
void
cpu_mp_smi(int index)
{
    if ((cpu_mp_count < 2) || (index < 0) || (index >= cpu_mp_count) || (index == cpu_mp_cur)) {
        cpu_mp_cur_core()->smi_raise();
        return;
    }

    cpu_mp_ctx[index].smi_pending = 1;
}

/* A processor other than the first takes its turn: in place, the clock
   wound back to where the first processor's slice began, timers held.
   Answers whether it is to run. */
int
cpu_mp_begin_slice(int index, uint64_t start_tsc)
{
    if (!cpu_mp_runnable(index))
        return 0;

    cpu_mp_slice_tsc = tsc; /* where the first processor left it */

    if ((cpu_mp_ctx[index].state == CPU_MP_WAIT_SIPI) && cpu_mp_ctx[index].smi_pending)
        cpu_mp_ctx[index].state = CPU_MP_SMM_WAIT;

    if (cpu_mp_copy_mode) {
        const cpu_core_ops_t *core = cpu_mp_core[index];
        cpu_core_config_t     cfg;

        /* Anything the machine charges while this one runs lands in the
           first core's count; it is taken back at the end of the slice. */
        cpu_mp_c0_cycles = cycles;
        cpu_mp_cur       = index;

        cpu_core_ops()->config_get(&cfg);
        core->config_set(&cfg);
        core->tlb_catch_up();
        core->tsc_set(start_tsc);
        mem_tlb_flush_running  = core->tlb_flush_now;
        mem_fill_read_running  = core->fill_read;
        mem_fill_write_running = core->fill_write;

        if (cpu_mp_ctx[index].nmi_pending) {
            const int apic = (cpu_mp_ctx[index].nmi_pending == 2);

            cpu_mp_ctx[index].nmi_pending = 0;
            core->nmi_raise(apic);
        }
        if (cpu_mp_ctx[index].smi_pending) {
            cpu_mp_ctx[index].smi_pending = 0;
            core->smi_raise();
        }

        if (cpu_mp_ctx[index].state == CPU_MP_STARTING) {
            const uint8_t vector = cpu_mp_ctx[index].sipi_vector;

            core->startup(vector);
            cpu_mp_ctx[index].state = CPU_MP_RUNNING;
            cpu_mp_log("CPU MP: processor %i starts at %04X:0000\n", index, vector << 8);
        }

        core->halted_set(cpu_mp_ctx[index].halted);
        cpu_mp_timers_frozen = 1;
        return 1;
    }

    cpu_mp_switch(index);
    tsc = start_tsc;

    if (cpu_mp_ctx[index].state == CPU_MP_STARTING) {
        /* Real mode at VV00:0000, from the state RESET leaves. */
        uint8_t vector = cpu_mp_ctx[index].sipi_vector;

        loadcs((uint16_t) vector << 8);
        cpu_state.pc            = 0;
        cycles                  = 0;
        cpu_core_waiting        = 0;
        cpu_mp_ctx[index].state = CPU_MP_RUNNING;
        cpu_mp_log("CPU MP: processor %i starts at %04X:0000\n", index, vector << 8);
    }

    /* Its own halt state until a block of its says otherwise: a slice in
       which it runs nothing must not record the first processor's. */
    cpu_halted           = cpu_mp_ctx[index].halted;
    cpu_mp_timers_frozen = 1;
    return 1;
}

/* A device bringing the clock up to date while another core runs: it is
   that core's cycles that have passed. Answers whether it was handled. */
int
cpu_mp_update_tsc_running(void)
{
    if (!cpu_mp_copy_mode || (cpu_mp_cur == 0))
        return 0;

    cpu_mp_core[cpu_mp_cur]->update_tsc();
    return 1;
}

/* End the slice of whichever processor is running (a reset the machine
   asked of the first processor, which acts on it when back in place). */
void
cpu_mp_stop_running(void)
{
    if (cpu_mp_count > 1)
        cpu_mp_stop_current();
}

/* The processor in place runs its share of the slice in its own core.
   Answers non-zero if the debugger wants control back. */
int
cpu_mp_run_slice(int32_t cycs)
{
    return cpu_mp_core[cpu_mp_cur]->run(cycs);
}

/* A reset asked for by a processor other than the first, done now with
   the first in place. */
static void
cpu_mp_run_pending_reset(void)
{
    int p;
    int was_reset;

    if (cpu_mp_cur != 0)
        return;
    p = cpu_mp_reset_pending;
    cpu_mp_reset_pending = 0;
    was_reset     = x86_was_reset;
    x86_was_reset = 0;
    if (p == 2)
        resetx86();
    else if ((p == 3) && in_smm) {
        /* Held in SMM until RSM (Pentium Vol. 1, spec update 49), looked at
           when taken: an SMI taken at the same boundary comes first. */
        cpu_mp_reset_pending = 3;
    } else if (p == 3) {
        /* INIT, like RESET, leaves the signature in EDX; the FPU it leaves
           alone (Pentium Vol. 3, Table 16-1), so not cpu_set_edx, which
           resets the softfloat FPU too. */
        apic_initx86();
        EDX = cpu_s->edx_reset;
    } else if (p == 1) {
        /* What the callers of a soft reset do after it, done here for
           the processor actually reset (a hard reset sets EDX itself). */
        softresetx86();
        cpu_set_edx();
    }
    x86_was_reset |= was_reset;
}

void
cpu_mp_end_slice(void)
{
    cpu_mp_ctx[cpu_mp_cur].halted = cpu_mp_cur_core()->halted_get();
    cpu_mp_timers_frozen          = 0;

    if (cpu_mp_copy_mode) {
        const cpu_core_ops_t *core = cpu_mp_core[cpu_mp_cur];

        mem_tlb_flush_running  = NULL;
        mem_fill_read_running  = NULL;
        mem_fill_write_running = NULL;
        /* What the machine charged the first core's count while this one
           ran was this one's: it pays at its next turn. */
        core->charge(cpu_mp_c0_cycles - cycles);
        cycles     = cpu_mp_c0_cycles;
        cpu_mp_cur = 0;
        tsc        = cpu_mp_slice_tsc;
        mem_tlb_catch_up();

        /* An NMI or SMI for the first processor that came while the other
           ran waited in its context; it is taken now it is back. */
        if (cpu_mp_ctx[0].nmi_pending) {
            const int apic = (cpu_mp_ctx[0].nmi_pending == 2);

            cpu_mp_ctx[0].nmi_pending = 0;
            cpu_core_ops()->nmi_raise(apic);
        }
        if (cpu_mp_ctx[0].smi_pending) {
            cpu_mp_ctx[0].smi_pending = 0;
            cpu_core_ops()->smi_raise();
        }
    } else {
        cpu_mp_switch(0);
        tsc = cpu_mp_slice_tsc;
    }

    for (int i = 1; i < cpu_mp_count; i++) {
        /* Out of the SMM it entered from waiting for a STARTUP. */
        if ((cpu_mp_ctx[i].state == CPU_MP_SMM_WAIT) && cpu_mp_smm_wait_over(i)) {
            cpu_mp_ctx[i].state  = CPU_MP_WAIT_SIPI;
            cpu_mp_ctx[i].halted = 0;
        }
        if (cpu_mp_ctx[i].init_pending)
            cpu_mp_init_signal((uint8_t) i);
    }

    cpu_mp_run_pending_reset();
}

/* ---- RESET, INIT and STARTUP -------------------------------------------- */

/* A reset is the machine's and the first processor's business. Asked for
   while another processor is in place, it waits for the first. */
int
cpu_mp_defer_reset(int self, int hard)
{
    if ((cpu_mp_count < 2) || (self == 0))
        return 0;

    if (hard)
        cpu_mp_reset_pending = 2;
    else if ((cpu_mp_reset_pending == 0) || (cpu_mp_reset_pending == 3)) {
        /* A soft reset is INIT to every socket: it takes the place of an
           INIT of the first processor alone still waiting (3), and yields
           to a hard one (2). */
        cpu_mp_reset_pending = 1;
    }

    /* The reset happens at the end of this processor's slice, so end it
       now: left as it is, a processor that shut down on a triple fault
       faults again at once, costing no cycles, and the slice never ends. */
    if (cpu_mp_copy_mode)
        cpu_mp_core[self]->stop();
    else
        cpu_mp_stop_current();
    return 1;
}

/* A processor put back into its reset state out of place has left SMM
   without an RSM: take it off SMIACT#, and give the bus its view back if
   it was the last one in. */
static void
cpu_mp_smm_forget(int index)
{
    if (mem_smm_leave(index))
        smram_recalc_all(1);
}

/* The board's RESET (hard) or INIT pin (soft: port 92, the keyboard
   controller, a shutdown cycle -- 82434NX PCMC, INIT) reaches every socket:
   the others go through it and wait for a STARTUP. INIT keeps what INIT
   keeps (cpu_mp_init_image). */
static void
cpu_mp_park_others(int hard)
{
    for (int i = 1; (i < cpu_mp_count) && (i < CPU_MP_MAX); i++) {
        /* The INIT pin to a processor in SMM waits for its RSM (Pentium
           Vol. 1, spec update 49), as an APIC INIT does: held, and taken
           at a slice's end after it (cpu_mp_init_signal). */
        if (!hard && cpu_mp_in_smm(i)) {
            lapic_index_board_reset((uint8_t) i);
            cpu_mp_ctx[i].init_pending = 1;
            continue;
        }
        if (hard)
            cpu_mp_load_state(i, cpu_mp_reset_blob); /* RESET empties the TLB too */
        else {
            cpu_mp_state_image(i, cpu_mp_init_cur);
            cpu_mp_init_image(cpu_mp_init_img, cpu_mp_init_cur, cpu_mp_reset_blob);
            cpu_mp_load_state(i, cpu_mp_init_img);
        }
        cpu_mp_smm_forget(i);
        lapic_index_board_reset((uint8_t) i); /* and its APIC waits for STARTUP */
        cpu_mp_ctx[i].state        = CPU_MP_WAIT_SIPI;
        cpu_mp_ctx[i].sipi_vector  = 0;
        cpu_mp_ctx[i].halted       = 0;
        cpu_mp_ctx[i].nmi_pending  = 0;
        cpu_mp_ctx[i].smi_pending  = 0;
        cpu_mp_ctx[i].init_pending = 0;
    }
}

/* After the first processor has been through RESET or INIT. The INIT pin
   is wired to every socket, and the second processor, the Dual, comes out
   of either waiting for a STARTUP (Pentium Vol. 1, 3.4.1.4). Its state is
   what RESET leaves, with the Dual's processor type in its CPUID signature
   (Table 2-11: type 10b). */
void
cpu_mp_after_reset(int self, int hard)
{
    /* A hard reset ends whatever reset or INIT was still waiting, with one
       processor too (an INIT to itself posted in the same chunk). */
    if (hard && (self == 0))
        cpu_mp_reset_pending = 0;

    if ((cpu_mp_count < 2) || (self != 0))
        return;

    /* The RESET image, from the first processor as RESET left it: only a
       hard reset leaves that. A soft one is INIT, which keeps state
       (Pentium Vol. 1, 3.3), so the image stays the one from the last
       RESET. The time stamp counter, the Control and Event Select register
       and TR12 are zero in it as in the first's (Pentium Vol. 3, Table
       16-1). */
    if (hard) {
        int      saved     = CPUID;
        uint32_t saved_cr0 = cr0;
        uint32_t saved_cr2 = cr2;
        uint32_t saved_cr3 = cr3;
        uint32_t saved_dr[8];

        /* The debug registers, CR2 and CR3 as RESET leaves them (Table
           16-1), not as the first processor had them; the Dual's signature
           and its CR0 after RESET, 60000010H (Figure 16-2). They go into
           the image through the variables themselves, so the image's layout
           stays this file's business. */
        memcpy(saved_dr, dr, sizeof(saved_dr));
        dr[0] = dr[1] = dr[2] = dr[3] = 0;
        dr[6] = 0xffff0ff0;
        dr[7] = 0x00000400;
        cr2   = 0;
        cr3   = 0;

        /* Type 10b is the P5's Dual (Table 2-11); a P6 pair are equals. */
        if (cpu_is_p5())
            CPUID |= 0x2000;
        cr0 = 0x60000010;
        /* Out of RESET or INIT the others wait for a STARTUP: every one of
           them is loaded from this image for either, and the first never
           is. */
        cpu_core_waiting = 1;
        cpu_mp_save_blob(cpu_mp_reset_blob);
        cpu_core_waiting = 0;
        CPUID            = saved;
        cr0              = saved_cr0;
        cr2              = saved_cr2;
        cr3              = saved_cr3;
        memcpy(dr, saved_dr, sizeof(saved_dr));
    }
    cpu_mp_park_others(hard);
}

/* A STARTUP delivered to a processor's local APIC. Answers whether that
   processor will now run. */
int
cpu_mp_startup(uint8_t index, uint8_t vector)
{
    if ((cpu_mp_count < 2) || (index == 0) || (index >= cpu_mp_count))
        return 0;

    if (cpu_mp_ctx[index].state != CPU_MP_WAIT_SIPI) {
        cpu_mp_log("CPU MP: STARTUP to processor %i ignored: it is %s, not waiting for one\n", index,
                   (cpu_mp_ctx[index].state == CPU_MP_RUNNING)  ? "running" :
                   (cpu_mp_ctx[index].state == CPU_MP_SMM_WAIT) ? "in SMM from wait-for-STARTUP" :
                                                                  "starting");
        return 0; /* only after RESET or INIT */
    }

    cpu_mp_ctx[index].sipi_vector = vector;
    cpu_mp_ctx[index].state       = CPU_MP_STARTING;
    cpu_mp_ctx[index].halted      = 0;
    cpu_mp_interacted             = 1;
    cpu_mp_log("CPU MP: STARTUP to processor %i, vector %02X\n", index, vector);
    return 1;
}

/* Processor index's cpu_state, wherever it is: its own core's, the live
   one of the processor in place, or its saved context. NULL before there
   is one. */
static const cpu_state_t *
cpu_mp_state_of(int index)
{
    if (cpu_mp_copy_mode)
        return (const cpu_state_t *) cpu_mp_vars[index][0].addr;
    if (index == cpu_mp_cur)
        return &cpu_state;
    return (const cpu_state_t *) cpu_mp_ctx[index].blob;
}

/* Whether processor index is in SMM, wherever its state is. */
static int
cpu_mp_in_smm(int index)
{
    const cpu_state_t *st = cpu_mp_state_of(index);

    return (st != NULL) && st->_in_smm;
}

/* Whether processor index, in SMM from waiting for a STARTUP, is waiting
   again: out of SMM through its RSM, and no SMI raised in it that it has
   not taken yet -- one latched in SMM is taken at the RSM (opRSM). A
   processor that would not take the SMI at all (Cyrix, SMM switched off)
   ends the same way. */
static int
cpu_mp_smm_wait_over(int index)
{
    const cpu_state_t *st = cpu_mp_state_of(index);

    return (st == NULL) || (!st->_in_smm && !st->_smi_line);
}

/* Processor index's RSM (leave_smm, not waiting for a STARTUP): an INIT it
   held in SMM is taken now, before anything after the RSM runs. Answers
   whether one waits; its slice then ends there, and its slice's end takes
   it (cpu_mp_end_slice). */
int
cpu_mp_rsm_init_waits(int index)
{
    if ((cpu_mp_count < 2) || (index <= 0) || (index >= cpu_mp_count))
        return 0;
    return cpu_mp_ctx[index].init_pending;
}

extern int cpu_exec386_active; /* 386_dynarec.c */

/* The first processor's INIT, from its local APIC, at a block boundary
   with it in place (cpu_mp_after_chunk): it may be the one that sent it. */
static void
cpu_mp_init_first(void)
{
    /* A board reset already asked for covers it. */
    if (cpu_mp_reset_pending == 0)
        cpu_mp_reset_pending = 3;
    /* The interpreter takes it at the next instruction boundary ("recognized
       at instruction boundaries", Pentium Vol. 1, 2.7) by itself; stopping
       it mid-instruction charged the rest of its budget to the clock at
       once. The recompiler's chunk is ended here. */
    if ((cpu_mp_cur == 0) && !cpu_exec386_active)
        cpu_mp_stop_current();
    cpu_mp_log("CPU MP: INIT to processor 0\n");
}

/* After each of the first processor's chunks, with it in place: an INIT
   its APIC took meanwhile. */
void
cpu_mp_after_chunk(void)
{
    if ((cpu_mp_cur == 0) && cpu_mp_reset_pending)
        cpu_mp_run_pending_reset();
}

/* An INIT delivered to a processor's local APIC: the Dual goes back to
   waiting for a STARTUP, in the state INIT leaves; the first goes through
   INIT and runs on from the reset vector. */
void
cpu_mp_init_signal(uint8_t index)
{
    if (index == 0) {
        cpu_mp_init_first();
        return;
    }
    if ((cpu_mp_count < 2) || (index >= cpu_mp_count))
        return;

    /* In SMM an INIT waits: "the processor exits SMM and services the
       pending NMI or INIT" (Pentium Vol. 1, spec update 49). Held, it is
       looked at again at each slice's end (cpu_mp_end_slice). */
    if (cpu_mp_in_smm(index)) {
        cpu_mp_ctx[index].init_pending = 1;
        return;
    }

    /* To itself (a self-INIT, or one to all including self): it stops
       now and goes back to waiting when its slice ends, with the first
       processor in place. */
    if (index == cpu_mp_cur) {
        cpu_mp_log("CPU MP: INIT to processor %i, itself: taken at the end of its slice\n", index);
        cpu_mp_ctx[index].init_pending = 1;
        cpu_mp_stop_current();
        return;
    }

    cpu_mp_ctx[index].init_pending = 0;
    cpu_mp_state_image(index, cpu_mp_init_cur);
    cpu_mp_init_image(cpu_mp_init_img, cpu_mp_init_cur, cpu_mp_reset_blob);
    cpu_mp_load_state(index, cpu_mp_init_img); /* INIT empties the TLB too */
    cpu_mp_smm_forget(index);
    cpu_mp_ctx[index].state       = CPU_MP_WAIT_SIPI;
    cpu_mp_ctx[index].halted      = 0;
    cpu_mp_ctx[index].nmi_pending = 0;
    cpu_mp_ctx[index].smi_pending = 0;
    cpu_mp_log("CPU MP: INIT to processor %i\n", index);
}

/* ---- configuration ------------------------------------------------------ */

void
cpu_mp_configure(int count)
{
    /* The TSC is the machine's clock again until a RESET or a WRMSR gives
       it an offset (cpu_tsc_write). */
    cpu_tsc_offset = 0;

    if (count < 1)
        count = 1;
    if (count > CPU_MP_MAX)
        count = CPU_MP_MAX;
#if !defined(USE_DYNAREC)
    /* Only the recompiler's loop runs the others (386_dynarec.c), with the
       recompiler switched on or off; the build without it has one. */
    count = 1;
#endif

#ifdef CPU_MP_SECOND_CORE
    /* The other cores leave the machine's memory. */
    if (cpu_mp_copy_mode) {
        for (int i = 1; i < cpu_mp_count; i++)
            cpu_mp_core[i]->detach();
    }
#endif
    cpu_mp_copy_mode       = 0;
    mem_tlb_flush_running  = NULL;
    mem_fill_read_running  = NULL;
    mem_fill_write_running = NULL;

    /* Processor 0 keeps the TLB set every machine has; the others' go. */
    mem_tlb_select(mem_tlb_default());
    for (int i = 0; i < CPU_MP_MAX; i++) {
        free(cpu_mp_ctx[i].blob);
        cpu_mp_ctx[i].blob = NULL;
        if ((i > 0) && (cpu_mp_ctx[i].tlb != NULL))
            mem_tlb_destroy(cpu_mp_ctx[i].tlb);
        cpu_mp_ctx[i].tlb = NULL;
        cpu_mp_core[i]    = NULL;
    }
    free(cpu_mp_reset_blob);
    free(cpu_mp_init_cur);
    free(cpu_mp_init_img);
    cpu_mp_reset_blob = NULL;
    cpu_mp_init_cur   = NULL;
    cpu_mp_init_img   = NULL;

    cpu_mp_count         = count;
    cpu_mp_cur           = 0;
    cpu_mp_slice_now_us  = CPU_MP_SLICE_MIN_US;
    cpu_mp_interacted    = 0;
    cpu_mp_timers_frozen = 0;
    cpu_mp_reset_pending = 0;

    if (count < 2)
        return;

    if (cpu_mp_var_count == 0)
        cpu_mp_register_all();

#ifdef CPU_MP_SECOND_CORE
    /* Each other processor gets a core of its own: its tables and code
       cache once, the machine's model and settings, its TLB set and page
       table copy in the machine's memory. */
    {
        cpu_core_config_t cfg;

        cpu_core_ops()->config_get(&cfg);
        cpu_mp_core[0] = cpu_core_ops();
        for (int i = 1; i < count; i++) {
            const cpu_core_ops_t *core = cpu_mp_ap_core_ops(i);

            if (!cpu_mp_core_inited[i]) {
                core->init();
                cpu_mp_core_inited[i] = 1;
            }
            core->self_set(i);
            core->config_set(&cfg);
            core->set_model();
            /* cpu_set() put its own defaults in; the machine's again. */
            core->config_set(&cfg);
            core->attach();
            cpu_mp_core[i] = core;
        }
        cpu_mp_copy_mode = 1;
    }
#endif

    cpu_mp_reset_blob = calloc(1, cpu_mp_blob_size);
    cpu_mp_init_cur   = calloc(1, cpu_mp_blob_size);
    cpu_mp_init_img   = calloc(1, cpu_mp_blob_size);
    for (int i = 0; i < count; i++) {
        cpu_mp_ctx[i].blob = calloc(1, cpu_mp_blob_size);
        /* A processor taking its turns in the first core has a TLB set of
           its own put in place for them; one with a core of its own has
           its core's. */
        cpu_mp_ctx[i].tlb          = (i == 0) ? mem_tlb_default() : (cpu_mp_copy_mode ? NULL : mem_tlb_create());
        cpu_mp_ctx[i].halted       = 0;
        cpu_mp_ctx[i].nmi_pending  = 0;
        cpu_mp_ctx[i].smi_pending  = 0;
        cpu_mp_ctx[i].init_pending = 0;
        cpu_mp_ctx[i].state        = (i == 0) ? CPU_MP_RUNNING : CPU_MP_WAIT_SIPI;
    }
    cpu_mp_log("CPU MP: %i processors, %u bytes of state each, %s\n", count, cpu_mp_blob_size,
               cpu_mp_copy_mode ? "each in a core of its own" : "sharing one core");
}
