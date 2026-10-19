/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The APIC bus's lock: every local APIC and I/O APIC together.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_APIC_LOCK_H
#define EMU_APIC_LOCK_H

#include <stdint.h>

/* Each processor reaches its own local APIC from its own thread, as a
   processor's APIC is part of it; the machine's thread reaches the I/O
   APIC and delivers into the local APICs. Messages cross the APIC bus
   between all of them, so their state is one thing, held by one lock.
   It is taken where the APIC code is entered from outside it, and nested
   entries (an EOI that makes the I/O APIC send again) take it again.
   Under it nothing waits for another processor and nothing is handed to
   the machine's thread: what only that thread may do -- the timers -- is
   left for it (cpu_lapic_timer_hosts_apply). */
extern void apic_lock(void);       /* for the processor the machine's thread has in place */
extern void apic_lock_as(int who); /* for processor who, from its own thread */
extern void apic_unlock(void);
extern int  apic_lock_held(void);
extern int  apic_lock_release_all(void);
extern void apic_lock_reacquire(int depth, int asker);
/* The processor asking while the lock is held (apic_lock_as). */
extern int apic_bus_asker;

/* Processor who's accesses to its own local APIC's window, from its own
   thread (lapic_mmio.c): each takes the lock as who. TPR, and PPR read,
   need no lock (lapic_mmio_fast_reg answers whether addr is one, in who's
   own window, and which). */
extern uint8_t  lapic_readb_as(int who, uint32_t addr, void *priv);
extern uint16_t lapic_readw_as(int who, uint32_t addr, void *priv);
extern uint32_t lapic_readl_as(int who, uint32_t addr, void *priv);
extern void     lapic_writeb_as(int who, uint32_t addr, uint8_t val, void *priv);
extern void     lapic_writew_as(int who, uint32_t addr, uint16_t val, void *priv);
extern void     lapic_writel_as(int who, uint32_t addr, uint32_t val, void *priv);
extern int      lapic_mmio_fast_reg(int who, uint32_t addr, uint32_t *reg);

#endif /*EMU_APIC_LOCK_H*/
