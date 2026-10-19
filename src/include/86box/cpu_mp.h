/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          More than one processor on one board, taking turns on the
 *          emulation thread (cpu_mp.c).
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_CPU_MP_H
#define EMU_CPU_MP_H

#include <stdint.h>

#define CPU_MP_MAX 2

/* How many processors the machine has, and which one is executing. Both
   are 1 and 0 on every machine that has not asked for more. */
extern int cpu_mp_count;
extern int cpu_mp_cur;

/* Another processor could be running beside this one: memory
   read-modify-writes are compare-and-swaps, and serializing instructions
   full fences (386_common.h). */
extern int cpu_mp_atomic;

/* Which processor the code running now belongs to, as the processor core
   sees it: another processor's core is always its own processor
   (cpu_core_self, cpu_core.h); the first core's is whichever cpu_mp.c has
   in place (processor 0, or another taking its turn in the first core when
   it has no core of its own). */
extern int cpu_core_self;
#ifdef CPU_AP_CORE
#    define cpu_mp_self cpu_core_self
#else
#    define cpu_mp_self cpu_mp_cur
#endif

/* Set while a processor other than the first runs its share of a slice:
   time is the first processor's to advance, so nothing fires timers. */
extern int cpu_mp_timers_frozen;

/* The Processor settings of a board with more than one socket
   (MACHINE_DUAL_CPU): how many are fitted, and the SMP drift -- how far
   apart, in microseconds of guest time, the processors' clocks may be.
   Taking turns on one thread, it is the longest turn: turns start at 5 us
   and grow towards it while the processors do not interact. Tighter keeps
   the processors closer in step; wider costs the host less switching. */
#define CPU_SMP_DRIFT_DEFAULT 20
extern int cpu_count;
extern int cpu_smp_drift_us;

extern void cpu_mp_note_interaction(void);
extern int  cpu_mp_next_slice_us(void);

extern void cpu_mp_configure(int count);
extern void cpu_mp_register(void *addr, uint32_t size);
extern void cpu_mp_register_keep(void *addr, uint32_t size); /* one INIT leaves as it is */

extern int  cpu_mp_defer_reset(int self, int hard);
extern void cpu_mp_after_reset(int self, int hard);

extern int  cpu_mp_startup(uint8_t index, uint8_t vector);
extern void cpu_mp_after_chunk(void);
extern void cpu_mp_init_signal(uint8_t index);
extern void cpu_mp_nmi(int index);
extern void cpu_mp_nmi_apic(int index);
extern void cpu_mp_smi(int index);
extern int  cpu_mp_rsm_init_waits(int index);

extern int  cpu_mp_begin_slice(int index, uint64_t start_tsc);
extern void cpu_mp_end_slice(void);
extern void cpu_mp_stop_running(void);

/* With a core of their own each (cpu_core.h): whether they have, the
   processor in place's share of a slice run in its core, and what devices
   ask of the processor in place while another than the first runs. */
extern int      cpu_mp_copy_mode_active(void);
extern int      cpu_mp_run_slice(int32_t cycs);
extern int      cpu_mp_update_tsc_running(void);
extern uint32_t cpu_mp_current_cr0(void);
extern void     cpu_mp_ignne_assert(void);

/* A reset asked for by a processor other than the first, for the first to
   do: 0 none, 1 soft, 2 hard, 3 the first processor's INIT from its APIC. */
extern int cpu_mp_reset_pending;

#endif /*EMU_CPU_MP_H*/
