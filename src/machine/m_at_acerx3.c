/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The Acer X3 system board (AcerAltos 19000Pro4): four Pentium
 *          Pro sockets on an Intel 450GX with both PCI bridges, EISA
 *          through the 82375SB PCEB and 82374SB ESC.
 *
 *          What it is made of, from Acer's X3 System Service Guide
 *          (SG255-9801A) and its schematics, with the BIOS read where the
 *          schematics are missing:
 *
 *          - Host bus: the 82454GX compatibility PB (device 19h, "OPB 1")
 *            to the primary PCI bus, the auxiliary PB (1Ah, "OPB 2") to the
 *            secondary one, the 82453GX DC (14h).
 *          - Primary bus: the PCEB (device 0), slots 1-4 (devices 4-7), the
 *            on-board ATI 264VT2 (device 0Ch) with no interrupt connected.
 *            The slots' INTA#-INTD# go to the ESC's PIRQ0#-3# (links 1-4).
 *          - Secondary bus: an 82379AB SIO.A (device 0) as its arbiter and
 *            an I/O APIC, its ISA side unconnected; slots 5-7 (devices
 *            4-6); two single-channel AIC-7880s (devices 0Bh and 0Ch).
 *            Their interrupts go through a programmable part the guide
 *            calls the IRQ mapper, which drives the ISA IRQ lines open
 *            drain, so they reach the ESC's 8259 and I/O APIC as ISA
 *            interrupts. Five links (5-9), set through ports 4A9h, 4ABh and
 *            4AFh (the BIOS's routing code, F000:1090).
 *          - An SMC FDC37C935 (floppy, IDE, serial, parallel, keyboard
 *            controller and RTC), the 256 Kbyte SST 29EE020 flash with the
 *            system, SCSI and video BIOSes, 8 Kbytes of EISA configuration
 *            RAM on the ESC, and two LM78s at 4ADh and 4B5h.
 *
 *          The BIOS is Acer's own. It builds an MP table at POST (floating
 *          pointer at F000:D800) listing one I/O APIC, the ESC's at
 *          FEC00000h, with every ISA IRQ on the INTIN of the same number
 *          and no PCI interrupt entries: PCI devices are used through their
 *          ISA IRQs.
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
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/flash.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/sio.h>
#include <86box/hdc.h>
#include <86box/chipset.h>
#include <86box/ioapic_routes.h>
#include <86box/eisa.h>
#include <86box/esc.h>
#include <86box/hwm.h>
#include <86box/i2c.h>
#include <86box/nvr.h>
#include <86box/scsi_aic7xxx.h>
#include <86box/video.h>
#include <86box/machine.h>
#include <86box/cpu_mp.h>
#include "cpu.h"
#include <86box/rom.h>
#include <86box/plat_unused.h>

/* ---- the board's own ports: onboard peripheral control and the IRQ mapper

   4A0h is read and written back by the BIOS a bit at a time (bits 0 and 1
   masked off on every access; bits 2-6 are "onboard peripheral control",
   Service Guide 1-26). Bit 5 holds the flash's A17 low, bringing its lower
   128 Kbytes -- the loader, the packed video BIOS at 08000h, the SCSI BIOS
   at 10000h -- into E0000h-FFFFFh in place of the system BIOS. The BIOS
   sets it around unpacking the video BIOS from E8000h into C0000h, running
   from a copy of itself in RAM meanwhile (E000:30AD-30D6, through
   E000:4C79 and 4C6F), so it can be nothing else. What the other bits
   enable is not documented; they are kept as written.

   The mapper's registers are nibbles, one per secondary-bus link: 4ABh
   bits 7:4 link 5, 3:0 link 6; 4AFh bits 7:4 link 7, 3:0 link 8; 4A9h bits
   3:0 link 9. Bit 3 enables the link, bits 2:0 pick one of the eight lines
   it can drive: IRQ 3, 4, 9, 10, 11, 12, 14, 15 (the BIOS's table at
   F000:0FE4; the ECU file offers the same eight). The part's equations are
   not published: it is taken to read back what was written, and to start
   with every link off. */

/* The RDM module (Remote Diagnostic Management): a daughter board with a
   processor of its own on the X-bus, through connectors CN13/CN14
   (Service Guide sheet 51), that the BIOS reports each POST code to when
   Setup has it enabled -- which Acer's defaults do (CMOS 41h bit 1). Its
   firmware and host protocol are not published; this is the interface as
   the BIOS uses it (E000:5451, F000:7700):
   - 4B1h read, status: bits 0 and 3 set while it can take a byte; bit 2
     set while it holds a byte for the host at 4B7h.
   - 4B1h write, strobes: 05h, 01h, 05h after the host puts a byte at 4B7h
     (bit 2 pulsed low, the write strobe); 05h, 04h, 05h after it takes one
     (bit 0 pulsed low, the read strobe).
   - A message ends in 00h (or is the single command AFh) and is answered
     with 06h, which the BIOS waits for without a time limit. */
typedef struct acerx3_rdm_t {
    uint8_t ctl;      /* the strobe lines as last written */
    uint8_t in;       /* the byte the host last put at 4B7h */
    uint8_t out;      /* the byte waiting for the host */
    uint8_t out_full; /* one is waiting */
} acerx3_rdm_t;

/* Port 4A5h, ASM (Service Guide 1-26): bits 7 and 6 are the SDA and SCL of
   an I2C bus the BIOS drives by hand (its SMM code, the routines round
   A000:8C3B as it runs there), with the 24C08 EEPROM of the server
   management sheet (U1) on it at 50h-53h -- the BIOS reads it through A0h/A1h
   and A6h/A7h. The part starts blank, as a new one is, and keeps what is
   written in the machine's NVR folder. What the other bits of 4A5h do is
   not documented; they are kept as written. */
#define ACERX3_EEPROM_SIZE 1024

typedef struct acerx3_t {
    void        *flash;
    uint8_t      asm_ctl;  /* 4A5h */
    void        *i2c;
    void        *eeprom;
    uint8_t      eeprom_data[ACERX3_EEPROM_SIZE];
    uint8_t      ctl;     /* 4A0h */
    uint8_t      map[3];  /* 4A9h, 4ABh, 4AFh */
    acerx3_rdm_t rdm;
} acerx3_t;

static const uint8_t acerx3_map_irq[8] = { 3, 4, 9, 10, 11, 12, 14, 15 };

static int
acerx3_map_index(uint16_t port)
{
    switch (port) {
        case 0x04a9:
            return 0;
        case 0x04ab:
            return 1;
        case 0x04af:
            return 2;
        default:
            return -1;
    }
}

/* The five links' IRQs from the mapper's registers. */
static void
acerx3_map_apply(const acerx3_t *dev)
{
    const uint8_t nib[5] = { dev->map[1] >> 4, dev->map[1] & 0x0f, dev->map[2] >> 4, dev->map[2] & 0x0f,
                             dev->map[0] & 0x0f };

    for (int i = 0; i < 5; i++)
        pci_set_irq_routing(5 + i, (nib[i] & 0x08) ? acerx3_map_irq[nib[i] & 0x07] : PCI_IRQ_DISABLED);
}

static uint8_t
acerx3_rdm_read(uint16_t port, void *priv)
{
    const acerx3_rdm_t *rdm = &((acerx3_t *) priv)->rdm;

    if (port == 0x04b1)
        return 0x09 | (rdm->out_full ? 0x04 : 0x00);
    return rdm->out;
}

static void
acerx3_rdm_write(uint16_t port, uint8_t val, void *priv)
{
    acerx3_rdm_t *rdm = &((acerx3_t *) priv)->rdm;

    if (port == 0x04b7) {
        rdm->in = val;
        return;
    }
    /* The strobes act on their falling edge. */
    if ((rdm->ctl & 0x04) && !(val & 0x04) && ((rdm->in == 0x00) || (rdm->in == 0xaf))) {
        rdm->out      = 0x06;
        rdm->out_full = 1;
    }
    if ((rdm->ctl & 0x01) && !(val & 0x01))
        rdm->out_full = 0;
    rdm->ctl = val;
}

static uint8_t
acerx3_asm_read(UNUSED(uint16_t port), void *priv)
{
    const acerx3_t *dev = (acerx3_t *) priv;

    return (dev->asm_ctl & 0x3f) | (i2c_gpio_get_sda(dev->i2c) ? 0x80 : 0x00) | (i2c_gpio_get_scl(dev->i2c) ? 0x40 : 0x00);
}

static void
acerx3_asm_write(UNUSED(uint16_t port), uint8_t val, void *priv)
{
    acerx3_t *dev = (acerx3_t *) priv;

    dev->asm_ctl = val;
    i2c_gpio_set(dev->i2c, !!(val & 0x40), !!(val & 0x80));
}

static void
acerx3_write(uint16_t port, uint8_t val, void *priv)
{
    acerx3_t *dev = (acerx3_t *) priv;
    int       i;

    if (port == 0x04a0) {
        dev->ctl = val;
        sst_flash_set_a17_low(dev->flash, !!(val & 0x20));
        return;
    }
    if ((i = acerx3_map_index(port)) < 0)
        return;
    /* 4A9h's upper nibble holds no link. */
    dev->map[i] = (i == 0) ? (val & 0x0f) : val;
    acerx3_map_apply(dev);
}

static uint8_t
acerx3_read(uint16_t port, void *priv)
{
    const acerx3_t *dev = (acerx3_t *) priv;
    int             i;

    if (port == 0x04a0)
        return dev->ctl;
    if ((i = acerx3_map_index(port)) < 0)
        return 0xff;
    return dev->map[i];
}

static void
acerx3_reset(void *priv)
{
    acerx3_t *dev = (acerx3_t *) priv;

    dev->ctl = 0x00;
    sst_flash_set_a17_low(dev->flash, 0);
    dev->asm_ctl = 0xc0;
    i2c_gpio_set(dev->i2c, 1, 1);
    memset(dev->map, 0x00, sizeof(dev->map));
    memset(&dev->rdm, 0x00, sizeof(dev->rdm));
    dev->rdm.ctl = 0x05;
    acerx3_map_apply(dev);
}

static void
acerx3_close(void *priv)
{
    acerx3_t *dev = (acerx3_t *) priv;
    FILE     *fp  = nvr_fopen("acerx3_24c08.nvr", "wb");

    if (fp != NULL) {
        fwrite(dev->eeprom_data, 1, ACERX3_EEPROM_SIZE, fp);
        fclose(fp);
    }
    i2c_eeprom_close(dev->eeprom);
    i2c_gpio_close(dev->i2c);
    free(dev);
}

static void *
acerx3_init(UNUSED(const device_t *info))
{
    acerx3_t *dev = (acerx3_t *) calloc(1, sizeof(acerx3_t));
    FILE     *fp;

    memset(dev->eeprom_data, 0xff, ACERX3_EEPROM_SIZE);
    if ((fp = nvr_fopen("acerx3_24c08.nvr", "rb")) != NULL) {
        if (fread(dev->eeprom_data, 1, ACERX3_EEPROM_SIZE, fp) != ACERX3_EEPROM_SIZE)
            memset(dev->eeprom_data, 0xff, ACERX3_EEPROM_SIZE);
        fclose(fp);
    }
    dev->i2c    = i2c_gpio_init("i2c_acerx3_asm");
    dev->eeprom = i2c_eeprom_init(i2c_gpio_get_bus(dev->i2c), 0x50, dev->eeprom_data, ACERX3_EEPROM_SIZE, 1);

    io_sethandler(0x04a0, 0x0001, acerx3_read, NULL, NULL, acerx3_write, NULL, NULL, dev);
    io_sethandler(0x04a5, 0x0001, acerx3_asm_read, NULL, NULL, acerx3_asm_write, NULL, NULL, dev);
    io_sethandler(0x04a9, 0x0001, acerx3_read, NULL, NULL, acerx3_write, NULL, NULL, dev);
    io_sethandler(0x04ab, 0x0001, acerx3_read, NULL, NULL, acerx3_write, NULL, NULL, dev);
    io_sethandler(0x04af, 0x0001, acerx3_read, NULL, NULL, acerx3_write, NULL, NULL, dev);
    io_sethandler(0x04b1, 0x0001, acerx3_rdm_read, NULL, NULL, acerx3_rdm_write, NULL, NULL, dev);
    io_sethandler(0x04b7, 0x0001, acerx3_rdm_read, NULL, NULL, acerx3_rdm_write, NULL, NULL, dev);

    /* The mapper drives each IRQ line open drain and low while its link
       is asserted: level, as the ESC's PIRQs are. */
    for (int i = 5; i <= 9; i++)
        pci_set_irq_level(i, 1);

    acerx3_reset(dev);

    return dev;
}

static const device_t acerx3_board_device = {
    .name          = "Acer X3 board logic",
    .internal_name = "acerx3_board",
    .flags         = 0,
    .local         = 0,
    .init          = acerx3_init,
    .close         = acerx3_close,
    .reset         = acerx3_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* ---- the machine ------------------------------------------------------ */

/* Which APICs the processors reach: each 82454GX forwards its APICR range
   to its own bus, the compatibility PB to the ESC's I/O APIC, the
   auxiliary PB to the SIO.A's. */
static struct {
    void *gx;
    void *esc_ioapic;
    void *sioa;
} acerx3_apic;

static void
acerx3_apicr_changed(UNUSED(void *arg))
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    int      on = i450gx_apic_range(acerx3_apic.gx, 0, &lo, &hi);

    ioapic_set_host_decode(acerx3_apic.esc_ioapic, lo, hi, on);
    sioa_apic_update(acerx3_apic.sioa);
}

static const device_config_t acerx3_config[] = {
    // clang-format off
    /* The three releases of Acer's BIOS V3.0 for this board. Each is the
       whole 256 Kbyte flash: the system BIOS, the AIC-7880s' and the
       264VT2's. */
    {
        .name           = "bios",
        .description    = "BIOS Version",
        .type           = CONFIG_BIOS,
        .default_string = "acerx3_b3",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = {
            {
                .name          = "Acer BIOS V3.0 R01-A3 (10/17/97)",
                .internal_name = "acerx3_a3",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 262144,
                .files         = { "roms/machines/acerx3/x3_r03a3_rdm5.bin", "" }
            },
            {
                .name          = "Acer BIOS V3.0 R01-B1 (03/26/98)",
                .internal_name = "acerx3_b1",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 262144,
                .files         = { "roms/machines/acerx3/x3r1b1.bin", "" }
            },
            {
                .name          = "Acer BIOS V3.0 R01-B3 (07/07/98)",
                .internal_name = "acerx3_b3",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 262144,
                .files         = { "roms/machines/acerx3/X3_r1b3.bin", "" }
            },
            { .files_no = 0 }
        },
    },
    /* As on the other EISA boards: whether a board never configured starts
       with an empty but well formed EISA store or with nothing. */
    {
        .name           = "auto_eisa_config",
        .description    = "Initialize EISA configuration store",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t acerx3_device = {
    .name          = "Acer X3",
    .internal_name = "acerx3_device",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = acerx3_config
};

static uint32_t
acerx3_scsi_buses(UNUSED(const device_t *info))
{
    return 2;
}

static void *
acerx3_scsi_init(UNUSED(const device_t *info))
{
    void *first = device_add_inst(&aic7880_pci_device, 1);

    device_add_inst(&aic7880_pci_device, 2);
    return first;
}

/* The board has two single-channel AIC-7880s on its secondary bus, their
   BIOS in the system's flash: one device that adds both, so the Settings
   know the board's two SCSI buses (scsi_buses). */
const device_t acerx3_scsi_device = {
    .name          = "Acer X3 onboard SCSI (two AIC-7880 controllers)",
    .internal_name = "acerx3_scsi",
    .flags         = DEVICE_PCI | DEVICE_ONBOARD,
    .init          = acerx3_scsi_init,
    .short_name    = "AIC-7880",
    .scsi_buses    = acerx3_scsi_buses
};

int
machine_at_acerx3_init(const machine_t *model)
{
    int         ret;
    const char *fn;
    void       *gx;
    void       *sioa;
    acerx3_t   *board;
    uint8_t     bus2;

    if (!device_available(model->device))
        return 0;

    device_context(model->device);
    fn  = device_get_bios_file(machine_get_device(machine), device_get_config_bios("bios"), 0);
    ret = bios_load_linear(fn, 0x000c0000, 262144, 0);
    device_context_restore();

    if (bios_only || !ret)
        return ret;

    machine_at_common_init(model);

    machine_pci_init();
    /* The host bus and the primary bus (the machine table). */
    machine_pci_register_slots();

    gx   = device_add(&i450gx_dual_device);
    bus2 = i450gx_aux_bus_index(gx);

    /* The secondary bus, behind the auxiliary PB, now that it has its
       number. */
    machine_pci_register_bus_slots(PCI_CARD_NORTHBRIDGE_AUX, bus2);

    /* Three EISA slots. */
    eisa_init(3);

    device_add(&pceb_device);
    device_add_params(&esc_device, (void *) ESC_CRAM_ACER);
    sioa  = device_add(&sioa_peer_device);
    board = device_add(&acerx3_board_device);

    /* The FDC37C935 carries the keyboard controller, the RTC and the one
       IDE channel (CN15), which it places and enables itself. */
    device_add(&ide_isa_device);
    device_add_params(&fdc37c93x_device, (void *) (FDC37C93X_NORMAL | FDC37C93X_IDE));

    board->flash = device_add(&sst_flash_29ee020_device);

    /* The two LM78s, on the ISA bus at 4ADh/4AEh and 4B5h/4B6h. */
    device_add_inst_params(&lm78_isa_device, 1, (void *) (uintptr_t) 0x04a8);
    device_add_inst_params(&lm78_isa_device, 2, (void *) (uintptr_t) 0x04b0);

    /* Soldered to the board, their BIOS in the system's flash. */
    device_add(machine_get_scsi_device(machine));

    if (gfxcard[0] == VID_INTERNAL)
        device_add(machine_get_vid_device(machine));

    /* The ESC's I/O APIC, and a local APIC in each processor. The BIOS
       starts every processor it finds with INIT and STARTUP and lists them
       in its MP table. Four sockets; a processor that does not pair (no
       on-chip APIC, an OverDrive) runs alone. */
    {
        static const ioapic_legacy_irq_routing_t esc_irq_routing = {
            .irq_to_intin = IOAPIC_LEGACY_IRQ_ROUTE_ESC
        };
        uintptr_t topo = IOAPIC_TOPOLOGY_UP_APIC_DEFAULT;
        void     *ioapic;

        if ((cpu_count >= 2) && cpu_mp_pairable(cpu_f, cpu_s)) {
            cpu_mp_configure(cpu_count);
            topo |= IOAPIC_TOPOLOGY_LAPIC_SLOTS(cpu_count) | IOAPIC_TOPOLOGY_SMP;
        } else {
            if (cpu_count >= 2)
                pclog("Acer X3: %i processors configured, one runs: this processor does not pair\n", cpu_count);
            topo |= IOAPIC_TOPOLOGY_LAPIC_SLOTS(1);
        }

        ioapic = device_add_params(&ioapic_esc_device, (void *) topo);
        ioapic_set_legacy_irq_routing(ioapic, &esc_irq_routing);
        esc_set_ioapic(ioapic);

        /* The SIO.A's I/O APIC, on the secondary bus's interrupts; the
           BIOS neither programs it nor lists it, and until the auxiliary
           PB forwards its address it cannot be reached. */
        acerx3_apic.gx         = gx;
        acerx3_apic.esc_ioapic = ioapic;
        acerx3_apic.sioa       = sioa;
        sioa_attach(sioa, device_add(&ioapic_secondary_device), gx, bus2);
        i450gx_set_apicr_callback(gx, acerx3_apicr_changed, NULL);
    }

    return ret;
}
