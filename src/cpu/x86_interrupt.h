/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CPU-facing external interrupt arbitration helpers.
 */
#ifndef EMU_X86_INTERRUPT_H
#define EMU_X86_INTERRUPT_H

#include <86box/pic.h>
#include <86box/cpu_mp.h>

extern int ioapic_external_interrupt_pending(void);
extern int ioapic_external_interrupt_vector(void);
extern int ioapic_pic_fallback_allowed(void);
extern uint8_t ioapic_irq_path_active;

extern uint32_t cpu_lapic_irr_live;
extern int      cpu_lapic_highest_deliverable_irr_index(uint8_t lapic_index, int started);
extern uint8_t  cpu_lapic_extint_live[];
extern uint8_t  cpu_apic_int_fast_ok;

/* IOAPIC arbitrates first; PIC remains only while platform allows fallback. */
static inline int
x86_external_interrupt_pending(void)
{
    if (!ioapic_irq_path_active)
        return pic.int_pending;

    /* Nothing from the 8259, nothing in this processor's IRR, no ExtINT:
       the full answer below would be no. The boot processor still takes
       the full path once after its APIC is enabled or disabled, so the
       I/O APIC sees the new enable state (ioapic_external_interrupt_pending). */
    if (!pic.int_pending && !cpu_lapic_extint_live[cpu_mp_self] &&
        (cpu_apic_int_fast_ok || cpu_mp_self)) {
        if (!(cpu_lapic_irr_live & (1u << cpu_mp_self)))
            return 0;

        /* Something is in IRR but may be held back by TPR/PPR, as it
           often is at a raised IRQL: the one scan the full path would end
           in answers it. Asking as if started can only find more, never
           less, than the full path would. */
        if (cpu_lapic_highest_deliverable_irr_index((uint8_t) cpu_mp_self, 1) < 0)
            return 0;
    }

    if (ioapic_external_interrupt_pending())
        return 1;

    /* The 8259 straight to the processor is the boot processor's path;
       any other takes it only through its own LINT0, above. */
    if (!ioapic_pic_fallback_allowed() || cpu_mp_self)
        return 0;

    return pic.int_pending;
}

static inline int
x86_external_interrupt_vector(void)
{
    if (!ioapic_irq_path_active)
        return picinterrupt();

    if (ioapic_external_interrupt_pending())
        return ioapic_external_interrupt_vector();

    /* Blocked PIC fallback reports no vector without acknowledging the PIC. */
    if (!ioapic_pic_fallback_allowed() || cpu_mp_self)
        return -1;

    return picinterrupt();
}

#endif /*EMU_X86_INTERRUPT_H*/
