/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The processor core's state, as more than one processor needs
 *          it told apart (cpu_mp.c).
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
#include <86box/nmi.h>
#include <86box/cpu_mp.h>

extern void cpu_mp_register_dynarec(void);
extern void cpu_mp_register_mmx(void);
extern int  rf_flag_no_clear;

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

/* Everything a processor carries that is not the machine's. What is in
   cpu_state is most of it; the rest are the globals round it. */
void
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

    /* The ones that live as statics in their own files. */
#ifdef USE_DYNAREC
    cpu_mp_register_dynarec();
#endif
    cpu_mp_register_mmx();
}
