/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          One processor core's entry points for the rest of the machine
 *          (cpu_core.h). Built into every copy of the core; each copy's
 *          table reaches that copy's own state.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(__APPLE__) && defined(__aarch64__)
#    include <pthread.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x87_sf.h"
#include "x86seg.h"
#ifdef USE_DYNAREC
#    include "codegen_public.h"
#endif
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/cpu_mp.h>
#include "cpu_core.h"

extern void   cpu_mp_register_dynarec(void);
extern void   cpu_mp_register_mmx(void);
extern int    cpu_core_run_chunk(int32_t cycs);
extern void   x86_tables_init(void);
extern void   update_tsc(void);
extern int    rf_flag_no_clear;
extern int    cpu_isa_pci_div;
extern int    cpu_dyn_accurate_fpu_env;
extern int    cpu_force_interpreter;
extern double exp_pow_table[0x800];

#define CPU_CORE_REG(v) cpu_mp_register(&(v), sizeof(v))
/* One INIT leaves as it is (cpu_mp.c, cpu_mp_init_image). */
#define CPU_CORE_REG_KEEP(v) cpu_mp_register_keep(&(v), sizeof(v))

/* The processor waits for a STARTUP: from its RESET or INIT (it comes in
   the image cpu_mp.c loads for those) until the STARTUP. It runs nothing
   meanwhile but an SMI, taken at once (386_dynarec.c), whose RSM returns
   it to waiting, not to the state the SMI saved (leave_smm). A P6 BIOS
   moves each processor's SMBASE so: INIT, then an SMI IPI to the one
   waiting. Never set in the first processor. */
int cpu_core_waiting = 0;

/* Which processor this core runs: 0 in the first core (which also runs
   the others in turn when they have no core of their own, cpu_mp_cur),
   1 to 3 in the others' copies, set as each is given its processor. */
int cpu_core_self = 0;

#ifdef CPU_AP_CORE
/* The second processor's own physical address mask: "A20M# is internally
   masked by the Pentium processor when configured as a Dual processor"
   (Pentium Vol. 1, Table 4-2), and the board's A20 gate is the boot
   processor's (MP spec 1.4, 3.9). The first core's is the machine's
   (mem.c). */
uint32_t rammask = 0xffffffff;
#endif

static void
cpu_core_self_set(int index)
{
    cpu_core_self = index;
}

static int
cpu_core_halted_get(void)
{
    return cpu_halted;
}

static void
cpu_core_halted_set(int h)
{
    cpu_halted = h;
}

static void
cpu_core_ignne_assert(void)
{
    /* Only while this processor's FERR# is up, as the board's latch. */
    if (fpu_ferr)
        fpu_ignne = 1;
}

/* Everything a processor carries that is not the machine's. What is in
   cpu_state is most of it; the rest are the globals round it. */
static void
cpu_core_register_state(void)
{
    CPU_CORE_REG(cpu_state);
    CPU_CORE_REG_KEEP(fpu_state);
    CPU_CORE_REG_KEEP(msr);
    CPU_CORE_REG(gdt);
    CPU_CORE_REG(ldt);
    CPU_CORE_REG(idt);
    CPU_CORE_REG(tr);
    CPU_CORE_REG(_oldds);
    CPU_CORE_REG(cr2);
    CPU_CORE_REG(cr3);
    CPU_CORE_REG(cr4);
    CPU_CORE_REG(dr);
    CPU_CORE_REG(use32);
    CPU_CORE_REG(stack32);
    CPU_CORE_REG(cpu_cur_status);
    CPU_CORE_REG(CPUID);
    CPU_CORE_REG(nmi);
    CPU_CORE_REG(nmi_auto_clear);
    CPU_CORE_REG(nmi_enable);
    CPU_CORE_REG(new_ne);
    CPU_CORE_REG(trap);
    CPU_CORE_REG(cpu_init);
    CPU_CORE_REG(cpu_end_block_after_ins);
    CPU_CORE_REG(rf_flag_no_clear);
    CPU_CORE_REG(smi_latched);
    CPU_CORE_REG(smm_in_hlt);
    CPU_CORE_REG(smi_block);
    CPU_CORE_REG(old_rammask);
    CPU_CORE_REG(cpu_old_paging);
    CPU_CORE_REG(cpu_flush_pending);
    CPU_CORE_REG(cpl_override);
    CPU_CORE_REG(oldds);
    CPU_CORE_REG(oldss);
    CPU_CORE_REG(olddslimit);
    CPU_CORE_REG(oldsslimit);
    CPU_CORE_REG(olddslimitw);
    CPU_CORE_REG(oldsslimitw);
    CPU_CORE_REG_KEEP(cpu_tsc_offset);
    CPU_CORE_REG(cpu_core_waiting);
    /* The code held after a locked instruction (386_common.c). */
    CPU_CORE_REG(cpu_fa_state);
    CPU_CORE_REG(cpu_fa_lin);
    CPU_CORE_REG(cpu_fa_len);
    CPU_CORE_REG(cpu_fa_resume);
    CPU_CORE_REG(cpu_fa_buf);

    /* The ones that live as statics in their own files. */
#ifdef USE_DYNAREC
    cpu_mp_register_dynarec();
#endif
    cpu_mp_register_mmx();
}

static void
cpu_core_init(void)
{
    for (int c = 0; c <= 0x7ff; c++)
        exp_pow_table[c] = pow(2.0, (double) (c - 1023)); /* 1023 = BIAS64 */

    x86_tables_init();
#ifdef USE_DYNAREC
    /* codegen_init writes this core's memory routines into its code cache:
       on Apple silicon, only with the calling thread's JIT write protection
       off, as pc_init_modules() does for the first core. */
#    if defined(__APPLE__) && defined(__aarch64__)
    if (__builtin_available(macOS 11.0, *)) {
        pthread_jit_write_protect_np(0);
    }
#    endif
    codegen_init();
#    if defined(__APPLE__) && defined(__aarch64__)
    if (__builtin_available(macOS 11.0, *)) {
        pthread_jit_write_protect_np(1);
    }
#    endif
#endif
}

static void
cpu_core_set_model(void)
{
    cpu_set();
}

static void
cpu_core_attach(void)
{
    mem_tlb_clear(mem_tlb_default());
    mem_tlb_register(mem_tlb_default());
    mem_cpu_register_pages();
    mem_cpu_reset();
    flushmmucache_cpu();
#ifdef USE_DYNAREC
    codegen_reset();
#endif
}

static void
cpu_core_detach(void)
{
    mem_tlb_unregister(mem_tlb_default());
    mem_page_copy_unregister(&pages);
}

static void
cpu_core_after_load(void)
{
    /* The image is the first core's: what in cpu_state points into a core
       points into this one again. */
    cpu_state.ea_seg = &cpu_state.seg_ds;
#ifdef USE_DYNAREC
    cpu_state.lookup2_r = readlookup2;
    cpu_state.lookup2_w = writelookup2;
#endif

    mem_smm_view(!!(cpu_cur_status & CPU_STATUS_SMM));
    flushmmucache_cpu();

    /* The cache enable follows the new CR0.CD, and the timings with it. */
    cpu_cache_int_enabled = hascache && !(cr0 & (1 << 30));
    cpu_update_waitstates();
}

static void
cpu_core_config_get(cpu_core_config_t *c)
{
    c->cpu_f                    = cpu_f;
    c->cpu_effective            = cpu_effective;
    c->cpu_cache_ext_enabled    = cpu_cache_ext_enabled;
    c->cpu_waitstates           = cpu_waitstates;
    c->cpu_isa_speed            = cpu_isa_speed;
    c->cpu_pci_speed            = cpu_pci_speed;
    c->cpu_agp_speed            = cpu_agp_speed;
    c->cpu_isa_pci_div          = cpu_isa_pci_div;
    c->io_waitstates            = io_waitstates;
    c->isa_cycles               = isa_cycles;
    c->reg_op_waitstates        = reg_op_waitstates;
    c->cpu_alt_reset            = cpu_alt_reset;
    c->cpu_cpurst_on_sr         = cpu_cpurst_on_sr;
    c->soft_reset_pci           = soft_reset_pci;
    c->reset_on_hlt             = reset_on_hlt;
    c->unmask_a20_in_smm        = unmask_a20_in_smm;
    c->cpu_override_dynarec     = cpu_override_dynarec;
    c->cpu_override_interpreter = cpu_override_interpreter;
    c->cpu_use_exec             = cpu_use_exec;
    c->cpu_dyn_accurate_fpu_env = cpu_dyn_accurate_fpu_env;
    c->cpu_force_interpreter    = cpu_force_interpreter;
    c->cpu_busspeed             = cpu_busspeed;
    c->cpu_dmulti               = cpu_dmulti;
}

/* The chipset's cache and wait-state settings as last applied here: the
   memory timings follow from them and this core's own CR0.CD. */
static int cpu_core_applied_ext = -1;
static int cpu_core_applied_ws  = -1;
static int cpu_core_applied_eff = -1;

static void
cpu_core_config_set(const cpu_core_config_t *c)
{
    cpu_f                    = (cpu_family_t *) c->cpu_f;
    cpu_effective            = c->cpu_effective;
    cpu_cache_ext_enabled    = c->cpu_cache_ext_enabled;
    cpu_waitstates           = c->cpu_waitstates;
    cpu_isa_speed            = c->cpu_isa_speed;
    cpu_pci_speed            = c->cpu_pci_speed;
    cpu_agp_speed            = c->cpu_agp_speed;
    cpu_isa_pci_div          = c->cpu_isa_pci_div;
    io_waitstates            = c->io_waitstates;
    isa_cycles               = c->isa_cycles;
    reg_op_waitstates        = c->reg_op_waitstates;
    cpu_alt_reset            = c->cpu_alt_reset;
    cpu_cpurst_on_sr         = c->cpu_cpurst_on_sr;
    soft_reset_pci           = c->soft_reset_pci;
    reset_on_hlt             = c->reset_on_hlt;
    unmask_a20_in_smm        = c->unmask_a20_in_smm;
    cpu_override_dynarec     = c->cpu_override_dynarec;
    cpu_override_interpreter = c->cpu_override_interpreter;
    cpu_use_exec             = c->cpu_use_exec;
    cpu_dyn_accurate_fpu_env = c->cpu_dyn_accurate_fpu_env;
    cpu_force_interpreter    = c->cpu_force_interpreter;
    cpu_busspeed             = c->cpu_busspeed;
    cpu_dmulti               = c->cpu_dmulti;

    if ((cpu_cache_ext_enabled != cpu_core_applied_ext) || (cpu_waitstates != cpu_core_applied_ws) ||
        (cpu_effective != cpu_core_applied_eff)) {
        cpu_core_applied_ext = cpu_cache_ext_enabled;
        cpu_core_applied_ws  = cpu_waitstates;
        cpu_core_applied_eff = cpu_effective;
        cpu_update_waitstates();
    }
}

static int
cpu_core_run(int32_t cycs)
{
#ifdef USE_DYNAREC
    return cpu_core_run_chunk(cycs);
#else
    (void) cycs;
    return 0;
#endif
}

static void
cpu_core_stop(void)
{
    cycles = 0;
}

static void
cpu_core_charge(int32_t cycs)
{
    cycles -= cycs;
}

static void
cpu_core_trim(void)
{
    if (cycles > 0)
        cycles = 0;
}

static void
cpu_core_discard(void)
{
    cpu_state.abrt = ABRT_DISCARD;
    cycles         = 0;
}

/* A STARTUP: real mode at VV00:0000, from the state RESET left. */
static void
cpu_core_startup(uint8_t vector)
{
    loadcs((uint16_t) vector << 8);
    cpu_state.pc     = 0;
    cycles           = 0;
    cpu_core_waiting = 0;
}

static uint64_t
cpu_core_tsc_get(void)
{
    return tsc;
}

static void
cpu_core_tsc_set(uint64_t t)
{
    tsc = t;
}

const cpu_core_ops_t *
cpu_core_ops(void)
{
    static const cpu_core_ops_t ops = {
        .init           = cpu_core_init,
        .set_model      = cpu_core_set_model,
        .attach         = cpu_core_attach,
        .detach         = cpu_core_detach,
        .register_state = cpu_core_register_state,
        .after_load     = cpu_core_after_load,
        .config_get     = cpu_core_config_get,
        .config_set     = cpu_core_config_set,
        .run            = cpu_core_run,
        .stop           = cpu_core_stop,
        .charge         = cpu_core_charge,
        .trim           = cpu_core_trim,
        .discard        = cpu_core_discard,
        .nmi_raise      = cpu_nmi_raise_core,
        .smi_raise      = cpu_smi_raise_direct,
        .startup        = cpu_core_startup,
        .tsc_get        = cpu_core_tsc_get,
        .tsc_set        = cpu_core_tsc_set,
        .tlb_flush_now  = mem_tlb_flush_now,
        .tlb_catch_up   = mem_tlb_catch_up,
        .tlb_outside    = mem_tlb_outside,
        .tlb_set        = mem_tlb_current,
        .fill_read      = mem_cpu_fill_read,
        .fill_write     = mem_cpu_fill_write,
#ifdef USE_DYNAREC
        .update_tsc     = update_tsc,
#endif
        .halted_get     = cpu_core_halted_get,
        .halted_set     = cpu_core_halted_set,
        .ignne_assert   = cpu_core_ignne_assert,
        .self_set       = cpu_core_self_set
    };

    return &ops;
}
