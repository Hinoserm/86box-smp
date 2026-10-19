/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The other processors' host threads (cpu_thread.c).
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_CPU_THREAD_H
#define EMU_CPU_THREAD_H

#include <stdint.h>

/* What another processor is doing, as its thread tells the machine's
   (cpu_mp_ap_prepare). */
enum {
    AP_RUNNING = 0,
    AP_IDLE_HALTED, /* halted; an interrupt for it wakes it */
    AP_IDLE_OTHER   /* waiting for STARTUP, or for a reset it asked for */
};

struct cpu_core_ops_t;

extern void cpu_thread_attach(int index, const struct cpu_core_ops_t *core);
extern int  cpu_thread_start(int index, const struct cpu_core_ops_t *core);
extern void cpu_thread_stop(void);
extern int  cpu_thread_active(void);
extern int  cpu_thread_self(void);
extern void cpu_thread_poke(int index);
extern void cpu_thread_poke_all(void);
extern void cpu_thread_wake(int index);
extern void cpu_thread_enter(int64_t bound);
extern void cpu_thread_leave(void);
extern void cpu_thread_hold(void);
extern void cpu_thread_release(void);
extern void cpu_thread_split_begin(void);
extern void cpu_thread_split_end(void);
extern void cpu_thread_tick(void);
extern void cpu_thread_main_poll(void);
extern void cpu_thread_drain_all(void);
extern void cpu_thread_machine_attn(void);

/* The processors' side of it (cpu_mp.c). */
extern int  cpu_mp_ap_prepare(int index);
extern void cpu_mp_ap_after_run(int index);
extern int  cpu_mp_ap_wakes(int index);
extern int  cpu_mp_ap_reset_outstanding(int index);
extern int  cpu_mp_ap_hard_reset_outstanding(int index);
extern void cpu_mp_after_serve(void);

#endif /*EMU_CPU_THREAD_H*/
