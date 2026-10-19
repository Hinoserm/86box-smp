/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Debug console: look at and change the processor's registers and
 *          memory while the machine is paused, with GDB's syntax.
 *
 *          Commands run on the emulation thread, from its paused loop
 *          (debugcon_poll), never on the thread that typed them, so they
 *          see the processor between two blocks exactly as the recompiler
 *          left it. Memory is reached without side effects: a page walk
 *          that neither faults nor sets accessed/dirty bits, and only RAM
 *          or ROM behind the processor's own view of the map -- reading a
 *          device register could change the device. A write marks the
 *          range dirty, so compiled code there is thrown away.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_flags.h"
#include "x86seg.h"
#include "x87_sf.h"
#include "x87.h"
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/pci.h>
#include <86box/eisa.h>
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/pit_fast.h>
#include <86box/dma.h>
#include <86box/debugcon.h>

#define DEBUGCON_OUT_MAX 65536

static mutex_t *debugcon_mutex;
static void (*debugcon_output)(int source, const char *text);

static char   debugcon_buf[DEBUGCON_OUT_MAX];
static size_t debugcon_len;

/* What an x command with no format letters uses: the last one's. */
static char debugcon_x_fmt  = 'x';
static int  debugcon_x_size = 4;

static void
out_v(const char *fmt, va_list ap)
{
    int n;

    if (debugcon_len >= (DEBUGCON_OUT_MAX - 1))
        return;

    n = vsnprintf(debugcon_buf + debugcon_len, DEBUGCON_OUT_MAX - debugcon_len, fmt, ap);

    if (n > 0)
        debugcon_len += n;
    if (debugcon_len >= DEBUGCON_OUT_MAX)
        debugcon_len = DEBUGCON_OUT_MAX - 1;
}

static void
out(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    out_v(fmt, ap);
    va_end(ap);
}

/* The same, for a device's own view (debugcon_register). */
void
debugcon_out(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    out_v(fmt, ap);
    va_end(ap);
}

/* ---- registers ----------------------------------------------------------- */

typedef enum {
    REG_GPR32,
    REG_GPR16,
    REG_GPR8L,
    REG_GPR8H,
    REG_EIP,
    REG_EFLAGS,
    REG_SEG,
    REG_CR
} reg_kind_t;

typedef struct {
    const char *name;
    reg_kind_t  kind;
    int         index;
} reg_t;

// clang-format off
static const reg_t regs[] = {
    { "eax", REG_GPR32, 0 }, { "ecx", REG_GPR32, 1 }, { "edx", REG_GPR32, 2 }, { "ebx", REG_GPR32, 3 },
    { "esp", REG_GPR32, 4 }, { "ebp", REG_GPR32, 5 }, { "esi", REG_GPR32, 6 }, { "edi", REG_GPR32, 7 },
    { "ax", REG_GPR16, 0 },  { "cx", REG_GPR16, 1 },  { "dx", REG_GPR16, 2 },  { "bx", REG_GPR16, 3 },
    { "sp", REG_GPR16, 4 },  { "bp", REG_GPR16, 5 },  { "si", REG_GPR16, 6 },  { "di", REG_GPR16, 7 },
    { "al", REG_GPR8L, 0 },  { "cl", REG_GPR8L, 1 },  { "dl", REG_GPR8L, 2 },  { "bl", REG_GPR8L, 3 },
    { "ah", REG_GPR8H, 0 },  { "ch", REG_GPR8H, 1 },  { "dh", REG_GPR8H, 2 },  { "bh", REG_GPR8H, 3 },
    { "eip", REG_EIP, 0 },   { "pc", REG_EIP, 0 },    { "eflags", REG_EFLAGS, 0 },
    { "cs", REG_SEG, 0 },    { "ss", REG_SEG, 1 },    { "ds", REG_SEG, 2 },
    { "es", REG_SEG, 3 },    { "fs", REG_SEG, 4 },    { "gs", REG_SEG, 5 },
    { "cr0", REG_CR, 0 },    { "cr2", REG_CR, 2 },    { "cr3", REG_CR, 3 },    { "cr4", REG_CR, 4 },
    { NULL, 0, 0 }
};
// clang-format on

static x86seg *const seg_regs[] = {
    &cpu_state.seg_cs, &cpu_state.seg_ss, &cpu_state.seg_ds,
    &cpu_state.seg_es, &cpu_state.seg_fs, &cpu_state.seg_gs
};
static const char *const seg_names[] = { "cs", "ss", "ds", "es", "fs", "gs" };

static const reg_t *
reg_find(const char *name)
{
    for (const reg_t *r = regs; r->name; r++)
        if (!strcasecmp(r->name, name))
            return r;
    return NULL;
}

static uint32_t
reg_get(const reg_t *r)
{
    switch (r->kind) {
        case REG_GPR32:
            return cpu_state.regs[r->index].l;
        case REG_GPR16:
            return cpu_state.regs[r->index].w;
        case REG_GPR8L:
            return cpu_state.regs[r->index].b.l;
        case REG_GPR8H:
            return cpu_state.regs[r->index].b.h;
        case REG_EIP:
            return cpu_state.pc;
        case REG_EFLAGS:
            cpu_386_flags_rebuild();
            return ((uint32_t) cpu_state.eflags << 16) | cpu_state.flags;
        case REG_SEG:
            return seg_regs[r->index]->seg;
        case REG_CR:
            switch (r->index) {
                case 0:
                    return cr0;
                case 2:
                    return cr2;
                case 3:
                    return cr3;
                default:
                    return cr4;
            }
    }
    return 0;
}

/* Real and V86 mode only for segment registers: loading one in protected
   mode reads a descriptor and can fault, which would deliver an exception
   to the guest from the console. */
static int
reg_set(const reg_t *r, uint32_t val)
{
    switch (r->kind) {
        case REG_GPR32:
            cpu_state.regs[r->index].l = val;
            break;
        case REG_GPR16:
            cpu_state.regs[r->index].w = val;
            break;
        case REG_GPR8L:
            cpu_state.regs[r->index].b.l = val;
            break;
        case REG_GPR8H:
            cpu_state.regs[r->index].b.h = val;
            break;
        case REG_EIP:
            cpu_state.pc = val;
            flushmmucache();
            break;
        case REG_EFLAGS:
            cpu_386_flags_rebuild();
            cpu_state.flags    = (val & 0xffff) | 2;
            cpu_state.eflags   = val >> 16;
            cpu_state.flags_op = FLAGS_UNKNOWN;
            break;
        case REG_SEG:
            if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG)) {
                out("Segment registers can only be set in real or V86 mode.\n");
                return 0;
            }
            /* The processor's own loads: in these modes they cannot fault,
               and they keep the flat-segment bits compiled blocks are keyed
               on. */
            if (r->index == 0)
                loadcs(val & 0xffff);
            else
                loadseg(val & 0xffff, seg_regs[r->index]);
            flushmmucache();
            break;
        case REG_CR:
            switch (r->index) {
                case 0:
                    cr0 = val;
                    break;
                case 2:
                    cr2 = val;
                    break;
                case 3:
                    cr3 = val;
                    break;
                default:
                    cr4 = val;
                    break;
            }
            flushmmucache();
            break;
    }

    /* Compiled blocks are keyed on the mode: keep it in step with CR0.PE
       and EFLAGS.VM, as a mode change by the processor does. */
    if ((r->kind == REG_CR) || (r->kind == REG_EFLAGS)) {
        cpu_cur_status &= ~(CPU_STATUS_PMODE | CPU_STATUS_V86);
        if (cr0 & 1) {
            cpu_cur_status |= CPU_STATUS_PMODE;
            if (cpu_state.eflags & VM_FLAG)
                cpu_cur_status |= CPU_STATUS_V86;
        }
    }
    return 1;
}

static void
show_flags(uint32_t f)
{
    static const struct {
        uint32_t    bit;
        const char *name;
    } names[] = {
        // clang-format off
        { 0x000001, "CF" }, { 0x000004, "PF" }, { 0x000010, "AF" }, { 0x000040, "ZF" },
        { 0x000080, "SF" }, { 0x000100, "TF" }, { 0x000200, "IF" }, { 0x000400, "DF" },
        { 0x000800, "OF" }, { 0x004000, "NT" }, { 0x010000, "RF" }, { 0x020000, "VM" },
        { 0x040000, "AC" }, { 0x200000, "ID" }, { 0, NULL }
        // clang-format on
    };

    out("[ ");
    for (int i = 0; names[i].name; i++)
        if (f & names[i].bit)
            out("%s ", names[i].name);
    out("IOPL=%u ]", (f >> 12) & 3);
}

static void
show_registers(void)
{
    static const char *const gpr[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    uint32_t                 f;

    for (int i = 0; i < 8; i++)
        out("%-6s 0x%08x  %u\n", gpr[i], cpu_state.regs[i].l, cpu_state.regs[i].l);

    out("%-6s 0x%08x  (linear 0x%08x)\n", "eip", cpu_state.pc, cs + cpu_state.pc);

    cpu_386_flags_rebuild();
    f = ((uint32_t) cpu_state.eflags << 16) | cpu_state.flags;
    out("%-6s 0x%08x  ", "eflags", f);
    show_flags(f);
    out("\n");

    for (int i = 0; i < 6; i++)
        out("%-6s 0x%04x      base 0x%08x limit 0x%08x access 0x%02x\n", seg_names[i],
            seg_regs[i]->seg, seg_regs[i]->base, seg_regs[i]->limit, seg_regs[i]->access);

    out("%-6s 0x%08x\n%-6s 0x%08x\n%-6s 0x%08x\n%-6s 0x%08x\n", "cr0", cr0, "cr2", cr2, "cr3", cr3, "cr4", cr4);

    out("mode   %s, CPL %u%s\n",
        !(cr0 & 1) ? "real" : ((cpu_state.eflags & VM_FLAG) ? "V86" : "protected"),
        CPL, in_smm ? ", in SMM" : "");
}

/* ---- memory -------------------------------------------------------------- */

/* A linear address to a physical one as this processor would translate it,
   without touching anything: no fault, no accessed or dirty bit, no TLB
   entry. Returns 0 when it does not translate. */
static int
mem_linear_to_phys(uint32_t lin, uint32_t *phys)
{
    uint64_t pa;
    int      old_override;

    if (!(cr0 >> 31)) {
        *phys = lin & rammask;
        return 1;
    }

    if (cpu_state.abrt)
        return 0;

    old_override = cpl_override;
    cpl_override = 1; /* the console sees supervisor and user pages alike */
    pa           = mmutranslate_noabrt(lin, 0);
    cpl_override = old_override;

    if (pa > 0xffffffffULL)
        return 0;

    *phys = ((uint32_t) pa) & rammask;
    return 1;
}

/* Where a physical byte lives in host memory, if it is plain memory in the
   processor's view of the map; NULL for a device, or nothing. */
static uint8_t *
mem_phys_host(uint32_t phys, int write)
{
    mem_mapping_t *map = write ? write_mapping[phys >> MEM_GRANULARITY_BITS] : read_mapping[phys >> MEM_GRANULARITY_BITS];

    uint32_t off;

    if ((map == NULL) || (map->exec == NULL))
        return NULL;

    /* As the physical accessors do; and never past the mapping's end,
       which a mapping seen at an alias of its base would otherwise reach. */
    off = (phys - map->base) & map->mask;
    if (off >= map->size)
        return NULL;

    return map->exec + off;
}

typedef struct {
    uint32_t addr;
    int      phys; /* addr is physical */
} dbg_addr_t;

static int
mem_access(const dbg_addr_t *a, uint32_t off, uint8_t *val, int write)
{
    uint32_t phys;
    uint8_t *p;

    if (a->phys)
        phys = a->addr + off;
    else if (!mem_linear_to_phys(a->addr + off, &phys))
        return 0;

    p = mem_phys_host(phys, write);
    if (p == NULL)
        return 0;

    if (write) {
        *p = *val;
        mem_invalidate_range(phys, phys);
    } else
        *val = *p;

    return 1;
}

/* ---- parsing ------------------------------------------------------------- */

static const char *
skip_ws(const char *s)
{
    while (*s && isspace((unsigned char) *s))
        s++;
    return s;
}

/* A number, a $register, or a sum or difference of them. */
static int
parse_value(const char **ps, uint32_t *val)
{
    const char *s     = skip_ws(*ps);
    uint32_t    total = 0;
    int         sign  = 1;
    int         any   = 0;

    for (;;) {
        uint32_t term;

        s = skip_ws(s);
        if (*s == '$') {
            char         name[16];
            int          n = 0;
            const reg_t *r;

            s++;
            while ((isalnum((unsigned char) *s)) && (n < 15))
                name[n++] = *s++;
            name[n] = '\0';
            if (!(r = reg_find(name))) {
                out("No register named $%s.\n", name);
                return 0;
            }
            term = reg_get(r);
        } else if (isdigit((unsigned char) *s)) {
            char *end;

            term = (uint32_t) strtoul(s, &end, 0);
            s    = end;
        } else
            break;

        total += (sign > 0) ? term : -term;
        any = 1;

        s = skip_ws(s);
        if (*s == '+')
            sign = 1;
        else if (*s == '-')
            sign = -1;
        else
            break;
        s++;
    }

    if (!any)
        return 0;

    *ps  = s;
    *val = total;
    return 1;
}

/* An address: `phys:VALUE' for physical memory, `SEG:VALUE' with a segment
   register name or a real-mode segment number, or a linear VALUE. */
static int
parse_addr(const char **ps, dbg_addr_t *a)
{
    const char *s    = skip_ws(*ps);
    uint32_t    base = 0;
    uint32_t    off;

    a->phys = 0;

    if (!strncasecmp(s, "phys:", 5)) {
        s += 5;
        a->phys = 1;
    } else {
        for (int i = 0; i < 6; i++) {
            if (!strncasecmp(s, seg_names[i], 2) && (s[2] == ':')) {
                base = seg_regs[i]->base;
                s += 3;
                goto offset;
            }
        }

        /* A number followed by a colon is a real-mode segment. */
        {
            const char *t = s;
            uint32_t    seg;

            if (parse_value(&t, &seg) && (*skip_ws(t) == ':')) {
                base = (seg & 0xffff) << 4;
                s    = skip_ws(t) + 1;
            }
        }
    }

offset:
    if (!parse_value(&s, &off)) {
        out("Expected an address.\n");
        return 0;
    }

    a->addr = base + off;
    *ps     = s;
    return 1;
}

/* ---- commands ------------------------------------------------------------ */

static void
cmd_info_registers(const char *arg)
{
    char         word[16] = "";
    int          n        = 0;
    const reg_t *r;

    arg = skip_ws(arg);
    while (*arg && !isspace((unsigned char) *arg) && (n < 15))
        word[n++] = *arg++;
    word[n] = '\0';

    if (!word[0]) {
        show_registers();
        return;
    }

    /* A register name, with or without $: that register. */
    r = reg_find((word[0] == '$') ? word + 1 : word);
    if (!r) {
        out("Invalid register `%s'.\n", word);
        return;
    }
    out("%-6s 0x%08x  %u\n", r->name, reg_get(r), reg_get(r));
}

static void
cmd_print(const char *arg)
{
    char     fmt = 'd';
    uint32_t val;

    /* A format letter must follow the slash: p/ alone has none, and two
       characters on from it is past the end of the line. */
    if (*arg == '/') {
        if (!arg[1] || isspace((unsigned char) arg[1])) {
            out("p/F VALUE: F is x, d, u, o, t or c.\n");
            return;
        }
        fmt = arg[1];
        arg += 2;
    }

    if (!parse_value(&arg, &val)) {
        out("Expected a value, a $register or a sum of them.\n");
        return;
    }

    switch (fmt) {
        case 'x':
            out("0x%x\n", val);
            break;
        case 'o':
            out("0%o\n", val);
            break;
        case 't':
            for (int i = 31; i >= 0; i--)
                out("%c", (val >> i) & 1 ? '1' : '0');
            out("\n");
            break;
        case 'c':
            out("%u '%c'\n", val & 0xff, isprint((int) (val & 0xff)) ? (int) (val & 0xff) : '.');
            break;
        case 'u':
            out("%u\n", val);
            break;
        default:
            out("%d\n", (int32_t) val);
            break;
    }
}

/* x/NFU ADDR, as GDB: N units of size U (b, h, w, g) in format F (x, d, u,
   o, c). Bytes that are not plain memory show as ??. */
static void
cmd_examine(const char *arg)
{
    int        count = 1;
    char       fmt   = debugcon_x_fmt;
    int        size  = debugcon_x_size;
    dbg_addr_t a;
    int        per_line;

    if (*arg == '/') {
        arg++;
        if (isdigit((unsigned char) *arg))
            count = (int) strtol(arg, (char **) &arg, 10);
        while (*arg && !isspace((unsigned char) *arg)) {
            switch (*arg) {
                case 'b':
                    size = 1;
                    break;
                case 'h':
                    size = 2;
                    break;
                case 'w':
                    size = 4;
                    break;
                case 'g':
                    size = 8;
                    break;
                case 'x':
                case 'd':
                case 'u':
                case 'o':
                case 'c':
                    fmt = *arg;
                    break;
                default:
                    out("Unknown format letter `%c'.\n", *arg);
                    return;
            }
            arg++;
        }
    }

    if (fmt == 'c')
        size = 1;
    debugcon_x_fmt  = fmt;
    debugcon_x_size = size;

    if ((count < 1) || (count > 4096)) {
        out("Count must be between 1 and 4096.\n");
        return;
    }

    if (!parse_addr(&arg, &a))
        return;

    per_line = (fmt == 'c') ? 16 : (16 / size);

    for (int i = 0; i < count; i++) {
        uint32_t off = (uint32_t) (i * size);
        uint8_t  b[8];
        int      ok = 1;
        uint64_t v  = 0;

        if ((i % per_line) == 0) {
            if (i)
                out("\n");
            out("%s%08x:", a.phys ? "phys:" : "", a.addr + off);
        }

        for (int j = 0; j < size; j++)
            if (!mem_access(&a, off + j, &b[j], 0))
                ok = 0;

        if (!ok) {
            out(" %.*s", size * 2, "????????????????");
            continue;
        }

        for (int j = size - 1; j >= 0; j--)
            v = (v << 8) | b[j];

        switch (fmt) {
            case 'd':
                if (size == 1)
                    out(" %d", (int8_t) v);
                else if (size == 2)
                    out(" %d", (int16_t) v);
                else if (size == 4)
                    out(" %d", (int32_t) v);
                else
                    out(" %" PRId64, (int64_t) v);
                break;
            case 'u':
                out(" %" PRIu64, v);
                break;
            case 'o':
                out(" 0%" PRIo64, v);
                break;
            case 'c':
                out(" %c", isprint((int) v) ? (int) v : '.');
                break;
            default:
                out(" 0x%0*" PRIx64, size * 2, v);
                break;
        }
    }
    out("\n");
}

static void
cmd_set(const char *arg)
{
    arg = skip_ws(arg);
    if (!strncasecmp(arg, "var ", 4))
        arg = skip_ws(arg + 4);

    /* set $reg = value */
    if (*arg == '$') {
        char         name[16];
        int          n = 0;
        const reg_t *r;
        uint32_t     val;

        arg++;
        while (isalnum((unsigned char) *arg) && (n < 15))
            name[n++] = *arg++;
        name[n] = '\0';

        if (!(r = reg_find(name))) {
            out("No register named $%s.\n", name);
            return;
        }
        arg = skip_ws(arg);
        if (*arg++ != '=') {
            out("Expected `='.\n");
            return;
        }
        if (!parse_value(&arg, &val)) {
            out("Expected a value.\n");
            return;
        }
        if (reg_set(r, val))
            out("%s = 0x%08x\n", r->name, reg_get(r));
        return;
    }

    /* set {TYPE} ADDR = value */
    if (*arg == '{') {
        static const struct {
            const char *name;
            int         size;
        } types[] = {
            // clang-format off
            { "char", 1 }, { "unsigned char", 1 }, { "uint8_t", 1 },
            { "short", 2 }, { "unsigned short", 2 }, { "uint16_t", 2 },
            { "int", 4 }, { "unsigned int", 4 }, { "long", 4 }, { "uint32_t", 4 },
            { "long long", 8 }, { "unsigned long long", 8 }, { "uint64_t", 8 },
            { NULL, 0 }
            // clang-format on
        };
        char        type[32];
        int         n    = 0;
        int         size = 0;
        dbg_addr_t  a;
        uint64_t    val;
        const char *end;
        char       *vend;

        arg++;
        end = strchr(arg, '}');
        if (!end || ((end - arg) >= (int) sizeof(type))) {
            out("Expected {TYPE}.\n");
            return;
        }
        while (arg < end)
            type[n++] = *arg++;
        type[n] = '\0';
        arg     = end + 1;

        for (int i = 0; types[i].name; i++)
            if (!strcasecmp(types[i].name, type))
                size = types[i].size;
        if (!size) {
            out("Unknown type `%s'; use char, short, int or long long.\n", type);
            return;
        }

        if (!parse_addr(&arg, &a))
            return;
        arg = skip_ws(arg);
        if (*arg++ != '=') {
            out("Expected `='.\n");
            return;
        }
        arg = skip_ws(arg);
        val = strtoull(arg, &vend, 0);
        if (vend == arg) {
            out("Expected a value.\n");
            return;
        }

        /* All or nothing: every byte must be writable before any is written. */
        for (int i = 0; i < size; i++) {
            uint32_t phys;

            if (a.phys)
                phys = a.addr + i;
            else if (!mem_linear_to_phys(a.addr + i, &phys)) {
                out("0x%08x does not translate.\n", a.addr + i);
                return;
            }
            if (!mem_phys_host(phys, 1)) {
                out("0x%08x is not RAM.\n", a.addr + i);
                return;
            }
        }
        for (int i = 0; i < size; i++) {
            uint8_t b = (uint8_t) (val >> (i * 8));

            mem_access(&a, i, &b, 1);
        }
        out("Wrote %i byte%s at %s%08x.\n", size, (size == 1) ? "" : "s", a.phys ? "phys:" : "", a.addr);
        return;
    }

    out("Use set $REGISTER = VALUE or set {TYPE} ADDRESS = VALUE.\n");
}

/* The guest's physical RAM, as it is, to a file. */
static void
dump_ram(const char *path)
{
    FILE    *fp;
    uint64_t size = (uint64_t) mem_size * 1024;

    if (ram == NULL) {
        out("There is no RAM to dump.\n");
        return;
    }

    fp = plat_fopen(path, "wb");
    if (fp == NULL) {
        out("Could not open %s for writing.\n", path);
        return;
    }

    /* In pieces, so a size_t narrower than the RAM is never an issue. */
    for (uint64_t done = 0; done < size;) {
        size_t chunk = (size_t) (((size - done) > (1ULL << 24)) ? (1ULL << 24) : (size - done));

        if (fwrite(ram + done, 1, chunk, fp) != chunk) {
            out("Writing %s failed.\n", path);
            fclose(fp);
            return;
        }
        done += chunk;
    }
    fclose(fp);
    out("Dumped %" PRIu64 " KB of RAM to %s.\n", size >> 10, path);
}

/* A file name, quoted if it has spaces. */
static int
parse_path(const char **ps, char *path, size_t size)
{
    const char *s = skip_ws(*ps);
    size_t      n = 0;
    char        end;

    if (!*s)
        return 0;

    end = (*s == '"') ? '"' : ' ';
    if (end == '"')
        s++;
    while (*s && (*s != end) && (n < (size - 1)))
        path[n++] = *s++;
    path[n] = '\0';
    if (*s == '"')
        s++;

    *ps = s;
    return n > 0;
}

static void dump_registers(const char *path);

/* dump binary memory FILE START STOP, as GDB; dump ram FILE, all of it. */
static void
cmd_dump(const char *arg)
{
    char       path[1024];
    dbg_addr_t a;
    dbg_addr_t stop;
    FILE      *fp;
    uint32_t   len;
    uint32_t   bad = 0;

    arg = skip_ws(arg);
    if (!strncasecmp(arg, "registers ", 10)) {
        arg += 10;
        if (!parse_path(&arg, path, sizeof(path))) {
            out("Usage: dump registers FILE\n");
            return;
        }
        dump_registers(path);
        return;
    }
    if (!strncasecmp(arg, "ram ", 4)) {
        arg += 4;
        if (!parse_path(&arg, path, sizeof(path))) {
            out("Usage: dump ram FILE\n");
            return;
        }
        dump_ram(path);
        return;
    }

    if (strncasecmp(arg, "binary memory ", 14)) {
        out("Usage: dump binary memory FILE START STOP, or dump ram FILE\n");
        return;
    }
    arg += 14;

    if (!parse_path(&arg, path, sizeof(path)) || !parse_addr(&arg, &a) || !parse_addr(&arg, &stop))
        return;

    if ((stop.addr <= a.addr) || (stop.phys != a.phys)) {
        out("STOP must be past START, and both the same kind of address.\n");
        return;
    }
    len = stop.addr - a.addr;

    fp = plat_fopen(path, "wb");
    if (fp == NULL) {
        out("Could not open %s for writing.\n", path);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b;

        if (!mem_access(&a, i, &b, 0)) {
            b = 0xff;
            bad++;
        }
        fputc(b, fp);
    }
    fclose(fp);

    out("Dumped %u bytes to %s", len, path);
    if (bad)
        out(" (%u not plain memory, written as FF)", bad);
    out(".\n");
}

/* ---- the machine --------------------------------------------------------- */

static const char *
dev_name(const void *priv)
{
    const device_t *d = device_debug_of_priv(priv);

    return d ? d->name : NULL;
}

/* Sixteen bytes a line, skipping lines that are all FF or all 00 when asked
   (an empty configuration store is mostly that). */
static void
hexdump(const uint8_t *p, uint32_t base, uint32_t n, int skip_empty)
{
    int skipped = 0;

    for (uint32_t i = 0; i < n; i += 16) {
        uint32_t len = ((n - i) < 16) ? (n - i) : 16;

        if (skip_empty) {
            int ff = 1, zero = 1;

            for (uint32_t j = 0; j < len; j++) {
                ff &= (p[i + j] == 0xff);
                zero &= (p[i + j] == 0x00);
            }
            if (ff || zero) {
                skipped++;
                continue;
            }
        }

        out("  %04x:", base + i);
        for (uint32_t j = 0; j < len; j++)
            out("%s%02x", (j == 8) ? "  " : " ", p[i + j]);
        out("  ");
        for (uint32_t j = 0; j < len; j++)
            out("%c", isprint(p[i + j]) ? p[i + j] : '.');
        out("\n");
    }
    if (skipped)
        out("  (%i lines of all FF or all 00 not shown)\n", skipped);
}

static void
cmd_info_devices(void)
{
    const device_t *d;
    void           *priv;

    for (int i = 0; device_debug_get(i, &d, &priv); i++) {
        if (d == NULL)
            continue;
        out("%-40s %-20s%s\n", d->name, d->internal_name ? d->internal_name : "",
            d->debug_dump ? "  [info board]" : "");
    }
}

/* Every device that can show its registers. */
static void
cmd_info_board(void)
{
    const device_t *d;
    void           *priv;
    int             any = 0;

    out("Machine: %s\n", machine_getname(machine));
    for (int i = 0; device_debug_get(i, &d, &priv); i++) {
        if ((d == NULL) || (d->debug_dump == NULL))
            continue;
        out("\n== %s ==\n", d->name);
        d->debug_dump(priv, out);
        any = 1;
    }
    if (!any)
        out("No device on this machine shows its registers yet.\n");
    out("\nPCI functions: info pci. EISA slots and configuration RAM: info eisa.\n");
}

/* Every PCI function's configuration space, read as the guest would. */
static void
cmd_info_pci(void)
{
    uint8_t bus, id;
    void   *priv;
    uint8_t (*read)(int func, int addr, int len, void *priv);
    int any = 0;

    for (int i = 0; i < PCI_CARDS_NUM; i++) {
        if (!pci_debug_card(i, &bus, &id, &priv, &read))
            continue;

        for (int func = 0; func < 8; func++) {
            uint8_t  cfg[256];
            uint16_t vendor;

            vendor = read(func, 0x00, 1, priv) | (read(func, 0x01, 1, priv) << 8);
            if ((vendor == 0xffff) || (vendor == 0x0000)) {
                if (func == 0)
                    break;
                continue;
            }
            for (int r = 0; r < 256; r++)
                cfg[r] = read(func, r, 1, priv);

            out("%s%02x:%02x.%i  %04x:%04x  class %02x%02x%02x rev %02x  %s\n", any ? "\n" : "",
                bus, id, func, vendor, cfg[2] | (cfg[3] << 8), cfg[0x0b], cfg[0x0a], cfg[0x09], cfg[0x08],
                dev_name(priv) ? dev_name(priv) : "");
            hexdump(cfg, 0, 256, 0);
            any = 1;

            /* Single-function devices answer every function number. */
            if ((func == 0) && !(cfg[0x0e] & 0x80))
                break;
        }
    }
    if (!any)
        out("No PCI functions.\n");
}

static void
eisa_id_string(const uint8_t *id, char *buf)
{
    uint16_t packed = (id[0] << 8) | id[1];

    buf[0] = '@' + ((packed >> 10) & 0x1f);
    buf[1] = '@' + ((packed >> 5) & 0x1f);
    buf[2] = '@' + (packed & 0x1f);
    sprintf(buf + 3, "%02X%02X", id[2], id[3]);
}

/* The slots, who is in them, and the configuration RAM the ECU writes. */
static void
cmd_info_eisa(void)
{
    uint8_t        nr = eisa_slots_present();
    const uint8_t *cram;
    uint32_t       cram_size;

    if (!nr) {
        out("This machine has no EISA bus.\n");
        return;
    }

    out("%u slots\n", nr);
    for (uint8_t slot = 0; slot <= nr; slot++) {
        const eisa_slot_t *e = eisa_debug_slot(slot);
        char               id[8];

        if (!e || !e->present) {
            if (slot)
                out("slot %2u  empty\n", slot);
            continue;
        }
        eisa_id_string(e->id, id);
        out("slot %2u  %s  (%02x %02x %02x %02x)  %s\n", slot, id, e->id[0], e->id[1], e->id[2], e->id[3],
            slot ? (dev_name(e->priv) ? dev_name(e->priv) : "") : "system board");
    }

    cram = eisa_get_cram(&cram_size);
    if (cram == NULL) {
        out("\nThe chipset keeps no configuration RAM.\n");
        return;
    }
    out("\nConfiguration RAM, %u bytes (page:offset as 0C00h:0800h shows it):\n", cram_size);
    for (uint32_t page = 0; page < (cram_size >> 8); page++) {
        int empty = 1;

        for (int j = 0; j < 256; j++)
            if ((cram[(page << 8) + j] != 0xff) && (cram[(page << 8) + j] != 0x00))
                empty = 0;
        if (empty)
            continue;
        out(" page %u:\n", page);
        hexdump(cram + (page << 8), 0, 256, 1);
    }
}

static void
describe_state(uint16_t v)
{
    static const char *const bits[5]  = { "internal", "ROMCS", "SMRAM", "cache", "disabled" };
    const char *const        kinds[3] = { "x", "w", "r" };

    for (int k = 0; k < 3; k++) {
        uint16_t f = (v >> (k * 5)) & 0x1f;

        out(" %s=", kinds[k]);
        if (!f)
            out("external");
        for (int b = 0, first = 1; b < 5; b++)
            if (f & (1 << b)) {
                out("%s%s", first ? "" : "+", bits[b]);
                first = 0;
            }
    }
}

static void
describe_mapping(const char *what, const mem_mapping_t *m)
{
    out("  %-14s ", what);
    if (m == NULL) {
        out("nothing\n");
        return;
    }
    out("%08x-%08x%s%s%s  %s\n", m->base, m->base + m->size - 1,
        m->exec ? "  memory" : "  handlers",
        (m->flags & MEM_MAPPING_SMRAM) ? " SMRAM" : "",
        (m->flags & MEM_MAPPING_IS_ROM) ? " ROM" : "",
        dev_name(m->priv) ? dev_name(m->priv) : "");
}

/* With an address: what the processor and the bus masters meet there.
   Without: every mapping, in the order the map is built from. */
static void
cmd_info_mem(const char *arg)
{
    dbg_addr_t a;

    arg = skip_ws(arg);
    if (*arg) {
        uint32_t       phys;
        uint32_t       g;
        mem_mapping_t *rd, *wr;

        if (!parse_addr(&arg, &a))
            return;
        if (a.phys)
            phys = a.addr;
        else if (!mem_linear_to_phys(a.addr, &phys)) {
            out("0x%08x does not translate.\n", a.addr);
            return;
        }
        out("physical 0x%08x (A20 %s)\n", phys, (rammask & 0x100000) ? "on" : "masked");

        g = phys >> MEM_GRANULARITY_BITS;
        out(" processor, %s SMM now:\n", in_smm ? "in" : "outside");
        describe_mapping("read", read_mapping[g]);
        describe_mapping("write", write_mapping[g]);
        out("  %-14s %s\n", "execute", _mem_exec[g] ? "memory" : "not directly");

        mem_debug_bus(phys, &rd, &wr);
        out(" bus masters:\n");
        describe_mapping("read", rd);
        describe_mapping("write", wr);

        out(" chipset access state:\n");
        for (int n = 0; n < 4; n++) {
            static const char *const who[4] = { "processor", "processor SMM", "bus", "bus SMIACT#" };

            out("  %-14s", who[n]);
            describe_state(mem_debug_state(phys, n));
            out("\n");
        }
        return;
    }

    out("base     end      en  kind      flags       device\n");
    for (const mem_mapping_t *m = mem_debug_first_mapping(); m; m = m->next) {
        if (!m->size)
            continue;
        out("%08x %08x %s  %-9s %s%s%s%s  %s\n", m->base, m->base + m->size - 1, m->enable ? "on " : "off",
            m->exec ? "memory" : "handlers",
            (m->flags & MEM_MAPPING_INTERNAL) ? "int " : "",
            (m->flags & MEM_MAPPING_EXTERNAL) ? "ext " : "",
            (m->flags & MEM_MAPPING_SMRAM) ? "smram " : "",
            (m->flags & MEM_MAPPING_IS_ROM) ? "rom " : "",
            dev_name(m->priv) ? dev_name(m->priv) : "");
    }
}

static void
show_pic(const char *name, const pic_t *p)
{
    out("%s: vector base 0x%02x, IRR 0x%02x, ISR 0x%02x, IMR 0x%02x, ELCR 0x%02x, %s-triggered%s%s%s\n",
        name, p->icw2 & 0xf8, p->irr, p->isr, p->imr, p->elcr,
        (p->icw1 & 0x08) ? "level" : "edge",
        (p->icw4 & 0x02) ? ", auto EOI" : "",
        p->special_mask_mode ? ", special mask" : "",
        p->int_pending ? ", interrupt pending" : "");
}

static void
cmd_info_pic(void)
{
    show_pic("master", &pic);
    show_pic("slave ", &pic2);
}

static void
cmd_info_pit(void)
{
    if (pit_devs[0].data == NULL) {
        out("No PIT.\n");
        return;
    }
    /* Both timers: the second is the EISA ESC's, whose counter 0 is the
       fail-safe timer. The fast model's counters are read whole, so the
       state that decides whether one is counting -- gate, running, the
       timer behind it -- shows; the classic one reports count and out. */
    for (int t = 0; t < 2; t++) {
        const pit_intf_t *intf = &pit_devs[t];

        if (intf->data == NULL)
            continue;
        out("timer %i (%s):\n", t + 1, (intf->set_out_func == pit_fast_intf.set_out_func) ? "fast" : "classic");
        for (int i = 0; i < 3; i++) {
            uint16_t count = intf->get_count ? intf->get_count(intf->data, i) : 0;

            out("  counter %i: count 0x%04x (%u)", i, count, count);
            if (intf->set_out_func == pit_fast_intf.set_out_func) {
                const ctrf_t *c = &((const pitf_t *) intf->data)->counters[i];

                out(", mode %u, gate %i, out %i, enabled %i, disabled %i, running %i, using_timer %i, thit %i, timer %s, clock div %i",
                    c->m, c->gate, c->out, c->enabled, c->disabled, c->running, c->using_timer, c->thit,
                    timer_is_enabled((pc_timer_t *) &c->timer) ? "on" : "off", (c->clock_div > 1) ? c->clock_div : 1);
            } else if (intf->get_outlevel)
                out(", out %i", intf->get_outlevel(intf->data, i));
            out("\n");
        }
    }
}

static void
cmd_info_dma(void)
{
    out("ch  mask mode page  base addr   cur addr   base count cur count\n");
    for (int i = 0; i < 8; i++)
        out("%2i  %-4s 0x%02x 0x%02x  0x%08x 0x%08x 0x%06x   0x%06x\n", i, ((dma_m >> i) & 1) ? "yes" : "no",
            dma[i].mode, dma[i].page, dma[i].ab, dma[i].ac, dma[i].cb, (uint32_t) dma[i].cc);
}

static void
cmd_info_float(void)
{
    static const char *const tags[4] = { "valid", "zero", "special", "empty" };
    uint16_t                 tag     = x87_gettag();

    out("CW 0x%04x  SW 0x%04x  TW 0x%04x  TOP %i\n", cpu_state.npxc, cpu_state.npxs, tag, cpu_state.TOP & 7);
    for (int i = 0; i < 8; i++) {
        int r = (cpu_state.TOP + i) & 7;

        out("ST(%i) R%i %-7s ", i, r, tags[(tag >> (r * 2)) & 3]);
        if (fpu_softfloat)
            out("0x%04x %016" PRIx64 "\n", fpu_state.st_space[r].signExp, fpu_state.st_space[r].signif);
        else
            out("%.17g\n", cpu_state.ST[r]);
    }
    for (int i = 0; i < 8; i++)
        out("MM%i 0x%016" PRIx64 "%s", i, cpu_state.MM[i].q, (i & 1) ? "\n" : "   ");
}

/* A descriptor table, read without side effects. */
static void
cmd_info_desc(const char *name, const x86seg *t, int is_idt)
{
    dbg_addr_t a = { t->base, 0 };
    uint32_t   n = (t->limit + 1) / 8;

    if (!(cr0 & 1) && is_idt) {
        out("Real mode: interrupt vector table at 0x%08x\n", t->base);
        for (uint32_t v = 0; (v < 256) && ((v * 4) <= t->limit); v++) {
            uint8_t b[4];
            int     ok = 1;

            for (int j = 0; j < 4; j++)
                ok &= mem_access(&a, (v * 4) + j, &b[j], 0);
            if (ok)
                out("  %02x  %04x:%04x%s", v, b[2] | (b[3] << 8), b[0] | (b[1] << 8), ((v & 3) == 3) ? "\n" : "   ");
        }
        out("\n");
        return;
    }

    out("%s at 0x%08x, limit 0x%04x (%u entries)\n", name, t->base, t->limit, n);
    if (n > 256)
        n = 256;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t  d[8];
        int      ok = 1;
        uint32_t lo, hi;

        for (int j = 0; j < 8; j++)
            ok &= mem_access(&a, (i * 8) + j, &d[j], 0);
        if (!ok) {
            out("  %04x  ??\n", i * 8);
            continue;
        }
        lo = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t) d[3] << 24);
        hi = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t) d[7] << 24);
        if (!(hi & 0x8000) && !lo && !hi)
            continue;

        if (!(hi & 0x1000)) {
            /* System descriptor or gate. */
            uint8_t type = (hi >> 8) & 0x0f;

            if ((type == 0x5) || (type == 0x6) || (type == 0x7) || (type == 0xc) || (type == 0xe) || (type == 0xf))
                out("  %04x  gate type %x  %04x:%08x  DPL %u %s\n", i * 8, type, lo >> 16,
                    (hi & 0xffff0000) | (lo & 0xffff), (hi >> 13) & 3, (hi & 0x8000) ? "P" : "not present");
            else
                out("  %04x  system type %x  base %08x limit %08x  DPL %u %s\n", i * 8, type,
                    (hi & 0xff000000) | ((hi & 0xff) << 16) | (lo >> 16),
                    (hi & 0x800000) ? ((((hi & 0xf0000) | (lo & 0xffff)) << 12) | 0xfff) : ((hi & 0xf0000) | (lo & 0xffff)),
                    (hi >> 13) & 3, (hi & 0x8000) ? "P" : "not present");
        } else
            out("  %04x  %s  base %08x limit %08x  DPL %u %s%s%s\n", i * 8,
                (hi & 0x800) ? "code" : "data",
                (hi & 0xff000000) | ((hi & 0xff) << 16) | (lo >> 16),
                (hi & 0x800000) ? ((((hi & 0xf0000) | (lo & 0xffff)) << 12) | 0xfff) : ((hi & 0xf0000) | (lo & 0xffff)),
                (hi >> 13) & 3, (hi & 0x400000) ? "32-bit " : "16-bit ",
                (hi & 0x800) ? ((hi & 0x200) ? "readable " : "") : ((hi & 0x200) ? "writable " : "read-only "),
                (hi & 0x8000) ? "P" : "not present");
    }
}

/* ---- devices' own views ------------------------------------------------- */

/* A device's own view for info NAME: what it registered with, while it
   is there. */
#define DEBUGCON_DEVICES_MAX 16
static struct {
    char        name[16];
    const char *help;
    void (*info)(const char *arg, void *priv);
    void *priv;
} debugcon_devices[DEBUGCON_DEVICES_MAX];

void
debugcon_register(const char *name, const char *help, void (*info)(const char *arg, void *priv), void *priv)
{
    for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
        if (debugcon_devices[i].info == NULL) {
            snprintf(debugcon_devices[i].name, sizeof(debugcon_devices[i].name), "%s", name);
            debugcon_devices[i].help = help;
            debugcon_devices[i].info = info;
            debugcon_devices[i].priv = priv;
            return;
        }
    }
}

void
debugcon_unregister(void *priv)
{
    for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
        if (debugcon_devices[i].priv == priv)
            memset(&debugcon_devices[i], 0, sizeof(debugcon_devices[i]));
    }
}

/* info NAME [N] ...: the device registered as NAME, its Nth if there are
   several (from 0; the first if no number). Answers whether one had the
   name. */
static int
debugcon_device_info(const char *word, const char *arg)
{
    int want   = -1;
    int seen   = 0;
    int number = 0;

    for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
        if ((debugcon_devices[i].info != NULL) && !strcmp(debugcon_devices[i].name, word))
            number++;
    }
    if (!number)
        return 0;

    arg = skip_ws(arg);
    if (isdigit((unsigned char) *arg)) {
        want = atoi(arg);
        while (isdigit((unsigned char) *arg))
            arg++;
        arg = skip_ws(arg);
    } else if (number == 1)
        want = 0;

    /* Several and no number: each one's line of who it is, and how to
       pick. */
    if (want < 0) {
        for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
            if ((debugcon_devices[i].info != NULL) && !strcmp(debugcon_devices[i].name, word)) {
                out("%s %i: ", word, seen++);
                debugcon_devices[i].info("brief", debugcon_devices[i].priv);
            }
        }
        out("info %s N ... for one of them.\n", word);
        return 1;
    }
    for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
        if ((debugcon_devices[i].info != NULL) && !strcmp(debugcon_devices[i].name, word)) {
            if (seen++ == want) {
                debugcon_devices[i].info(arg, debugcon_devices[i].priv);
                return 1;
            }
        }
    }
    out("There are %i %s, numbered from 0.\n", number, word);
    return 1;
}

/* The views there are, each name's help line once. */
static void
debugcon_device_help(void)
{
    for (int i = 0; i < DEBUGCON_DEVICES_MAX; i++) {
        int dup = 0;

        if (debugcon_devices[i].info == NULL)
            continue;
        for (int j = 0; j < i; j++)
            if ((debugcon_devices[j].info != NULL) && !strcmp(debugcon_devices[j].name, debugcon_devices[i].name))
                dup = 1;
        if (!dup)
            out("%s\n", debugcon_devices[i].help);
    }
}

static void
cmd_info(const char *arg)
{
    char word[16];
    int  n = 0;

    arg = skip_ws(arg);
    while (*arg && !isspace((unsigned char) *arg) && (n < 15))
        word[n++] = *arg++;
    word[n] = '\0';

#define IS(full, min) ((n >= (min)) && !strncmp(word, full, n))
    if (IS("registers", 1))
        cmd_info_registers(arg);
    else if (IS("pci", 2))
        cmd_info_pci();
    else if (IS("board", 1) || IS("chipset", 2))
        cmd_info_board();
    else if (IS("eisa", 1))
        cmd_info_eisa();
    else if (IS("devices", 2))
        cmd_info_devices();
    else if (IS("mem", 1))
        cmd_info_mem(arg);
    else if (IS("pic", 3))
        cmd_info_pic();
    else if (IS("pit", 3))
        cmd_info_pit();
    else if (IS("dma", 2))
        cmd_info_dma();
    else if (IS("float", 1) || IS("fpu", 2))
        cmd_info_float();
    else if (IS("gdt", 1))
        cmd_info_desc("GDT", &gdt, 0);
    else if (IS("idt", 1))
        cmd_info_desc("IDT", &idt, 1);
    else if (!debugcon_device_info(word, arg)) {
        out("info what? registers, float, gdt, idt, mem, pic, pit, dma,\n"
            "pci, eisa, board, devices.\n");
        debugcon_device_help();
    }
#undef IS
}

/* The registers, into a text file. */
static void
dump_registers(const char *path)
{
    size_t start = debugcon_len;
    FILE  *fp;

    cmd_info_registers("");
    fp = plat_fopen(path, "w");
    if (fp == NULL) {
        debugcon_len               = start;
        debugcon_buf[debugcon_len] = '\0';
        out("Could not open %s for writing.\n", path);
        return;
    }
    fwrite(debugcon_buf + start, 1, debugcon_len - start, fp);
    fclose(fp);

    debugcon_len               = start;
    debugcon_buf[debugcon_len] = '\0';
    out("Wrote the registers to %s.\n", path);
}

// clang-format off
static const char debugcon_help[] =
    "pause                      stop the machine\n"
    "continue | unpause | run   let it run again (c for short)\n"
    "hardreset                  what the Action menu's Hard Reset does\n"
    "info registers [REG]       the processor's registers, or one\n"
    "info float                 x87 stack, control/status/tag words, MMX\n"
    "info gdt | idt             descriptor tables (the IVT in real mode)\n"
    "info mem [ADDRESS]         the memory map, or what is at one address\n"
    "info pic | pit | dma       8259s, 8254 counters, 8237 channels\n"
    "info pci                   every PCI function's configuration space\n"
    "info eisa                  EISA slots and the configuration RAM\n"
    "info board                 chipset and super I/O registers\n"
    "info devices               every device on the machine\n"
    "info NAME [N] [WHAT]       a device's own view, the Nth of several; info alone\n"
    "                           lists the views this machine has\n"
    "p[/x|d|u|o|t|c] VALUE      print a value (numbers, $registers, + and -)\n"
    "x/NFU ADDRESS              examine memory: N units, F = x d u o c,\n"
    "                           U = b (byte) h (2) w (4) g (8)\n"
    "set $REG = VALUE           change a register\n"
    "set {TYPE} ADDRESS = VALUE write memory; TYPE char, short, int, long long\n"
    "dump binary memory FILE START STOP\n"
    "dump ram FILE              all of physical RAM\n"
    "dump registers FILE        the registers, as text\n"
    "\n"
    "ADDRESS is linear (through paging), SEG:OFFSET with a segment register\n"
    "or a real-mode segment number, or phys:ADDRESS for physical memory.\n"
    "Only RAM and ROM are read or written; device memory shows as ??.\n";
// clang-format on

/* Needs nothing from the machine, so the window answers it without
   stopping anything. */
const char *
debugcon_help_text(void)
{
    return debugcon_help;
}

static void
cmd_help(void)
{
    out("%s", debugcon_help);
}

static void
debugcon_run(const char *line)
{
    const char *s = skip_ws(line);

    if (!*s)
        return;

    if (!strncmp(s, "info ", 5) || !strncmp(s, "i ", 2) || !strcmp(s, "info") || !strcmp(s, "i")) {
        cmd_info(strchr(s, ' ') ? strchr(s, ' ') : "");
    } else if (!strncmp(s, "print", 5) || (s[0] == 'p' && (s[1] == '/' || isspace((unsigned char) s[1])))) {
        s += (s[1] == 'r') ? 5 : 1;
        cmd_print(skip_ws(s));
    } else if ((s[0] == 'x') && ((s[1] == '/') || isspace((unsigned char) s[1]))) {
        cmd_examine(s + 1);
    } else if (!strncmp(s, "set ", 4)) {
        cmd_set(s + 4);
    } else if (!strncmp(s, "dump ", 5)) {
        cmd_dump(s + 5);
    } else if (!strcmp(s, "help") || !strcmp(s, "h") || !strcmp(s, "?")) {
        cmd_help();
    } else
        out("Unknown command. Type `help'.\n");
}

/* ---- the queue between the consoles and the emulation thread ------------ */

/* Commands wait here for the paused loop, each with the console it came
   from (0 the window, others a network client), so its output goes back
   there. */
#define DEBUGCON_QUEUE 32

typedef struct {
    int   source;
    char *text;
} debugcon_req_t;

static debugcon_req_t debugcon_queue[DEBUGCON_QUEUE];
static int            debugcon_head;
static int            debugcon_count;

void
debugcon_init(void (*output)(int source, const char *text))
{
    if (debugcon_mutex == NULL)
        debugcon_mutex = thread_create_mutex();
    debugcon_output = output;
}

int
debugcon_submit(int source, const char *text)
{
    int queued = 0;

    if (debugcon_mutex == NULL)
        return 0;

    thread_wait_mutex(debugcon_mutex);
    if (debugcon_count < DEBUGCON_QUEUE) {
        debugcon_req_t *r = &debugcon_queue[(debugcon_head + debugcon_count) % DEBUGCON_QUEUE];

        r->source = source;
        r->text   = strdup(text);
        debugcon_count++;
        queued = 1;
    }
    thread_release_mutex(debugcon_mutex);

    return queued;
}

/* From the emulation thread's paused loop: the processor is between two
   blocks, and stays there until this returns. */
void
debugcon_poll(void)
{
    if (debugcon_mutex == NULL)
        return;

    for (;;) {
        debugcon_req_t r;

        thread_wait_mutex(debugcon_mutex);
        if (!debugcon_count) {
            thread_release_mutex(debugcon_mutex);
            return;
        }
        r             = debugcon_queue[debugcon_head];
        debugcon_head = (debugcon_head + 1) % DEBUGCON_QUEUE;
        debugcon_count--;
        thread_release_mutex(debugcon_mutex);

        /* One command per line, in order, each with its own output. */
        for (char *cmd = r.text, *next; cmd != NULL; cmd = next) {
            next = strchr(cmd, '\n');
            if (next != NULL)
                *next++ = '\0';

            debugcon_len    = 0;
            debugcon_buf[0] = '\0';
            out("(86box) %s\n", cmd);
            debugcon_run(cmd);

            if (debugcon_output)
                debugcon_output(r.source, debugcon_buf);
        }
        free(r.text);
    }
}
