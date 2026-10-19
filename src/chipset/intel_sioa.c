/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The Intel 82379AB System I/O APIC (SIO.A) on a peer PCI bus.
 *
 *          A board with a second PCI bus behind a 450GX auxiliary bridge
 *          may carry an SIO.A there for its PCI arbiter and its I/O APIC
 *          alone, with the whole ISA side unconnected: no ISA bus, no DMA,
 *          no utility bus, none of the processor sideband outputs (the Acer
 *          X3, whose service guide shows every ISA pin of it open). What is
 *          left is its PCI configuration space, which the BIOS may program,
 *          and the I/O APIC, relocated by APICBASE (71h).
 *
 *          The registers and their defaults are Intel's 82378ZB/82379AB
 *          data sheet (290571-001), 3.1; the 82379AB's revision is 88h.
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
#include <86box/device.h>
#include <86box/pci.h>
#include <86box/chipset.h>
#include <86box/plat_unused.h>

typedef struct sioa_t {
    uint8_t slot;
    uint8_t regs[256];
    /* PACC (70h) and APICBASE (71h) are write-only (3.1.26, 3.1.27): what
       was written, kept apart from what a read returns. */
    uint8_t pacc;
    uint8_t apicbase;

    /* Its I/O APIC, and the host bridge whose APIC range decides whether
       the processors reach it (i450gx_apic_range). */
    void   *ioapic;
    void   *gx;

    /* Its IRQ inputs, the bus's interrupts as the board steers them: which
       IRQ each card's INTx is driving, and how many drive each IRQ. */
    uint8_t line[PCI_CARDS_NUM][4];
    uint8_t drivers[16];
} sioa_t;

/* The I/O APIC's base: FEC0_xy00h, x from APICBASE bits 5:2 (A15:12), y
   from bits 1:0 (A11:10), in 1 Kbyte steps. */
uint32_t
sioa_apic_base(const void *priv)
{
    const sioa_t *dev = (const sioa_t *) priv;

    return 0xfec00000 | ((uint32_t) (dev->apicbase & 0x3f) << 10);
}

/* Its I/O APIC's window: at APICBASE, reached while the auxiliary host
   bridge forwards that address to this bus. The board calls this when the
   bridge's range changes. */
void
sioa_apic_update(void *priv)
{
    sioa_t        *dev  = (sioa_t *) priv;
    const uint32_t base = sioa_apic_base(dev);
    uint32_t       lo   = 0;
    uint32_t       hi   = 0;
    int            on;

    if (dev->ioapic == NULL)
        return;
    on = i450gx_apic_range(dev->gx, 1, &lo, &hi) && (base >= lo) && ((base + 0x3ff) <= hi);
    ioapic_secondary_map(dev->ioapic, base, on);
}

/* A card on its bus raised or dropped an INTx, steered to irq_line. Its
   APIC's INTIN n is IRQ n (Figure 8): IRQ3-7, 9-12, 14 and 15 are inputs
   the board may wire; what drives each is the OR of the cards on it. */
static void
sioa_irq_observe(void *priv, uint8_t slot, uint8_t pci_int_index, uint8_t irq_line, int set)
{
    sioa_t  *dev = (sioa_t *) priv;
    uint8_t *cur;
    uint8_t  was;

    if ((slot >= PCI_CARDS_NUM) || (pci_int_index > 3))
        return;
    cur = &dev->line[slot][pci_int_index];
    was = *cur;

    if (!set)
        irq_line = 0xff;
    if (irq_line == was)
        return;
    if ((was < 16) && dev->drivers[was] && !--dev->drivers[was])
        ioapic_secondary_line(dev->ioapic, was, 0);
    *cur = irq_line;
    if ((irq_line < 16) && !dev->drivers[irq_line]++)
        ioapic_secondary_line(dev->ioapic, irq_line, 1);
}

/* The board's wiring: its I/O APIC, the host bridge in front of its bus,
   and that bus's number (86Box's index). */
void
sioa_attach(void *priv, void *ioapic, void *gx, uint8_t bus)
{
    sioa_t *dev = (sioa_t *) priv;

    dev->ioapic = ioapic;
    dev->gx     = gx;
    pci_set_bus_irq_observer(bus, sioa_irq_observe, dev);
    sioa_apic_update(dev);
}

static void
sioa_write(int func, int addr, UNUSED(int len), uint8_t val, void *priv)
{
    sioa_t  *dev  = (sioa_t *) priv;
    uint8_t *regs = dev->regs;

    if (func > 0)
        return;

    switch (addr) {
        case 0x04: /* COM: only SCE (bit 3) is writable; BME, MSE, IOSE read 1 */
            regs[addr] = (regs[addr] & 0xf7) | (val & 0x08);
            break;

        case 0x07: /* DS: MA and RTA are R/WC; STA is read-only */
            regs[addr] &= ~(val & 0x30);
            break;

        case 0x40: /* PCICON */
            regs[addr] = val & 0x7f;
            break;
        case 0x41: /* PAC */
            regs[addr] = val & 0x1f;
            break;
        case 0x42: /* PAPC */
            regs[addr] = val;
            break;
        case 0x43: /* ARBPRIX */
            regs[addr] = val & 0x01;
            break;
        case 0x44: /* MCSCON */
            regs[addr] = val & 0x1f;
            break;
        case 0x45 ... 0x4b: /* MCSBOH, MCSTOH, MCSTOM, IADCON, IADRBE, IADBOH, IADTOH */
            regs[addr] = val;
            break;
        case 0x4c: /* ICRT */
        case 0x4d: /* ICD */
            regs[addr] = val & 0x7f;
            break;
        case 0x4e: /* UBCSA */
        case 0x4f: /* UBCSB */
            regs[addr] = val;
            break;

        case 0x54 ... 0x56: /* MAR1-3 */
            regs[addr] = val;
            break;

        /* 57h is reserved on the 82379AB: it has no scatter/gather. */

        case 0x60 ... 0x63: /* PIRQ route control; the PIRQ pins are pulled up */
            regs[addr] = val & 0x8f;
            break;

        case 0x70:
            dev->pacc = val & 0x03;
            break;
        case 0x71:
            dev->apicbase = val & 0x3f;
            sioa_apic_update(dev);
            break;

        case 0x80: /* BIOS timer base: bit 1 reserved */
            regs[addr] = val & 0xfd;
            break;
        case 0x81:
            regs[addr] = val;
            break;

        case 0xa0: /* SMICNTL: bit 6 is the 82379AB's CWSGNTDI */
            regs[addr] = val & 0x4f;
            break;
        case 0xa2: /* SMIEN */
            regs[addr] = val;
            break;
        case 0xa4: /* SEE */
            regs[addr] = val & 0xfb;
            break;
        case 0xa5:
            regs[addr] = val;
            break;
        case 0xa7: /* bits 30 and 27:24 the 82379AB's */
            regs[addr] = val & 0xef;
            break;
        case 0xa8: /* FTMR */
            regs[addr] = val;
            break;
        case 0xaa: /* SMIREQ: the status bits are cleared by writing 0 */
            regs[addr] &= val;
            break;
        case 0xac: /* CTLTMR */
        case 0xae: /* CTLTMRH */
            regs[addr] = val;
            break;

        default:
            break;
    }
}

static uint8_t
sioa_read(int func, int addr, UNUSED(int len), void *priv)
{
    const sioa_t *dev = (sioa_t *) priv;

    if (func > 0)
        return 0xff;

    /* The write-only registers do not read back; the rest of the space
       reads as the data sheet's reserved "read as 0". */
    return dev->regs[addr];
}

static void
sioa_reset(void *priv)
{
    sioa_t *dev = (sioa_t *) priv;

    memset(dev->regs, 0x00, sizeof(dev->regs));

    dev->regs[0x00] = 0x86;
    dev->regs[0x01] = 0x80;
    dev->regs[0x02] = 0x84;
    dev->regs[0x03] = 0x04;
    dev->regs[0x04] = 0x07;
    dev->regs[0x07] = 0x02;
    dev->regs[0x08] = 0x88; /* 82379AB A0 */
    dev->regs[0x40] = 0x20;
    dev->regs[0x42] = 0x04;
    dev->regs[0x45] = 0x10;
    dev->regs[0x46] = 0x0f;
    dev->regs[0x48] = 0x01;
    dev->regs[0x4a] = 0x10;
    dev->regs[0x4b] = 0x0f;
    dev->regs[0x4c] = 0x56;
    dev->regs[0x4d] = 0x40;
    dev->regs[0x4e] = 0x07;
    dev->regs[0x4f] = 0x4f;
    dev->regs[0x60] = 0x80;
    dev->regs[0x61] = 0x80;
    dev->regs[0x62] = 0x80;
    dev->regs[0x63] = 0x80;
    dev->regs[0x80] = 0x78;
    dev->regs[0xa0] = 0x08;
    dev->regs[0xa8] = 0x0f;

    dev->pacc     = 0x00;
    dev->apicbase = 0x00;
    sioa_apic_update(dev);
}

static void
sioa_close(void *priv)
{
    free(priv);
}

static void *
sioa_init(UNUSED(const device_t *info))
{
    sioa_t *dev = (sioa_t *) calloc(1, sizeof(sioa_t));

    memset(dev->line, 0xff, sizeof(dev->line));
    pci_add_card(PCI_ADD_SOUTHBRIDGE_AUX, sioa_read, sioa_write, dev, &dev->slot);
    sioa_reset(dev);

    return dev;
}

const device_t sioa_peer_device = {
    .name          = "Intel 82379AB SIO.A (peer bus arbiter and I/O APIC)",
    .internal_name = "sioa_peer",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = sioa_init,
    .close         = sioa_close,
    .reset         = sioa_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
