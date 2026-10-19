/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          One processor core, as the rest of the machine reaches it.
 *
 *          The core -- interpreter, recompiler, the processor's side of
 *          memory -- names its state as fixed globals. For the other
 *          processors the core is built a second time (CPU_AP_CORE) and
 *          copied three times with those globals renamed (s1_ to s3_ for
 *          processors 1 to 3), so each processor has its state at addresses
 *          of its own and nothing is copied to switch between them. Code
 *          outside the core reaches a core through this table:
 *          cpu_core_ops() in the first, s1_cpu_core_ops() to
 *          s3_cpu_core_ops() in the others.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_CPU_CORE_H
#define EMU_CPU_CORE_H

#include <stdint.h>

/* What the machine sets in the first core that another has to match: bus
   and cache timing from the chipset, reset wiring, the model. */
typedef struct cpu_core_config_t {
    const void *cpu_f;
    int         cpu_effective;
    int         cpu_cache_ext_enabled;
    int         cpu_waitstates;
    int         cpu_isa_speed;
    int         cpu_pci_speed;
    int         cpu_agp_speed;
    int         cpu_isa_pci_div;
    int         io_waitstates;
    int         isa_cycles;
    int         reg_op_waitstates;
    int         cpu_alt_reset;
    int         cpu_cpurst_on_sr;
    int         soft_reset_pci;
    int         reset_on_hlt;
    int         unmask_a20_in_smm;
    int         cpu_override_dynarec;
    int         cpu_override_interpreter;
    int         cpu_use_exec;
    int         cpu_dyn_accurate_fpu_env;
    int         cpu_force_interpreter;
    double      cpu_busspeed;
    double      cpu_dmulti;
} cpu_core_config_t;

typedef struct cpu_core_ops_t {
    /* Once: the tables and code cache a core needs before it runs. */
    void (*init)(void);
    /* Take the model from the machine (cpu_set() without the machine's
       parts: the APICs and the ports are the machine's, set up once). */
    void (*set_model)(void);
    /* Join the machine's memory: its TLB set and page table copy, and
       leave it again. */
    void (*attach)(void);
    void (*detach)(void);

    /* cpu_mp_register() every per-processor variable, in the one order
       every core shares, so one core's state image loads into another. */
    void (*register_state)(void);
    /* After its state was replaced from an image: nothing it had
       translated or compiled against the old state may survive. */
    void (*after_load)(void);

    void (*config_get)(cpu_core_config_t *c);
    void (*config_set)(const cpu_core_config_t *c);

    /* Run for cycs more cycles; answers non-zero if the debugger wants
       control back. */
    int (*run)(int32_t cycs);
    /* End the run now (a reset or INIT to itself). */
    void (*stop)(void);
    /* Cycles something outside the core spent on its behalf. */
    void (*charge)(int32_t cycs);
    /* A run left early (its mailbox, cpu_thread.c): what it did not run
       is not owed. */
    void (*trim)(void);
    /* A device access it made was dropped (a RESET is coming): the
       instruction ends without effect, and so does the run. */
    void (*discard)(void);

    void     (*nmi_raise)(int apic); /* apic: from the local APIC, not gated by port 70h */
    void     (*smi_raise)(void);
    void     (*startup)(uint8_t vector);
    uint64_t (*tsc_get)(void);
    void     (*tsc_set)(uint64_t tsc);

    /* A flush of every set reaching this core's while it runs, and what
       reached it while it did not (mem_cpu.c). */
    void (*tlb_flush_now)(void);
    void (*tlb_catch_up)(void);
    /* Leaving its blocks (1) or going back into them (0). */
    void (*tlb_outside)(int outside);
    /* Its TLB set: where its chains' signal word is. */
    struct mem_tlb_t *(*tlb_set)(void);

    /* A RAM handler's TLB fill for this core's last access. */
    void (*fill_read)(uint32_t phys);
    void (*fill_write)(uint32_t phys);

    /* Bring the clock up to this core's cycles (a device asking). */
    void (*update_tsc)(void);

    /* Whether its last run ended in HLT. */
    int  (*halted_get)(void);
    void (*halted_set)(int h);

    /* IGNNE# asserted (a write to port F0h): the board drives the one line
       to every processor. */
    void (*ignne_assert)(void);

    /* The processor it runs (cpu_core_self). */
    void (*self_set)(int index);

    /* The debug console's instruction trace (386_dynarec.c): start one
       into a file, as processor cpu, at most max records (0: no limit);
       stop it; and how far it has got. */
    int      (*trace_start)(const char *path, uint64_t max, uint8_t cpu);
    uint64_t (*trace_stop)(void);
    uint64_t (*trace_state)(int *on);
} cpu_core_ops_t;

extern const cpu_core_ops_t *cpu_core_ops(void);

/* The other processors' cores, one copy each (processors 1 to 3). */
extern const cpu_core_ops_t *s1_cpu_core_ops(void);
extern const cpu_core_ops_t *s2_cpu_core_ops(void);
extern const cpu_core_ops_t *s3_cpu_core_ops(void);

#endif /*EMU_CPU_CORE_H*/
