/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Idle loops skipped (x86_idle.c).
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_X86_IDLE_H
#define EMU_X86_IDLE_H

/* A pass of an idle loop is being proven: every load and store goes through
   the slow paths (mem.c), which log it here. */
extern int  x86_idle_watch;
extern void x86_idle_note(uint32_t addr, int size, int write, uint64_t val);
/* A compiled block has returned while a pass is being proven. */
extern void x86_idle_tick(void);

/* A compiled PAUSE (codegen.c). */
extern void x86_pause(uint32_t eip);
/* An IN AL,DX at eip just read a port whose reads only repeat
   (io_poll_pure). */
extern void x86_idle_in(uint32_t eip);

/* The processor's clock now, inside a compiled block (386_dynarec.c). */
extern uint64_t exec386_now_tsc(void);

#endif
