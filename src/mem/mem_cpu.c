/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Memory as one processor sees it: its TLB, its page walks, its
 *          reads and writes through the map. Everything here is that
 *          processor's own; the map, RAM and the page table it reaches
 *          are the machine's (mem.c).
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2008-2020 Sarah Walker.
 *          Copyright 2016-2020 Miran Grca.
 *          Copyright 2017-2020 Fred N. van Kempen.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/version.h>
#include "cpu.h"
#include "x86_ops.h"
#include "x86.h"
#include "x86seg_common.h"
#include <86box/machine.h>
#include <86box/m_xt_xi8088.h>
#include <86box/config.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/gdbstub.h>
#include "x86_idle.h"
#ifdef USE_DYNAREC
#    include "codegen_public.h"
#else
#    define PAGE_MASK_SHIFT 6
#    define PAGE_MASK_MASK 63
#endif
#if (!defined(USE_DYNAREC))
#    define BLOCK_PC_INVALID 0xffffffff
#    define BLOCK_INVALID    0
#endif

/* IN530 INIT reset path. */
static int mem_a20_reset_vector_bypass = 0;

void
mem_a20_reset_vector_bypass_once(void)
{
    mem_a20_reset_vector_bypass = 1;
}

uint32_t pccache;
uint8_t *pccache2;
int      cpu_fetch_device;

int        readlnext[2];
uintptr_t  old_rl2;
int        writelnext;

/* The software TLB. Every name below points into the set in place; a
   machine with more than one processor gives each its own set (a
   processor's TLB is its own) and selects it with mem_tlb_select(). The
   first set's storage is static, so a one-processor machine allocates
   nothing and addresses exactly what it always did. */
static page_t   *page_lookup_0[1048576];
static uintptr_t readlookup2_0[2097152];
static uintptr_t writelookup2_0[1048576];

static mem_tlb_t mem_tlb_0 = {
    .page_lookup  = page_lookup_0,
    .readlookup2  = readlookup2_0,
    .writelookup2 = writelookup2_0
};

int       *readlookup   = mem_tlb_0.readlookup;
int       *writelookup  = mem_tlb_0.writelookup;
page_t   **page_lookup  = page_lookup_0;
uintptr_t *readlookup2  = readlookup2_0;
uintptr_t *writelookup2 = writelookup2_0;

static mem_tlb_t *mem_tlb_cur = &mem_tlb_0;

uint32_t mem_logical_addr;

int readlnum  = 0;
int writelnum = 0;
int cachesize = 256;

uint64_t get_phys_virt = ~0ULL;
uint32_t get_phys_phys;

/* Moved by every flush that may change a translation: an idle loop's proof
   (x86_idle.c) holds only while this stays as it was. */
int mmuflush        = 0;
int is_compare      = 0;

uint8_t high_page = 0; /* if a high (> 4 gb) page was detected */

/* This processor's view of the map (mem.c): outside SMM or inside it. */
mem_mapping_t **read_mapping  = mem_read_mapping_view[0];
mem_mapping_t **write_mapping = mem_write_mapping_view[0];
uint8_t       **_mem_exec     = mem_exec_view[0];

extern mem_mapping_t ram_mid_mapping2;

int
mem_addr_is_ram(uint32_t addr)
{
    const mem_mapping_t *mapping = read_mapping[addr >> MEM_GRANULARITY_BITS];

    return (mapping == &ram_low_mapping) || (mapping == &ram_high_mapping) || (mapping == &ram_mid_mapping) ||
           (mapping == &ram_mid_mapping2) || (mapping == &ram_remapped_mapping);
}

/* get_phys() keeps the last translation it made (get_phys_virt/_phys), and
   it is only good while the translations stand: every TLB flush and every
   reset forget it, or a lookup of the same linear page straight after a CR3
   load returns the old physical page. get_phys_virt is 64 bits wide so that
   the forgotten value matches no linear page, the top one included. */
static __inline void
mem_get_phys_forget(void)
{
    get_phys_virt = ~0ULL;
}

/* Translations that land outside RAM (a frame buffer, a device's
   registers), which no TLB entry holds: each access to such a page walked
   the page tables again -- tens of millions a second for a driver drawing
   into a linear frame buffer. Kept here as a TLB would keep them, until the
   processor empties its TLB or invalidates a page (a new generation), by
   linear page and privilege; one made by a write walk (which set the dirty
   bit) serves writes, any serves reads. This processor's own. */
#define MEM_MMIO_TLB 64
static struct {
    uint32_t gen;
    uint32_t vpage;
    uint64_t phys;
    uint8_t  user;
    uint8_t  write;
} mem_mmio_tlb[MEM_MMIO_TLB];
static uint32_t mem_mmio_tlb_gen = 1;

static void
mem_mmio_tlb_forget(void)
{
    if (!++mem_mmio_tlb_gen)
        mem_mmio_tlb_gen = 1;
}

/* The processor's part of a RESET: its own set emptied, and every other
   set when it is next in place. */
void
resetreadlookup(void)
{
    mem_tlb_clear(mem_tlb_cur);
    mem_tlb_mark_others(mem_tlb_cur, MEM_TLB_STALE_ALL);

    readlnext[0] = 0;
    readlnext[1] = 0;
    writelnext   = 0;
    pccache      = 0xffffffff;
    high_page    = 0;
    mem_get_phys_forget();
    mem_mmio_tlb_forget();
    mmuflush++;
}

#if defined(USE_DYNAREC) && defined(CODEGEN_CHAIN)
#    define LINKS_DROP()       codegen_links_drop_all(0)
#    define LINKS_DROP_OWNED() codegen_links_drop_all(1)
#    define LINKS_REVAL(a, l)  codegen_links_revalidate_request(a, l)
#else
#    define LINKS_DROP()
#    define LINKS_DROP_OWNED()
#    define LINKS_REVAL(a, l)
#endif

/* The bank of a set the processor is using. */
static inline mem_tlb_t *
mem_tlb_active(mem_tlb_t *t)
{
    return t->in_user ? t->ubank : t;
}

/* Where every lookup, fill and flush goes: the bank's tables and rings. */
static void
mem_tlb_point(mem_tlb_t *b)
{
    page_lookup  = b->page_lookup;
    readlookup2  = b->readlookup2;
    writelookup2 = b->writelookup2;
    readlookup   = b->readlookup;
    writelookup  = b->writelookup;
    readlnext[0] = b->readlnext[0];
    readlnext[1] = b->readlnext[1];
    writelnext   = b->writelnext;
#ifdef USE_DYNAREC
    /* Where the ARM64 recompiler's memory routines find the tables. */
    cpu_state.lookup2_r = readlookup2;
    cpu_state.lookup2_w = writelookup2;
#endif
}

static void
mem_tlb_keep(mem_tlb_t *b)
{
    b->readlnext[0] = readlnext[0];
    b->readlnext[1] = readlnext[1];
    b->writelnext   = writelnext;
}

/* Empty one bank's entries: its write entries, and its read ones too. */
static void
mem_tlb_bank_flush(mem_tlb_t *b, int read_too)
{
    if (b == NULL)
        return;
    for (uint16_t c = 0; c < 512; c++) {
        if (read_too && (b->readlookup[c] != (int) 0xffffffff)) {
            b->readlookup2[b->readlookup[c]]           = LOOKUP_INV;
            b->readlookup2[b->readlookup[c] | 1048576] = LOOKUP_INV;
            b->readlookup[c]                           = 0xffffffff;
        }
        if ((c < 256) && (b->writelookup[c] != (int) 0xffffffff)) {
            b->page_lookup[b->writelookup[c]]  = NULL;
            b->writelookup2[b->writelookup[c]] = LOOKUP_INV;
            b->writelookup[c]                  = 0xffffffff;
        }
    }
}

/* Both banks of the set in place, and the translations kept outside RAM:
   the processor's entries carry the user bit, so every flush takes both. */
static void
mem_tlb_flush_banks(int read_too)
{
    mem_mmio_tlb_forget();
    mem_tlb_bank_flush(mem_tlb_cur, read_too);
    mem_tlb_bank_flush(mem_tlb_cur->ubank, read_too);
    if (read_too)
        memset(mem_tlb_cur->large_regions, 0, sizeof(mem_tlb_cur->large_regions));
}

/* The TLB emptied, and nothing else. */
void
mem_tlb_empty(void)
{
    mem_get_phys_forget();
    mem_tlb_flush_banks(1);
}

/* A walk resolved addr through a large page (4 MB, or 2 MB under PAE). */
static inline void
mem_tlb_large(uint32_t addr)
{
    mem_tlb_cur->large_regions[addr >> 28] |= 1ULL << ((addr >> 22) & 63);
}

/* One bank's entries for a linear page. The rings may still name it; a
   ring slot reused later only invalidates what is already invalid. */
static void
mem_tlb_bank_drop_linear(mem_tlb_t *b, uint32_t vpage)
{
    if (b == NULL)
        return;
    b->readlookup2[vpage]           = LOOKUP_INV;
    b->readlookup2[vpage | 1048576] = LOOKUP_INV;
    b->writelookup2[vpage]          = LOOKUP_INV;
    b->page_lookup[vpage]           = NULL;
}

/* One bank's entries for every page of a 4 MB region. */
static void
mem_tlb_bank_drop_region(mem_tlb_t *b, uint32_t region)
{
    if (b == NULL)
        return;
    for (uint16_t c = 0; c < 512; c++) {
        const int v = b->readlookup[c];

        if ((v != (int) 0xffffffff) && (((uint32_t) v >> 10) == region)) {
            b->readlookup2[v]           = LOOKUP_INV;
            b->readlookup2[v | 1048576] = LOOKUP_INV;
            b->readlookup[c]            = 0xffffffff;
        }
        if (c < 256) {
            const int w = b->writelookup[c];

            if ((w != (int) 0xffffffff) && (((uint32_t) w >> 10) == region)) {
                b->page_lookup[w]  = NULL;
                b->writelookup2[w] = LOOKUP_INV;
                b->writelookup[c]  = 0xffffffff;
            }
        }
    }
}

/* INVLPG: the TLB entry that translates addr goes -- the one 4K page's,
   or, where a large page mapped it, the whole region's (Pentium Vol. 3,
   11.3.5; INVLPG) -- in both banks. */
static void
mem_tlb_invlpg(uint32_t addr)
{
    const uint32_t region = addr >> 22;
    uint64_t      *word   = &mem_tlb_cur->large_regions[region >> 6];
    const uint64_t bit    = 1ULL << (region & 63);

    mem_get_phys_forget();
    mem_mmio_tlb_forget();
    if (*word & bit) {
        mem_tlb_bank_drop_region(mem_tlb_cur, region);
        mem_tlb_bank_drop_region(mem_tlb_cur->ubank, region);
        *word &= ~bit;
    } else {
        mem_tlb_bank_drop_linear(mem_tlb_cur, addr >> 12);
        mem_tlb_bank_drop_linear(mem_tlb_cur->ubank, addr >> 12);
    }
}

static mem_tlb_t *
mem_tlb_bank_create(void)
{
    mem_tlb_t *b = calloc(1, sizeof(mem_tlb_t));

    if (b == NULL)
        fatal("mem_tlb_bank_create: out of memory\n");
    b->page_lookup  = malloc((1 << 20) * sizeof(page_t *));
    b->readlookup2  = malloc((2 << 20) * sizeof(uintptr_t));
    b->writelookup2 = malloc((1 << 20) * sizeof(uintptr_t));
    if ((b->page_lookup == NULL) || (b->readlookup2 == NULL) || (b->writelookup2 == NULL))
        fatal("mem_tlb_bank_create: out of memory\n");
    mem_tlb_clear(b);
    return b;
}

void
mem_tlb_bank_destroy(mem_tlb_t *b)
{
    if (b == NULL)
        return;
    free(b->page_lookup);
    free(b->readlookup2);
    free(b->writelookup2);
    free(b);
}

/* The processor's privilege is now CPL 3 (user) or not: its other bank. No
   translation changed and nothing is emptied. */
void
mem_tlb_user(int user)
{
    mem_tlb_t *t = mem_tlb_cur;

    user = !!user;
    if (t->in_user == user)
        return;
    if (user && (t->ubank == NULL))
        t->ubank = mem_tlb_bank_create();
    mem_tlb_keep(mem_tlb_active(t));
    t->in_user = user;
    mem_tlb_point(mem_tlb_active(t));
    mem_get_phys_forget();
}

/* The set every machine starts with, and the one processor 0 keeps. */
mem_tlb_t *
mem_tlb_default(void)
{
    return &mem_tlb_0;
}

/* The set in place. */
mem_tlb_t *
mem_tlb_current(void)
{
    return mem_tlb_cur;
}

/* Empty the set in place and nothing else: what a processor's own TLB
   flush does (and a set catching up on another's). */
void
mem_tlb_flush_local(int read_too)
{
    mem_tlb_flush_banks(read_too);
}

/* Put a set in place: from here every lookup, fill and flush is its. A set
   another set's flush reached since it was last in place flushes now. */
void
mem_tlb_select(mem_tlb_t *t)
{
    if ((t == NULL) || (t == mem_tlb_cur))
        return;

    /* Another processor's translations: no link holds. */
    LINKS_DROP_OWNED();

    mem_tlb_keep(mem_tlb_active(mem_tlb_cur));

    mem_tlb_cur = t;
    mem_get_phys_forget();
    mem_mmio_tlb_forget();
    mem_tlb_point(mem_tlb_active(t));

    pccache  = 0xffffffff;
    pccache2 = (uint8_t *) 0xffffffff;

    if (t->stale & MEM_TLB_STALE_ALL)
        mem_tlb_flush_local(1);
    else if (t->stale & MEM_TLB_STALE_WRITE)
        mem_tlb_flush_local(0);
    t->stale = 0;
}

static void
flushmmucache_common(void)
{
    mem_tlb_empty();
    mmuflush++;

    pccache  = (uint32_t) 0xffffffff;
    pccache2 = (uint8_t *) 0xffffffff;

#ifdef USE_DYNAREC
    codegen_flush();
#endif
}

/* Memory changed under the processor (a mapping, shadow RAM, SMRAM, A20, a
   reset): every link goes, and every processor's TLB, the others' when
   they are next in place. */
void
flushmmucache(void)
{
    LINKS_DROP();
    mem_tlb_mark_others(mem_tlb_cur, MEM_TLB_STALE_ALL);
    flushmmucache_common();
}

/* The processor's own: CR0, CR3 and CR4 loads and task switches. Linked
   blocks are checked against the new translations at the loop's next
   look, and keep their links where nothing moved (an operating system's
   kernel keeps its mappings in every address space). */
void
flushmmucache_cpu(void)
{
    LINKS_REVAL(1, 0);
    flushmmucache_common();
}

void
flushmmucache_write(void)
{
    mem_tlb_mark_others(mem_tlb_cur, MEM_TLB_STALE_WRITE);
    flushmmucache_write_cpu();
}

/* The processor's own write entries: a change of CR0.WP. */
void
flushmmucache_write_cpu(void)
{
    mem_tlb_flush_banks(0);
    mmuflush++;
}

void
flushmmucache_pc(void)
{
    mmuflush++;

    pccache  = (uint32_t) 0xffffffff;
    pccache2 = (uint8_t *) 0xffffffff;

#ifdef USE_DYNAREC
    codegen_flush();
#endif
}

void
flushmmucache_nopc(void)
{
    LINKS_DROP();
    mem_tlb_mark_others(mem_tlb_cur, MEM_TLB_STALE_ALL);
    mem_tlb_empty();
    mmuflush++;
}

void
flushmmucache_nopc_cpu(void)
{
    LINKS_REVAL(1, 0);
    mem_tlb_empty();
    mmuflush++;
}

/* A change of privilege: the lookup tables hold access checks made at the
   old one, so the other bank is put in place (mem_tlb_user), and no
   translation changed. The links hold, as their key carries the CPL
   (codegen.h CODEGEN_CHAIN_KEY). */
void
flushmmucache_cpl(void)
{
    mem_tlb_user(CPL == 3);
}

#if defined(USE_DYNAREC) && defined(CODEGEN_CHAIN)
/* The 4 MB linear regions a link's translation went through a large page
   in (a 2 MB PAE page lies inside one): an INVLPG there drops the whole
   page's entry, so every link into the region is looked at again
   (flushmmucache_invlpg). */
static uint64_t mem_link_large_regions[1024 / 64];

static __inline void
mem_link_large_note(uint32_t addr)
{
    mem_link_large_regions[addr >> 28] |= 1ULL << ((addr >> 22) & 63);
}
#endif

/* No link is left (the code cache reset): no region has one. */
void
mem_link_large_forget(void)
{
#if defined(USE_DYNAREC) && defined(CODEGEN_CHAIN)
    memset(mem_link_large_regions, 0, sizeof(mem_link_large_regions));
#endif
}

/* INVLPG: that page's entries go (mem_tlb_invlpg), and only the links
   into blocks on that linear page (or one running onto it) are checked
   against the page tables -- every link, when one went through a large
   page in that region. */
void
flushmmucache_invlpg(uint32_t addr)
{
    mem_tlb_invlpg(addr);
    mmuflush++;
#if defined(USE_DYNAREC) && defined(CODEGEN_CHAIN)
    {
        uint64_t      *word = &mem_link_large_regions[addr >> 28];
        const uint64_t bit  = 1ULL << ((addr >> 22) & 63);

        if (*word & bit) {
            *word &= ~bit;
            LINKS_REVAL(1, 0);
        } else
            LINKS_REVAL(0, addr);
    }
#else
    (void) addr;
#endif
}

/* A set's write entries to the page, by what they reach: the tracked
   path's page, or the page's RAM less the entry's linear address. */
static void
mem_tlb_drop_page(mem_tlb_t *t, const page_t *page_target, uint32_t addr)
{
    if (t == NULL)
        return;
    for (uint16_t c = 0; c < 256; c++) {
        const int v = t->writelookup[c];

        if (v == (int) 0xffffffff)
            continue;

        if ((t->page_lookup[v] == page_target) ||
            ((t->writelookup2[v] != LOOKUP_INV) &&
             ((t->writelookup2[v] + ((uintptr_t) v << 12)) == (uintptr_t) &ram[addr & ~0xfff]))) {
            t->writelookup2[v] = LOOKUP_INV;
            t->page_lookup[v]  = NULL;
            t->writelookup[c]  = 0xffffffff;
        }
    }
}

/* The page has gained code: no set may keep a direct write entry to it,
   whatever linear address it maps the page at -- the one being written
   from, another alias of it, or another processor's. */
void
mem_flush_write_page(uint32_t addr, UNUSED(uint32_t virt))
{
    const page_t *page_target = &pages[addr >> 12];

    for (int i = 0; i < mem_tlb_count; i++) {
        mem_tlb_drop_page(mem_tlbs[i], page_target, addr);
        mem_tlb_drop_page(mem_tlbs[i]->ubank, page_target, addr);
    }
}

#define mmutranslate_read(addr)  mmutranslatereal(addr, 0)
#define mmutranslate_write(addr) mmutranslatereal(addr, 1)
/* A page-table walk can legitimately land on a physical page with no RAM behind it:
   `_mem_exec[]` is explicitly NULL both for addresses beyond installed RAM and for
   MMIO-style mappings that have no exec pointer. The old macros cast that NULL and
   indexed it, so a guest that merely pointed CR3 somewhere unbacked terminated the
   emulator - observed as an access violation at the `rammap(addr2)` in
   mmutranslatereal_normal() below, with an Intel Inboard 386/PC and Intel's own
   INBRDPC.SYS, which does exactly that. Hardware has no notion of "absent" here; it
   reads the bus and gets floating high bits. So substitute a shared page of 0xFF,
   matching this file's own convention that an unmapped read returns 0xFF.

   This keeps both macros usable as lvalues, which matters because several callers do
   `rammap(x) |= ...` to set accessed/dirty bits. Those writes land in the scratch page
   and are discarded, which is correct for memory that is not there - and the page stays
   all-ones because the only writes are ORs of a few bits, so it cannot drift into
   handing back a bogus present entry later. */
static uint8_t mem_unbacked_pt_page[MEM_GRANULARITY_SIZE];

#define rammap_backing(x)        (_mem_exec[(x) >> MEM_GRANULARITY_BITS] ? _mem_exec[(x) >> MEM_GRANULARITY_BITS] : mem_unbacked_pt_page)
#define rammap(x)                ((uint32_t *) rammap_backing(x))[((x) >> 2) & MEM_GRANULARITY_QMASK]
#define rammap64(x)              ((uint64_t *) rammap_backing(x))[((x) >> 3) & MEM_GRANULARITY_PMASK]

/* The PAE page-directory-pointer table entries, loaded with CR3 and kept
   until CR3 is loaded again (Pentium Pro vol. 3, 3.8). */
uint64_t pae_pdpte[4];

/* A CR3 load with PAE on: #GP(0) for a present entry with reserved bits
   set (answers 1), otherwise the four entries are loaded. */
int
pae_check_load(uint32_t cr3_val)
{
    uint64_t entries[4];

    for (int i = 0; i < 4; i++) {
        entries[i] = rammap64((cr3_val & ~0x1f) + (i << 3));
        if ((entries[i] & 1) && (entries[i] & 0xfffffff0000001e6ULL))
            return 1;
    }
    for (int i = 0; i < 4; i++)
        pae_pdpte[i] = entries[i];
    return 0;
}

void
pae_load_pdptes(void)
{
    for (int i = 0; i < 4; i++)
        pae_pdpte[i] = rammap64((cr3 & ~0x1f) + (i << 3));
}

static __inline uint64_t
mmutranslatereal_normal(uint32_t addr, int rw)
{
    uint32_t temp;
    uint32_t temp2;
    uint32_t temp3;
    uint32_t addr2;

    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    addr2 = ((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
    temp = temp2 = rammap(addr2);
    if (!(temp & 1)) {
        cr2 = addr;
        temp &= 1;
        if ((CPL == 3) && !cpl_override)
            temp |= 4;
        if (rw)
            temp |= 2;
        cpu_state.abrt = ABRT_PF;
        abrt_error     = temp;
        return 0xffffffffffffffffULL;
    }

    if ((temp & 0x80) && (cr4 & CR4_PSE)) {
        /*4MB page*/
        if (((CPL == 3) && !(temp & 4) && !cpl_override) || (rw && !(temp & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG))))) {
            cr2 = addr;
            temp &= 1;
            if ((CPL == 3) && !cpl_override)
                temp |= 4;
            if (rw)
                temp |= 2;
            cpu_state.abrt = ABRT_PF;
            abrt_error     = temp;

            return 0xffffffffffffffffULL;
        }

        rammap(addr2) |= (rw ? 0x60 : 0x20);
        mem_tlb_large(addr);

        uint64_t page = temp & ~0x3fffff;
        if (cpu_features & CPU_FEATURE_PSE36)
            page |= (uint64_t) (temp & 0x1e000) << 19;
        return page + (addr & 0x3fffff);
    }

    temp  = rammap((temp & ~0xfff) + ((addr >> 10) & 0xffc));
    temp3 = temp & temp2;
    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG))))) {
        cr2 = addr;
        temp &= 1;
        if ((CPL == 3) && !cpl_override)
            temp |= 4;
        if (rw)
            temp |= 2;
        cpu_state.abrt = ABRT_PF;
        abrt_error     = temp;
        return 0xffffffffffffffffULL;
    }

    rammap(addr2) |= 0x20;
    rammap((temp2 & ~0xfff) + ((addr >> 10) & 0xffc)) |= (rw ? 0x60 : 0x20);

    return (uint64_t) ((temp & ~0xfff) + (addr & 0xfff));
}

static __inline uint64_t
mmutranslatereal_pae(uint32_t addr, int rw)
{
    uint64_t temp;
    uint64_t temp2;
    uint64_t temp3;
    uint64_t temp4;
    uint64_t addr3;
    uint64_t addr4;

    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    temp = temp2 = pae_pdpte[(addr >> 30) & 3] & 0x000000ffffffffffULL;
    if (!(temp & 1)) {
        cr2 = addr;
        temp &= 1;
        if ((CPL == 3) && !cpl_override)
            temp |= 4;
        if (rw)
            temp |= 2;
        cpu_state.abrt = ABRT_PF;
        abrt_error     = temp;
        return 0xffffffffffffffffULL;
    }

    addr3 = (temp & ~0xfffULL) + ((addr >> 18) & 0xff8);
    temp = temp4 = rammap64(addr3) & 0x000000ffffffffffULL;
    temp3        = temp & temp2;
    if (!(temp & 1)) {
        cr2 = addr;
        temp &= 1;
        if ((CPL == 3) && !cpl_override)
            temp |= 4;
        if (rw)
            temp |= 2;
        cpu_state.abrt = ABRT_PF;
        abrt_error     = temp;
        return 0xffffffffffffffffULL;
    }

    if (temp & 0x80) {
        /*2MB page*/
        if (((CPL == 3) && !(temp & 4) && !cpl_override) || (rw && !(temp & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG))))) {
            cr2 = addr;
            temp &= 1;
            if ((CPL == 3) && !cpl_override)
                temp |= 4;
            if (rw)
                temp |= 2;
            cpu_state.abrt = ABRT_PF;
            abrt_error     = temp;

            return 0xffffffffffffffffULL;
        }
        rammap64(addr3) |= (rw ? 0x60 : 0x20);
        mem_tlb_large(addr);

        return ((temp & ~0x1fffffULL) + (addr & 0x1fffffULL)) & 0x000000ffffffffffULL;
    }

    addr4 = (temp & ~0xfffULL) + ((addr >> 9) & 0xff8);
    temp  = rammap64(addr4) & 0x000000ffffffffffULL;
    temp3 = temp & temp4;
    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG))))) {
        cr2 = addr;
        temp &= 1;
        if ((CPL == 3) && !cpl_override)
            temp |= 4;
        if (rw)
            temp |= 2;
        cpu_state.abrt = ABRT_PF;
        abrt_error     = temp;
        return 0xffffffffffffffffULL;
    }

    rammap64(addr3) |= 0x20;
    rammap64(addr4) |= (rw ? 0x60 : 0x20);

    return ((temp & ~0xfffULL) + ((uint64_t) (addr & 0xfff))) & 0x000000ffffffffffULL;
}

uint64_t
mmutranslatereal(uint32_t addr, int rw)
{
    const uint32_t vpage = addr >> 12;
    const uint8_t  user  = (CPL == 3) && !cpl_override;
    const int      e     = vpage & (MEM_MMIO_TLB - 1);
    uint64_t       r;

    /* Fast path to return invalid without any call if an exception has occurred beforehand. */
    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    /* A supervisor access at CPL 3 (cpl_override) that the CPL 0-2 bank
       holds: that entry was made by a supervisor walk that set the
       accessed and dirty bits already, so a walk would find what it did. */
    if (cpl_override && mem_tlb_cur->in_user) {
        const mem_tlb_t *s = mem_tlb_cur;

        if (rw) {
            if (s->page_lookup[vpage] != NULL)
                return ((uint64_t) (s->page_lookup[vpage] - pages) << 12) | (addr & 0xfff);
            if (s->writelookup2[vpage] != (uintptr_t) LOOKUP_INV)
                return (uint64_t) (s->writelookup2[vpage] + addr - (uintptr_t) ram);
        } else if (s->readlookup2[vpage] != (uintptr_t) LOOKUP_INV)
            return (uint64_t) (s->readlookup2[vpage] + addr - (uintptr_t) ram);
    }

    if ((mem_mmio_tlb[e].gen == mem_mmio_tlb_gen) && (mem_mmio_tlb[e].vpage == vpage) &&
        (mem_mmio_tlb[e].user == user) && (mem_mmio_tlb[e].write || !rw))
        return mem_mmio_tlb[e].phys | (addr & 0xfff);

    if (cr4 & CR4_PAE)
        r = mmutranslatereal_pae(addr, rw);
    else
        r = mmutranslatereal_normal(addr, rw);

    if ((r != 0xffffffffffffffffULL) && ((size_t) (r & ~0xfffULL) >= mem_ram_size())) {
        mem_mmio_tlb[e].gen   = mem_mmio_tlb_gen;
        mem_mmio_tlb[e].vpage = vpage;
        mem_mmio_tlb[e].phys  = r & ~0xfffULL;
        mem_mmio_tlb[e].user  = user;
        mem_mmio_tlb[e].write = !!rw;
    }
    return r;
}

/* This is needed because the old recompiler calls this to check for page fault. */
uint32_t
mmutranslatereal32(uint32_t addr, int rw)
{
    /* Fast path to return invalid without any call if an exception has occurred beforehand. */
    if (cpu_state.abrt)
        return (uint32_t) 0xffffffffffffffffULL;

    return (uint32_t) mmutranslatereal(addr, rw);
}

static __inline uint64_t
mmutranslate_noabrt_normal(uint32_t addr, int rw)
{
    uint32_t temp;
    uint32_t temp2;
    uint32_t temp3;
    uint32_t addr2;

    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    addr2 = ((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
    temp = temp2 = rammap(addr2);

    if (!(temp & 1))
        return 0xffffffffffffffffULL;

    if ((temp & 0x80) && (cr4 & CR4_PSE)) {
        /*4MB page*/
        if (((CPL == 3) && !(temp & 4) && !cpl_override) || (rw && !(temp & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG)))))
            return 0xffffffffffffffffULL;

        uint64_t page = temp & ~0x3fffff;
        if (cpu_features & CPU_FEATURE_PSE36)
            page |= (uint64_t) (temp & 0x1e000) << 19;
        mem_tlb_large(addr); /* a fill may follow (get_phys_noabrt) */
        return page + (addr & 0x3fffff);
    }

    temp  = rammap((temp & ~0xfff) + ((addr >> 10) & 0xffc));
    temp3 = temp & temp2;

    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG)))))
        return 0xffffffffffffffffULL;

    return (uint64_t) ((temp & ~0xfff) + (addr & 0xfff));
}

static __inline uint64_t
mmutranslate_noabrt_pae(uint32_t addr, int rw)
{
    uint64_t temp;
    uint64_t temp2;
    uint64_t temp3;
    uint64_t temp4;
    uint64_t addr3;
    uint64_t addr4;

    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    temp = temp2 = pae_pdpte[(addr >> 30) & 3] & 0x000000ffffffffffULL;

    if (!(temp & 1))
        return 0xffffffffffffffffULL;

    addr3 = (temp & ~0xfffULL) + ((addr >> 18) & 0xff8);
    temp = temp4 = rammap64(addr3) & 0x000000ffffffffffULL;
    temp3        = temp & temp2;

    if (!(temp & 1))
        return 0xffffffffffffffffULL;

    if (temp & 0x80) {
        /*2MB page*/
        if (((CPL == 3) && !(temp & 4) && !cpl_override) || (rw && !(temp & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG)))))
            return 0xffffffffffffffffULL;

        mem_tlb_large(addr); /* a fill may follow (get_phys_noabrt) */
        return ((temp & ~0x1fffffULL) + (addr & 0x1fffff)) & 0x000000ffffffffffULL;
    }

    addr4 = (temp & ~0xfffULL) + ((addr >> 9) & 0xff8);
    temp  = rammap64(addr4) & 0x000000ffffffffffULL;

    temp3 = temp & temp4;

    if (!(temp & 1) || ((CPL == 3) && !(temp3 & 4) && !cpl_override) || (rw && !(temp3 & 2) && (((CPL == 3) && !cpl_override) || ((is486 || isibm486) && (cr0 & WP_FLAG)))))
        return 0xffffffffffffffffULL;

    return ((temp & ~0xfffULL) + ((uint64_t) (addr & 0xfff))) & 0x000000ffffffffffULL;
}

uint64_t
mmutranslate_noabrt(uint32_t addr, int rw)
{
    /* Fast path to return invalid without any call if an exception has occurred beforehand. */
    if (cpu_state.abrt)
        return 0xffffffffffffffffULL;

    if (cr4 & CR4_PAE)
        return mmutranslate_noabrt_pae(addr, rw);
    else
        return mmutranslate_noabrt_normal(addr, rw);
}

#if defined(USE_DYNAREC) && defined(CODEGEN_CHAIN)
/* Where a linear page translates for a link (codegen_block.c): the
   physical page as get_phys() gives it, | 2 when user code may run there,
   | 1 when present; 0 when not present or beyond the physical space. It
   sets no accessed bit and raises no fault: an entry on the way whose
   accessed bit is clear gives 0 as well, so the link misses and the
   dispatcher's own walk sets the bit, as the processor's fetch would. */
uint64_t
mmu_link_translate(uint32_t addr)
{
    uint64_t page;
    int      user;

    addr &= ~0xfff;
    if (!(cr0 >> 31))
        return ((uint64_t) (addr & rammask) & ~0xfffULL) | 3;

    if (cr4 & CR4_PAE) {
        uint64_t pdpte = pae_pdpte[(addr >> 30) & 3] & 0x000000ffffffffffULL; /* as loaded with CR3 */
        uint64_t pde;
        uint64_t pte;

        if (!(pdpte & 1))
            return 0;
        pde = rammap64((pdpte & ~0xfffULL) + ((addr >> 18) & 0xff8)) & 0x000000ffffffffffULL;
        if ((pde & 0x21) != 0x21)
            return 0;
        if (pde & 0x80) {
            page = (pde & ~0x1fffffULL) + (addr & 0x1fffff);
            user = !!(pde & 4);
            mem_link_large_note(addr);
        } else {
            pte = rammap64((pde & ~0xfffULL) + ((addr >> 9) & 0xff8)) & 0x000000ffffffffffULL;
            if ((pte & 0x21) != 0x21)
                return 0;
            page = pte & ~0xfffULL;
            user = !!(pte & pde & 4);
        }
    } else {
        uint32_t pde = rammap((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
        uint32_t pte;

        if ((pde & 0x21) != 0x21)
            return 0;
        if ((pde & 0x80) && (cr4 & CR4_PSE)) {
            page = (uint64_t) (pde & ~0x3fffff);
            if (cpu_features & CPU_FEATURE_PSE36)
                page |= (uint64_t) (pde & 0x1e000) << 19;
            page += addr & 0x3fffff;
            user = !!(pde & 4);
            mem_link_large_note(addr);
        } else {
            pte = rammap((pde & ~0xfff) + ((addr >> 10) & 0xffc));
            if ((pte & 0x21) != 0x21)
                return 0;
            page = pte & ~0xfff;
            user = !!(pte & pde & 4);
        }
    }
    /* Beyond 4 GB get_phys() gives FFFFFFFFh, as here. */
    if (page > 0xffffffffULL)
        page = 0xffffffffULL;
    return ((uint64_t) ((uint32_t) page & rammask) & ~0xfffULL) | (user ? 2 : 0) | 1;
}
#endif

/* Whether code at CPL 3 may make this access (a write when rw) to the
   linear page: user on every level, and writable on every level for a
   write. Present pages only; it sets nothing and raises nothing. */
static int
mem_tlb_user_may(uint32_t addr, int rw)
{
    const uint32_t need = rw ? 6 : 4;

    if (!(cr0 >> 31))
        return 1;
    if (cr4 & CR4_PAE) {
        const uint64_t pdpte = pae_pdpte[(addr >> 30) & 3];
        uint64_t       pde;
        uint64_t       pte;

        if (!(pdpte & 1))
            return 0;
        pde = rammap64((pdpte & 0x000000fffffff000ULL) + ((addr >> 18) & 0xff8));
        if (!(pde & 1))
            return 0;
        if (pde & 0x80)
            return (pde & need) == need;
        pte = rammap64((pde & 0x000000fffffff000ULL) + ((addr >> 9) & 0xff8));
        return (pte & 1) && ((pde & pte & need) == need);
    } else {
        const uint32_t pde = rammap((cr3 & ~0xfff) + ((addr >> 20) & 0xffc));
        uint32_t       pte;

        if (!(pde & 1))
            return 0;
        if ((pde & 0x80) && (cr4 & CR4_PSE))
            return (pde & need) == need;
        pte = rammap((pde & ~0xfff) + ((addr >> 10) & 0xffc));
        return (pte & 1) && ((pde & pte & need) == need);
    }
}

static void
addreadlookup_bank(uint32_t virt, uint32_t phys)
{
    uint32_t large_offset = is_compare ? 1048576 : 0;
    uint32_t small_offset = is_compare ? 256 : 0;
    uint32_t index        = large_offset | (virt >> 12);
    int *    rln          = &(readlnext[is_compare]);
    int      cur_rln      = *rln | (int) small_offset;

    if (virt == 0xffffffff)
        return;

    if (GDBSTUB_WATCHED(virt))
        return;

    /* An idle loop's pass being proven: every access through the slow
       paths (x86_idle.c). */
    if (x86_idle_watch)
        return;

    if (readlookup2[index] != (uintptr_t) LOOKUP_INV)
        return;

    if (readlookup[cur_rln] != (int) 0xffffffff)
        readlookup2[large_offset | readlookup[cur_rln]] = LOOKUP_INV;

    readlookup2[index] = (uintptr_t) &ram[(uintptr_t) (phys & ~0xFFF) - (uintptr_t) (virt & ~0xfff)];

    readlookup[cur_rln] = virt >> 12;
    *rln                = (*rln + 1) & (cachesize - 1);

    cycles -= 9;
}

/* A fill goes into the bank of the privilege its walk was made at. A
   supervisor access at CPL 3 (a descriptor table, the TSS: cpl_override)
   walks as CPL 0: to a page CPL 3 code may itself reach that way, its
   entry is as good for CPL 3 and goes into that bank; to any other, into
   the CPL 0-2 bank, where CPL 3 code never looks, and the next such access
   finds it there (mmutranslatereal) instead of walking again. */
void
addreadlookup(uint32_t virt, uint32_t phys)
{
    if (cpl_override && (CPL == 3) && !mem_tlb_user_may(virt, 0)) {
        mem_tlb_user(0);
        addreadlookup_bank(virt, phys);
        mem_tlb_user(1);
        return;
    }
    if (mem_tlb_cur->in_user != (CPL == 3))
        mem_tlb_user(CPL == 3);
    addreadlookup_bank(virt, phys);
}

static void
addwritelookup_bank(uint32_t virt, uint32_t phys)
{
    if (virt == 0xffffffff)
        return;

    if (GDBSTUB_WATCHED(virt))
        return;

    if (x86_idle_watch)
        return;

    /* Already there, either way a page is entered: a second fill would
       evict another page's entry and leave the ring two slots for one. */
    if (page_lookup[virt >> 12] || (writelookup2[virt >> 12] != LOOKUP_INV))
        return;

    if (writelookup[writelnext] != -1) {
        page_lookup[writelookup[writelnext]]  = NULL;
        writelookup2[writelookup[writelnext]] = LOOKUP_INV;
    }

    /* A page holds code not only when a block starts in it (block) but also
       when a block crossing a page boundary ends in it (block_2). Writes to
       such a page must take the tracked page_lookup path, or self-modifying
       code in the second page never invalidates the spanning block. */
#ifdef USE_DYNAREC
    if (pages[phys >> 12].block || pages[phys >> 12].block_2 || (phys & ~0xfff) == recomp_page) {
#else
    if (pages[phys >> 12].block || pages[phys >> 12].block_2) {
#endif
        page_lookup[virt >> 12]  = &pages[phys >> 12];
    } else
        writelookup2[virt >> 12] = (uintptr_t) &ram[(uintptr_t) (phys & ~0xFFF) - (uintptr_t) (virt & ~0xfff)];

    writelookup[writelnext++] = virt >> 12;
    writelnext &= (cachesize - 1);

    cycles -= 9;
}

/* As addreadlookup. */
void
addwritelookup(uint32_t virt, uint32_t phys)
{
    if (cpl_override && (CPL == 3) && !mem_tlb_user_may(virt, 1)) {
        mem_tlb_user(0);
        addwritelookup_bank(virt, phys);
        mem_tlb_user(1);
        return;
    }
    if (mem_tlb_cur->in_user != (CPL == 3))
        mem_tlb_user(CPL == 3);
    addwritelookup_bank(virt, phys);
}

uint8_t *
getpccache(uint32_t a)
{
    uint64_t a64 = (uint64_t) a;
    uint8_t *p;
    uint32_t a2;

    a2 = a;

    if (cr0 >> 31) {
        a64 = mmutranslate_read(a64);

        if (a64 == 0xffffffffffffffffULL)
            return ram;
    }
    if (mem_a20_reset_vector_bypass &&
        ((a & 0xfffff000U) == 0xfffff000U))
        mem_a20_reset_vector_bypass = 0;
    else
        a64 &= rammask;

    if (_mem_exec[a64 >> MEM_GRANULARITY_BITS]) {
        if (is286) {
            if (read_mapping[a64 >> MEM_GRANULARITY_BITS] && (read_mapping[a64 >> MEM_GRANULARITY_BITS]->flags & MEM_MAPPING_ROM_WS))
                cpu_prefetch_cycles = cpu_rom_prefetch_cycles;
            else
                cpu_prefetch_cycles = cpu_mem_prefetch_cycles;
        }

        p = &_mem_exec[a64 >> MEM_GRANULARITY_BITS][(uintptr_t) (a64 & MEM_GRANULARITY_PAGE) - (uintptr_t) (a2 & ~0xfff)];
        return (uint8_t *) (((uintptr_t) p & 0x00000000ffffffffULL) | ((uintptr_t) &_mem_exec[a64 >> MEM_GRANULARITY_BITS][0] & 0xffffffff00000000ULL));
    }

    /* No RAM or ROM behind the page (video memory, a device's buffer): the
       fetch is an ordinary bus read, which the caller does through the read
       handlers. The recompiler must not keep a block built from it, since
       nothing tracks writes to device memory. */
    cpu_fetch_device = 1;

    return NULL;
}

uint8_t
read_mem_b(uint32_t addr)
{
    mem_mapping_t *map;
    uint8_t        ret        = 0xff;

    mem_logical_addr = addr;
    addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
        ret = map->read_b(addr, map->priv);

    return ret;
}

uint16_t
read_mem_w(uint32_t addr)
{
    mem_mapping_t *map;
    uint16_t       ret        = 0xffff;

    mem_logical_addr = addr;
    addr &= rammask;

    if (addr & 1)
        ret = read_mem_b(addr) | (read_mem_b(addr + 1) << 8);
    else {
        map = read_mapping[addr >> MEM_GRANULARITY_BITS];

        if (map && map->read_w)
            ret = map->read_w(addr, map->priv);
        else if (map && map->read_b)
            ret = map->read_b(addr, map->priv) | (map->read_b(addr + 1, map->priv) << 8);
    }

    return ret;
}

void
write_mem_b(uint32_t addr, uint8_t val)
{
    mem_mapping_t *map;

    mem_logical_addr = addr;
    addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
        map->write_b(addr, val, map->priv);
}

void
write_mem_w(uint32_t addr, uint16_t val)
{
    mem_mapping_t *map;

    mem_logical_addr = addr;
    addr &= rammask;

    if (addr & 1) {
        write_mem_b(addr, val);
        write_mem_b(addr + 1, val >> 8);
    } else {
        map = write_mapping[addr >> MEM_GRANULARITY_BITS];
        if (map) {
            if (map->write_w)
                map->write_w(addr, val, map->priv);
            else if (map->write_b) {
                map->write_b(addr, val, map->priv);
                map->write_b(addr + 1, val >> 8, map->priv);
            }
        }
    }
}

/* An idle loop's pass being proven (x86_idle.c): every access the processor
   makes comes through these slow paths, and is logged. */
#define IDLE_NOTE(addr, size, write, val)                               \
    do {                                                                \
        if (x86_idle_watch)                                             \
            x86_idle_note((addr), (size), (write), (uint64_t) (val));   \
    } while (0)

uint8_t
readmembl(uint32_t addr)
{
    mem_mapping_t *map;
    uint64_t       a;

    IDLE_NOTE(addr, 1, 0, 0);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 1);
#ifdef USE_DEBUG_REGS_486
    mem_debug_check_addr(addr, read_type);
#endif

    addr64           = (uint64_t) addr;
    mem_logical_addr = addr;

    high_page = 0;

    if (cr0 >> 31) {
        a      = mmutranslate_read(addr);
        addr64 = (uint32_t) a;

        if (a > 0xffffffffULL)
            return 0xff;
    }
    addr = (uint32_t) (addr64 & rammask);

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
        return map->read_b(addr, map->priv);

    return 0xff;
}

void
writemembl(uint32_t addr, uint8_t val)
{
    mem_mapping_t *map;
    uint64_t       a;

    IDLE_NOTE(addr, 1, 1, val);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 1);
#ifdef USE_DEBUG_REGS_486
    mem_debug_check_addr(addr, 2);
#endif

    addr64           = (uint64_t) addr;
    mem_logical_addr = addr;

    high_page = 0;

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_b) {
        page_lookup[addr >> 12]->write_b(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        a      = mmutranslate_write(addr);
        addr64 = (uint32_t) a;

        if (a > 0xffffffffULL)
            return;
    }
    addr = (uint32_t) (addr64 & rammask);

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
        map->write_b(addr, val, map->priv);
}

/* Read a byte from memory without MMU translation - result of previous MMU translation passed as value. */
uint8_t
readmembl_no_mmut(uint32_t addr, uint32_t a64)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 1, 0, 0);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 1);

    mem_logical_addr = addr;

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return 0xff;

        addr = a64 & rammask;
    } else
        addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->read_b)
        return map->read_b(addr, map->priv);

    return 0xff;
}

/* Write a byte to memory without MMU translation - result of previous MMU translation passed as value. */
void
writemembl_no_mmut(uint32_t addr, uint32_t a64, uint8_t val)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 1, 1, val);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 1);

    mem_logical_addr = addr;

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_b) {
        page_lookup[addr >> 12]->write_b(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return;

        addr = a64 & rammask;
    } else
        addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
        map->write_b(addr, val, map->priv);
}

uint16_t
readmemwl(uint32_t addr)
{
    mem_mapping_t *map;
    uint64_t       a;

    IDLE_NOTE(addr, 2, 0, 0);
    addr64a[0] = addr;
    addr64a[1] = addr + 1;
#ifdef USE_DEBUG_REGS_486
    mem_debug_check_addr(addr, read_type);
    mem_debug_check_addr(addr + 1, read_type);
#endif
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 2);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 1) {
        uint32_t       large_offset = is_compare ? 1048576 : 0;
        uintptr_t     *rl2 = &(readlookup2[large_offset | (addr >> 12)]);

        if (!cpu_cyrix_alignment || (addr & 7) == 7)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffe) {
            if (cr0 >> 31) {
                for (uint8_t i = 0; i < 2; i++) {
                    a          = mmutranslate_read(addr + i);
                    addr64a[i] = (uint32_t) a;

                    if (a > 0xffffffffULL)
                        return 0xffff;
                }
            }

            return readmembl_no_mmut(addr, addr64a[0]) | (((uint16_t) readmembl_no_mmut(addr + 1, addr64a[1])) << 8);
        } else if (*rl2 != (uintptr_t) LOOKUP_INV)
            return *(uint16_t *) (*rl2 + addr);
    }

    if (cr0 >> 31) {
        a          = mmutranslate_read(addr);
        addr64a[0] = (uint32_t) a;

        if (a > 0xffffffffULL)
            return 0xffff;
    } else
        addr64a[0] = (uint64_t) addr;

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_w)
        return map->read_w(addr, map->priv);

    if (map && map->read_b) {
        return map->read_b(addr, map->priv) | ((uint16_t) (map->read_b(addr + 1, map->priv)) << 8);
    }

    return 0xffff;
}

void
writememwl(uint32_t addr, uint16_t val)
{
    mem_mapping_t *map;
    uint64_t       a;

    IDLE_NOTE(addr, 2, 1, val);
    addr64a[0] = addr;
    addr64a[1] = addr + 1;
#ifdef USE_DEBUG_REGS_486
    mem_debug_check_addr(addr, 2);
    mem_debug_check_addr(addr + 1, 2);
#endif
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 2);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 1) {
        if (!cpu_cyrix_alignment || (addr & 7) == 7)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffe) {
            if (cr0 >> 31) {
                for (uint8_t i = 0; i < 2; i++) {
                    /* A page with a valid lookup is already translated: take its physical
                       address from the lookup, since writing the first half can recycle
                       the entry and the second half then falls back to addr64a[]. */
                    if (page_lookup[(addr + i) >> 12] && page_lookup[(addr + i) >> 12]->write_b) {
                        a          = ((uint64_t) (page_lookup[(addr + i) >> 12] - pages) << 12) | ((addr + i) & 0xfff);
                        addr64a[i] = (uint32_t) a;
                    } else {
                        a          = mmutranslate_write(addr + i);
                        addr64a[i] = (uint32_t) a;

                        if (a > 0xffffffffULL)
                            return;
                    }
                }
            }

            /* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
               their result as a parameter to be used if needed. */
            writemembl_no_mmut(addr, addr64a[0], val);
            writemembl_no_mmut(addr + 1, addr64a[1], val >> 8);
            return;
        } else if (writelookup2[addr >> 12] != (uintptr_t) LOOKUP_INV) {
            *(uint16_t *) (writelookup2[addr >> 12] + addr) = val;
            return;
        }
    }

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_w) {
        page_lookup[addr >> 12]->write_w(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        a          = mmutranslate_write(addr);
        addr64a[0] = (uint32_t) a;

        if (a > 0xffffffffULL)
            return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_w) {
        map->write_w(addr, val, map->priv);
        return;
    }

    if (map && map->write_b) {
        map->write_b(addr, val, map->priv);
        map->write_b(addr + 1, val >> 8, map->priv);
        return;
    }
}

/* Read a word from memory without MMU translation - results of previous MMU translation passed as array. */
uint16_t
readmemwl_no_mmut(uint32_t addr, uint32_t *a64)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 2, 0, 0);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 2);

    mem_logical_addr = addr;

    if (addr & 1) {
        uint32_t       large_offset = is_compare ? 1048576 : 0;
        uintptr_t     *rl2 = &(readlookup2[large_offset | (addr >> 12)]);

        if (!cpu_cyrix_alignment || (addr & 7) == 7)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffe) {
            if (cr0 >> 31) {
                if (cpu_state.abrt || high_page)
                    return 0xffff;
            }

            return readmembl_no_mmut(addr, a64[0]) | (((uint16_t) readmembl_no_mmut(addr + 1, a64[1])) << 8);
        } else if (*rl2 != (uintptr_t) LOOKUP_INV)
            return *(uint16_t *) (*rl2 + addr);
    }

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return 0xffff;

        addr = (uint32_t) (a64[0] & rammask);
    } else
        addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_w)
        return map->read_w(addr, map->priv);

    if (map && map->read_b) {
        return map->read_b(addr, map->priv) | ((uint16_t) (map->read_b(addr + 1, map->priv)) << 8);
    }

    return 0xffff;
}

/* Write a word to memory without MMU translation - results of previous MMU translation passed as array. */
void
writememwl_no_mmut(uint32_t addr, uint32_t *a64, uint16_t val)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 2, 1, val);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 2);

    mem_logical_addr = addr;

    if (addr & 1) {
        if (!cpu_cyrix_alignment || (addr & 7) == 7)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffe) {
            if (cr0 >> 31) {
                if (cpu_state.abrt || high_page)
                    return;
            }

            writemembl_no_mmut(addr, a64[0], val);
            writemembl_no_mmut(addr + 1, a64[1], val >> 8);
            return;
        } else if (writelookup2[addr >> 12] != (uintptr_t) LOOKUP_INV) {
            *(uint16_t *) (writelookup2[addr >> 12] + addr) = val;
            return;
        }
    }

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_w) {
        page_lookup[addr >> 12]->write_w(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return;

        addr = (uint32_t) (a64[0] & rammask);
    } else
        addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_w) {
        map->write_w(addr, val, map->priv);
        return;
    }

    if (map && map->write_b) {
        map->write_b(addr, val, map->priv);
        map->write_b(addr + 1, val >> 8, map->priv);
        return;
    }
}

uint32_t
readmemll(uint32_t addr)
{
    mem_mapping_t *map;
    int            i;
    uint64_t       a = 0x0000000000000000ULL;

    IDLE_NOTE(addr, 4, 0, 0);
    for (i = 0; i < 4; i++) {
        addr64a[i] = (uint64_t) (addr + i);
#ifdef USE_DEBUG_REGS_486
        mem_debug_check_addr(addr + i, read_type);
#endif
    }
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 4);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 3) {
        uint32_t       large_offset = is_compare ? 1048576 : 0;
        uintptr_t     *rl2 = &(readlookup2[large_offset | (addr >> 12)]);

        if (!cpu_cyrix_alignment || (addr & 7) > 4)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffc) {
            if (cr0 >> 31) {
                for (i = 0; i < 4; i++) {
                    if (i == 0) {
                        a          = mmutranslate_read(addr + i);
                        addr64a[i] = (uint32_t) a;
                    } else if (!((addr + i) & 0xfff)) {
                        a          = mmutranslate_read(addr + 3);
                        addr64a[i] = (uint32_t) a;
                        if (!cpu_state.abrt) {
                            a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                            addr64a[i] = (uint32_t) a;
                        }
                    } else {
                        a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                        addr64a[i] = (uint32_t) a;
                    }

                    if (a > 0xffffffffULL)
                        return 0xffff;
                }
            }

            /* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
               their result as a parameter to be used if needed. */
            return readmemwl_no_mmut(addr, addr64a) | (((uint32_t) readmemwl_no_mmut(addr + 2, &(addr64a[2]))) << 16);
        } else if (*rl2 != (uintptr_t) LOOKUP_INV)
            return *(uint32_t *) (*rl2 + addr);
    }

    if (cr0 >> 31) {
        a          = mmutranslate_read(addr);
        addr64a[0] = (uint32_t) a;

        if (a > 0xffffffffULL)
            return 0xffffffff;
    }

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_l)
        return map->read_l(addr, map->priv);

    if (map && map->read_w)
        return map->read_w(addr, map->priv) | ((uint32_t) (map->read_w(addr + 2, map->priv)) << 16);

    if (map && map->read_b)
        return map->read_b(addr, map->priv) | ((uint32_t) (map->read_b(addr + 1, map->priv)) << 8) | ((uint32_t) (map->read_b(addr + 2, map->priv)) << 16) | ((uint32_t) (map->read_b(addr + 3, map->priv)) << 24);

    return 0xffffffff;
}

void
writememll(uint32_t addr, uint32_t val)
{
    mem_mapping_t *map;
    int            i;
    uint64_t       a = 0x0000000000000000ULL;

    IDLE_NOTE(addr, 4, 1, val);
    for (i = 0; i < 4; i++) {
        addr64a[i] = (uint64_t) (addr + i);
#ifdef USE_DEBUG_REGS_486
        mem_debug_check_addr(addr + i, 2);
#endif
    }
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 4);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 3) {
        if (!cpu_cyrix_alignment || (addr & 7) > 4)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffc) {
            if (cr0 >> 31) {
                for (i = 0; i < 4; i++) {
                    /* A page with a valid lookup is already translated: take its physical
                       address from the lookup, since writing the first part can recycle
                       the entry and the rest then falls back to addr64a[]. */
                    if (page_lookup[(addr + i) >> 12] && page_lookup[(addr + i) >> 12]->write_b) {
                        a          = ((uint64_t) (page_lookup[(addr + i) >> 12] - pages) << 12) | ((addr + i) & 0xfff);
                        addr64a[i] = (uint32_t) a;
                    } else {
                        if (i == 0) {
                            a          = mmutranslate_write(addr + i);
                            addr64a[i] = (uint32_t) a;
                        } else if (!((addr + i) & 0xfff)) {
                            a          = mmutranslate_write(addr + 3);
                            addr64a[i] = (uint32_t) a;
                            if (!cpu_state.abrt) {
                                a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                                addr64a[i] = (uint32_t) a;
                            }
                        } else {
                            a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                            addr64a[i] = (uint32_t) a;
                        }

                        if (a > 0xffffffffULL)
                            return;
                    }
                }
            }

            /* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
               their result as a parameter to be used if needed. */
            writememwl_no_mmut(addr, &(addr64a[0]), val);
            writememwl_no_mmut(addr + 2, &(addr64a[2]), val >> 16);
            return;
        } else if (writelookup2[addr >> 12] != (uintptr_t) LOOKUP_INV) {
            *(uint32_t *) (writelookup2[addr >> 12] + addr) = val;
            return;
        }
    }

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_l) {
        page_lookup[addr >> 12]->write_l(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        a          = mmutranslate_write(addr);
        addr64a[0] = (uint32_t) a;

        if (a > 0xffffffffULL)
            return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
        map->write_l(addr, val, map->priv);
        return;
    }
    if (map && map->write_w) {
        map->write_w(addr, val, map->priv);
        map->write_w(addr + 2, val >> 16, map->priv);
        return;
    }
    if (map && map->write_b) {
        map->write_b(addr, val, map->priv);
        map->write_b(addr + 1, val >> 8, map->priv);
        map->write_b(addr + 2, val >> 16, map->priv);
        map->write_b(addr + 3, val >> 24, map->priv);
        return;
    }
}

/* Read a long from memory without MMU translation - results of previous MMU translation passed as array. */
uint32_t
readmemll_no_mmut(uint32_t addr, uint32_t *a64)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 4, 0, 0);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_READ, 4);

    mem_logical_addr = addr;

    if (addr & 3) {
        uint32_t       large_offset = is_compare ? 1048576 : 0;
        uintptr_t     *rl2 = &(readlookup2[large_offset | (addr >> 12)]);

        if (!cpu_cyrix_alignment || (addr & 7) > 4)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffc) {
            if (cr0 >> 31) {
                if (cpu_state.abrt || high_page)
                    return 0xffffffff;
            }

            return readmemwl_no_mmut(addr, a64) | ((uint32_t) (readmemwl_no_mmut(addr + 2, &(a64[2]))) << 16);
        } else if (*rl2 != (uintptr_t) LOOKUP_INV)
            return *(uint32_t *) (*rl2 + addr);
    }

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return 0xffffffff;

        addr = (uint32_t) (a64[0] & rammask);
    } else
        addr &= rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_l)
        return map->read_l(addr, map->priv);

    if (map && map->read_w)
        return map->read_w(addr, map->priv) | ((uint32_t) (map->read_w(addr + 2, map->priv)) << 16);

    if (map && map->read_b)
        return map->read_b(addr, map->priv) | ((uint32_t) (map->read_b(addr + 1, map->priv)) << 8) | ((uint32_t) (map->read_b(addr + 2, map->priv)) << 16) | ((uint32_t) (map->read_b(addr + 3, map->priv)) << 24);

    return 0xffffffff;
}

/* Write a long to memory without MMU translation - results of previous MMU translation passed as array. */
void
writememll_no_mmut(uint32_t addr, uint32_t *a64, uint32_t val)
{
    mem_mapping_t *map;

    IDLE_NOTE(addr, 4, 1, val);
    GDBSTUB_MEM_ACCESS(addr, GDBSTUB_MEM_WRITE, 4);

    mem_logical_addr = addr;

    if (addr & 3) {
        if (!cpu_cyrix_alignment || (addr & 7) > 4)
            cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xffc) {
            if (cr0 >> 31) {
                if (cpu_state.abrt || high_page)
                    return;
            }

            writememwl_no_mmut(addr, &(a64[0]), val);
            writememwl_no_mmut(addr + 2, &(a64[2]), val >> 16);
            return;
        } else if (writelookup2[addr >> 12] != (uintptr_t) LOOKUP_INV) {
            *(uint32_t *) (writelookup2[addr >> 12] + addr) = val;
            return;
        }
    }

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_l) {
        page_lookup[addr >> 12]->write_l(addr, val, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        if (cpu_state.abrt || high_page)
            return;

        addr = (uint32_t) (a64[0] & rammask);
    } else
        addr &= rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
        map->write_l(addr, val, map->priv);
        return;
    }
    if (map && map->write_w) {
        map->write_w(addr, val, map->priv);
        map->write_w(addr + 2, val >> 16, map->priv);
        return;
    }
    if (map && map->write_b) {
        map->write_b(addr, val, map->priv);
        map->write_b(addr + 1, val >> 8, map->priv);
        map->write_b(addr + 2, val >> 16, map->priv);
        map->write_b(addr + 3, val >> 24, map->priv);
        return;
    }
}

uint64_t
readmemql(uint32_t addr)
{
    mem_mapping_t *map;
    int            i;
    uint64_t       a = 0x0000000000000000ULL;

    IDLE_NOTE(addr, 8, 0, 0);
    for (i = 0; i < 8; i++) {
        addr64a[i] = (uint64_t) (addr + i);
#ifdef USE_DEBUG_REGS_486
        mem_debug_check_addr(addr + i, read_type);
#endif
    }
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_READ, 8);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 7) {
        uint32_t       large_offset = is_compare ? 1048576 : 0;
        uintptr_t     *rl2 = &(readlookup2[large_offset | (addr >> 12)]);

        cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xff8) {
            if (cr0 >> 31) {
                for (i = 0; i < 8; i++) {
                    if (i == 0) {
                        a          = mmutranslate_read(addr + i);
                        addr64a[i] = (uint32_t) a;
                    } else if (!((addr + i) & 0xfff)) {
                        a          = mmutranslate_read(addr + 7);
                        addr64a[i] = (uint32_t) a;
                        if (!cpu_state.abrt) {
                            a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                            addr64a[i] = (uint32_t) a;
                        }
                    } else {
                        a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                        addr64a[i] = (uint32_t) a;
                    }

                    if (a > 0xffffffffULL)
                        return 0xffff;
                }
            }

            /* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
               their result as a parameter to be used if needed. */
            return readmemll_no_mmut(addr, addr64a) | (((uint64_t) readmemll_no_mmut(addr + 4, &(addr64a[4]))) << 32);
        } else if (*rl2 != (uintptr_t) LOOKUP_INV)
            return *(uint64_t *) (*rl2 + addr);
    }

    if (cr0 >> 31) {
        a          = mmutranslate_read(addr);
        addr64a[0] = (uint32_t) a;

        if (a > 0xffffffffULL)
            return 0xffffffffffffffffULL;
    }

    addr = addr64a[0] & rammask;

    map = read_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->read_l)
        return map->read_l(addr, map->priv) |
               ((uint64_t) map->read_l(addr + 4, map->priv) << 32);

    if (map && map->read_w)
        return map->read_w(addr, map->priv) |
               ((uint64_t) map->read_w(addr + 2, map->priv) << 16) |
               ((uint64_t) map->read_w(addr + 4, map->priv) << 32) |
               ((uint64_t) map->read_w(addr + 6, map->priv) << 48);

    if (map && map->read_b)
        return map->read_b(addr, map->priv) |
               ((uint64_t) map->read_b(addr + 1, map->priv) << 8) |
               ((uint64_t) map->read_b(addr + 2, map->priv) << 16) |
               ((uint64_t) map->read_b(addr + 3, map->priv) << 24) |
               ((uint64_t) map->read_b(addr + 4, map->priv) << 32) |
               ((uint64_t) map->read_b(addr + 5, map->priv) << 40) |
               ((uint64_t) map->read_b(addr + 6, map->priv) << 48) |
               ((uint64_t) map->read_b(addr + 7, map->priv) << 56);

    return 0xffffffffffffffffULL;
}

void
writememql(uint32_t addr, uint64_t val)
{
    mem_mapping_t *map;
    int            i;
    uint64_t       a = 0x0000000000000000ULL;

    IDLE_NOTE(addr, 8, 1, val);
    for (i = 0; i < 8; i++) {
        addr64a[i] = (uint64_t) (addr + i);
#ifdef USE_DEBUG_REGS_486
        mem_debug_check_addr(addr + i, 2);
#endif
    }
    GDBSTUB_MEM_ACCESS_FAST(addr64a, GDBSTUB_MEM_WRITE, 8);

    mem_logical_addr = addr;

    high_page = 0;

    if (addr & 7) {
        cycles -= timing_misaligned;
        if ((addr & 0xfff) > 0xff8) {
            if (cr0 >> 31) {
                for (i = 0; i < 8; i++) {
                    /* A page with a valid lookup is already translated: take its physical
                       address from the lookup, since writing the first part can recycle
                       the entry and the rest then falls back to addr64a[]. */
                    if (page_lookup[(addr + i) >> 12] && page_lookup[(addr + i) >> 12]->write_b) {
                        a          = ((uint64_t) (page_lookup[(addr + i) >> 12] - pages) << 12) | ((addr + i) & 0xfff);
                        addr64a[i] = (uint32_t) a;
                    } else {
                        if (i == 0) {
                            a          = mmutranslate_write(addr + i);
                            addr64a[i] = (uint32_t) a;
                        } else if (!((addr + i) & 0xfff)) {
                            a          = mmutranslate_write(addr + 7);
                            addr64a[i] = (uint32_t) a;
                            if (!cpu_state.abrt) {
                                a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                                addr64a[i] = (uint32_t) a;
                            }
                        } else {
                            a          = (a & ~0xfffLL) | ((uint64_t) ((addr + i) & 0xfff));
                            addr64a[i] = (uint32_t) a;
                        }

                        if (addr64a[i] > 0xffffffffULL)
                            return;
                    }
                }
            }

            /* No need to waste precious CPU host cycles on mmutranslate's that were already done, just pass
               their result as a parameter to be used if needed. */
            writememll_no_mmut(addr, addr64a, val);
            writememll_no_mmut(addr + 4, &(addr64a[4]), val >> 32);
            return;
        } else if (writelookup2[addr >> 12] != (uintptr_t) LOOKUP_INV) {
            *(uint64_t *) (writelookup2[addr >> 12] + addr) = val;
            return;
        }
    }

    if (page_lookup[addr >> 12] && page_lookup[addr >> 12]->write_l) {
        page_lookup[addr >> 12]->write_l(addr, val, page_lookup[addr >> 12]);
        page_lookup[addr >> 12]->write_l(addr + 4, val >> 32, page_lookup[addr >> 12]);
        return;
    }

    if (cr0 >> 31) {
        addr64a[0] = mmutranslate_write(addr);
        if (addr64a[0] > 0xffffffffULL)
            return;
    }

    addr = addr64a[0] & rammask;

    map = write_mapping[addr >> MEM_GRANULARITY_BITS];

    if (map && map->write_l) {
        map->write_l(addr, val, map->priv);
        map->write_l(addr + 4, val >> 32, map->priv);
        return;
    }
    if (map && map->write_w) {
        map->write_w(addr, val, map->priv);
        map->write_w(addr + 2, val >> 16, map->priv);
        map->write_w(addr + 4, val >> 32, map->priv);
        map->write_w(addr + 6, val >> 48, map->priv);
        return;
    }
    if (map && map->write_b) {
        map->write_b(addr, val, map->priv);
        map->write_b(addr + 1, val >> 8, map->priv);
        map->write_b(addr + 2, val >> 16, map->priv);
        map->write_b(addr + 3, val >> 24, map->priv);
        map->write_b(addr + 4, val >> 32, map->priv);
        map->write_b(addr + 5, val >> 40, map->priv);
        map->write_b(addr + 6, val >> 48, map->priv);
        map->write_b(addr + 7, val >> 56, map->priv);
        return;
    }
}

void
do_mmutranslate(uint32_t addr, uint32_t *a64, int num, int write)
{
    int      i;
    int      cond = 1;
    uint32_t last_addr = addr + (num - 1);
    uint64_t a         = 0x0000000000000000ULL;

    for (i = 0; i < num; i++) {
        a64[i] = (uint64_t) (addr + i);
#ifdef USE_DEBUG_REGS_486
        mem_debug_check_addr(addr + i, write ? 2 : read_type);
#endif
    }

    if (cr0 >> 31)  for (i = 0; i < num; i++) {
        if (write && ((i == 0) || !(addr & 0xfff)))
            cond = (!page_lookup[addr >> 12] || !page_lookup[addr >> 12]->write_b);

        if (cond) {
            /* If we have encountered at least one page fault, mark all subsequent addresses as
               having page faulted, prevents false negatives in readmem*l_no_mmut. */
            if ((i > 0) && cpu_state.abrt && !high_page)
                a64[i] = a64[i - 1];
            /* If we are on the same page, there is no need to translate again, as we can just
               reuse the previous result. */
            else if (i == 0) {
                a      = mmutranslatereal(addr, write);
                a64[i] = (uint32_t) a;

                high_page = high_page || (!cpu_state.abrt && (a > 0xffffffffULL));
            } else if (!(addr & 0xfff)) {
                a      = mmutranslatereal(last_addr, write);
                a64[i] = (uint32_t) a;

                high_page = high_page || (!cpu_state.abrt && (a > 0xffffffffULL));

                if (!cpu_state.abrt) {
                    a      = (a & 0xfffffffffffff000ULL) | ((uint64_t) (addr & 0xfff));
                    a64[i] = (uint32_t) a;
                }
            } else {
                a      = (a & 0xfffffffffffff000ULL) | ((uint64_t) (addr & 0xfff));
                a64[i] = (uint32_t) a;
            }
        }

        addr++;
    }
}

/* Put back what a fresh map needs from this side. */
void
mem_cpu_reset(void)
{
    /* Reads of absent physical memory return floating high bits - see rammap() above. */
    memset(mem_unbacked_pt_page, 0xff, sizeof(mem_unbacked_pt_page));
}

/* Which view of the map this processor's accesses go through. The caller
   empties its TLB: entries filled through the other view must not
   survive. */
void
mem_smm_view(int smm)
{
    /* What addresses reach changes: no link holds. */
    LINKS_DROP_OWNED();
    read_mapping  = mem_read_mapping_view[!!smm];
    write_mapping = mem_write_mapping_view[!!smm];
    _mem_exec     = mem_exec_view[!!smm];
}
