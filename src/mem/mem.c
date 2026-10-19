/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Memory handling and MMU.
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

mem_mapping_t ram_low_mapping;       /* 0..640K mapping */
mem_mapping_t ram_mid_mapping;       /* 640..1024K mapping */
mem_mapping_t ram_mid_mapping2;      /* 640..1024K mapping, second part, for SiS 471 in relocate mode  */
mem_mapping_t ram_remapped_mapping;  /* 640..1024K mapping */
mem_mapping_t ram_remapped_mapping2; /* 640..1024K second mapping, for SiS 471 mode */
mem_mapping_t ram_high_mapping;      /* 1024K+ mapping */
mem_mapping_t ram_split_mapping;
mem_mapping_t bios_mapping;
mem_mapping_t bios_high_mapping;

uint32_t pages_sz;    /* #pages in table */

uint8_t *ram;  /* the virtual RAM */
uint8_t  page_ff[4096];
uint32_t rammask;

uint32_t addr_space_size;

uint8_t *rom; /* the virtual ROM */
uint32_t biosmask;
uint32_t biosaddr;

int shadowbios = 0;
int shadowbios_write;

/* Chains of linked blocks: what a link stub tests (mem.h). Set by the
   hooks in every build; only the new recompiler reads it. */
mem_chain_signal_t mem_chain_signal;

int mem_a20_key     = 0;
int mem_a20_alt     = 0;
int mem_a20_chipset = 0;
int mem_a20_state   = 0;

/* The masks cover RAM only; every page beyond it shares these, in every
   copy of the page table. Nothing writes there through the RAM handlers,
   so they only ever collect the code-present bits of blocks run from ROM
   above RAM. */
static uint64_t byte_dirty_mask_none[64];
static uint64_t byte_code_present_mask_none[64];

void
mem_page_set_byte_masks(page_t *p, uint32_t page_nr)
{
    if (byte_dirty_mask && (page_nr < (mem_size >> 2))) {
        p->byte_dirty_mask        = &byte_dirty_mask[page_nr * 64];
        p->byte_code_present_mask = &byte_code_present_mask[page_nr * 64];
    } else {
        p->byte_dirty_mask        = byte_dirty_mask_none;
        p->byte_code_present_mask = byte_code_present_mask_none;
    }
}

/* Every copy of the page table. The table carries a code cache's
   bookkeeping (blocks, purge list, code-present and dirty masks) beside
   the machine's half of each entry (the memory behind it, its write
   handlers), and each processor core has a code cache of its own, so it
   keeps a table of its own (mem_cpu.c). Copy 0's is `pages`, which the
   machine's code writes; the others follow its machine half
   (mem_pages_mirror), and a write that dirties a page dirties it in each.
   With one copy a write takes the path it always has. */
static mem_page_copy_t mem_page_copies[MEM_PAGE_COPIES_MAX];
int                    mem_page_copy_count;

static void mem_pages_alloc_copy(int i);
static void mem_pages_free_copy(int i);

void
mem_page_copy_register(const mem_page_copy_t *c)
{
    for (int i = 0; i < mem_page_copy_count; i++)
        if (mem_page_copies[i].pages == c->pages)
            return;

    if (mem_page_copy_count >= MEM_PAGE_COPIES_MAX)
        fatal("mem_page_copy_register: too many copies\n");

    mem_page_copies[mem_page_copy_count++] = *c;

    /* Joining a machine already built: its table now. */
    if ((mem_page_copy_count > 1) && (pages != NULL) && (*c->pages == NULL)) {
        mem_pages_alloc_copy(mem_page_copy_count - 1);
        mem_pages_mirror(0, pages_sz, 1);
    }
}

void
mem_page_copy_unregister(page_t **copy_pages)
{
    for (int i = 1; i < mem_page_copy_count; i++) {
        if (mem_page_copies[i].pages == copy_pages) {
            mem_pages_free_copy(i);
            mem_page_copies[i] = mem_page_copies[--mem_page_copy_count];
            break;
        }
    }
}

/* Whether a core other than the one whose table is `mine` has compiled
   code on page `idx`: a write there takes the tracked path in every core,
   or the other core's code never sees it change. */
int
mem_page_code_in_other_copy(uint32_t idx, const page_t *mine)
{
    for (int i = 0; i < mem_page_copy_count; i++) {
        const page_t *base = *mem_page_copies[i].pages;

        if ((base == NULL) || (base == mine) || (idx >= pages_sz))
            continue;
        if (base[idx].block || base[idx].block_2)
            return 1;
    }
    return 0;
}

/* Which copy a page entry is in. */
static int
mem_page_copy_of(const page_t *page)
{
    for (int i = 0; i < mem_page_copy_count; i++) {
        const page_t *base = *mem_page_copies[i].pages;

        if ((base != NULL) && (page >= base) && (page < (base + pages_sz)))
            return i;
    }
    return 0;
}

/* Put a page on its own copy's list of pages to purge. */
static __inline void
mem_page_add_evict(page_t *page)
{
    if (mem_page_copy_count <= 1)
        page_add_to_evict_list(page);
    else
        mem_page_copies[mem_page_copy_of(page)].add_to_evict_list(page);
}

#ifdef CODEGEN_CHAIN
/* Whether a write with these masks reached compiled code on the page. */
static __inline int
mem_page_code_reached(const page_t *p, uint64_t mask, int byte_offset, uint64_t bm0, uint64_t bm1)
{
    if (!p->block && !p->block_2)
        return 0;
    return (p->code_present_mask & mask) || (p->byte_code_present_mask[byte_offset] & bm0) ||
           (bm1 && (p->byte_code_present_mask[byte_offset + 1] & bm1));
}

/* A write changed bytes of the page: if it reached compiled code the
   core whose table the entry is in has linked to, those links go
   (codegen_page_code_hit), as only a link skips the dirty test a dispatch
   makes. */
static __inline void
mem_page_code_written(const page_t *p, uint64_t mask, int byte_offset, uint64_t bm0, uint64_t bm1)
{
    if (!mem_page_code_reached(p, mask, byte_offset, bm0, bm1))
        return;
    if (mem_page_copy_count <= 1)
        codegen_page_code_hit(p, mask, byte_offset, bm0, bm1);
    else
        mem_page_copies[mem_page_copy_of(p)].code_hit(p, mask, byte_offset, bm0, bm1);
}
#endif

/* The same bits a write just set in one copy, set in the others. */
static void
mem_page_mark_others(const page_t *page, uint64_t mask, int byte_offset, uint64_t bm0, uint64_t bm1)
{
    const int      own = mem_page_copy_of(page);
    const uint32_t idx = (uint32_t) (page - *mem_page_copies[own].pages);

    for (int i = 0; i < mem_page_copy_count; i++) {
        page_t *o;

        if ((i == own) || (*mem_page_copies[i].pages == NULL))
            continue;

        o = &(*mem_page_copies[i].pages)[idx];
        o->dirty_mask |= mask;
        o->byte_dirty_mask[byte_offset] |= bm0;
        if (bm1)
            o->byte_dirty_mask[byte_offset + 1] |= bm1;
        if (!page_in_evict_list(o) &&
            ((o->code_present_mask & mask) || (o->byte_code_present_mask[byte_offset] & bm0) ||
             (bm1 && (o->byte_code_present_mask[byte_offset + 1] & bm1))))
            mem_page_copies[i].add_to_evict_list(o);
#ifdef CODEGEN_CHAIN
        if (mem_page_code_reached(o, mask, byte_offset, bm0, bm1))
            mem_page_copies[i].code_hit(o, mask, byte_offset, bm0, bm1);
#endif
    }
}

/* Where copy 0's byte mask `mask` is, in a copy whose masks start at
   `copy`: the same RAM page's, or the ones every page beyond RAM shares. */
static uint64_t *
mem_page_mask_in(uint64_t *mask, const uint64_t *base, uint64_t *copy)
{
    if ((base != NULL) && (mask >= base) && (mask < (base + ((size_t) (mem_size >> 2) * 64))))
        return copy + (mask - base);
    return mask;
}

/* Copy 0's machine half of entries [first, first + count) into the other
   copies: the memory behind each, its write handlers and which RAM page's
   byte masks it uses. reset_evict takes them off their purge lists, as
   the machine's code does to copy 0 when it rebuilds entries. */
void
mem_pages_mirror(uint32_t first, uint32_t count, int reset_evict)
{
    for (int i = 1; i < mem_page_copy_count; i++) {
        page_t   *d  = *mem_page_copies[i].pages;
        uint64_t *bd = *mem_page_copies[i].byte_dirty_mask;
        uint64_t *bc = *mem_page_copies[i].byte_code_present_mask;

        if (d == NULL)
            continue;

        for (uint32_t c = first; (c < (first + count)) && (c < pages_sz); c++) {
            d[c].mem     = pages[c].mem;
            d[c].write_b = pages[c].write_b;
            d[c].write_w = pages[c].write_w;
            d[c].write_l = pages[c].write_l;
            if (pages[c].byte_dirty_mask)
                d[c].byte_dirty_mask = mem_page_mask_in(pages[c].byte_dirty_mask, byte_dirty_mask, bd);
            if (pages[c].byte_code_present_mask)
                d[c].byte_code_present_mask = mem_page_mask_in(pages[c].byte_code_present_mask,
                                                               byte_code_present_mask, bc);
            if (reset_evict)
                d[c].evict_prev = EVICT_NOT_IN_LIST;
        }
    }
}

/* The copies other than 0 get tables of their own on every reset. */
static void
mem_pages_free_copy(int i)
{
    free(*mem_page_copies[i].pages);
    free(*mem_page_copies[i].byte_dirty_mask);
    free(*mem_page_copies[i].byte_code_present_mask);
    *mem_page_copies[i].pages                  = NULL;
    *mem_page_copies[i].byte_dirty_mask        = NULL;
    *mem_page_copies[i].byte_code_present_mask = NULL;
}

static void
mem_pages_free_copies(void)
{
    for (int i = 1; i < mem_page_copy_count; i++)
        mem_pages_free_copy(i);
}

static void
mem_pages_alloc_copy(int i)
{
    page_t *d;

    *mem_page_copies[i].pages                  = (page_t *) calloc(pages_sz, sizeof(page_t));
    *mem_page_copies[i].byte_dirty_mask        = calloc(1, ((size_t) mem_size * 1024) / 8);
    *mem_page_copies[i].byte_code_present_mask = calloc(1, ((size_t) mem_size * 1024) / 8);
    if ((*mem_page_copies[i].pages == NULL) || (*mem_page_copies[i].byte_dirty_mask == NULL) ||
        (*mem_page_copies[i].byte_code_present_mask == NULL))
        fatal("mem_pages_alloc_copy: out of memory\n");

    /* Its own half: no blocks, off the purge list. */
    d = *mem_page_copies[i].pages;
    for (uint32_t c = 0; c < pages_sz; c++) {
        d[c].block      = BLOCK_INVALID;
        d[c].block_2    = BLOCK_INVALID;
        d[c].head       = BLOCK_INVALID;
        d[c].evict_prev = EVICT_NOT_IN_LIST;
    }
    mem_page_copies[i].reset_evict_list();
}

static void
mem_pages_alloc_copies(void)
{
    for (int i = 1; i < mem_page_copy_count; i++)
        mem_pages_alloc_copy(i);
    mem_pages_mirror(0, pages_sz, 1);
}

/* What a processor sees, twice over: [0] outside SMM, [1] inside it (or
   with a Cyrix's SMAC set). The two differ only where the chipset decodes
   SMRAM, and every map change fills both, so entering or leaving SMM
   selects the other view instead of rebuilding the map -- and two
   processors, one in SMM and one not, each see their own. The view in use
   is the processor's own (mem_cpu.c, mem_smm_view). */
mem_mapping_t *mem_read_mapping_view[2][MEM_MAPPINGS_NO];
mem_mapping_t *mem_write_mapping_view[2][MEM_MAPPINGS_NO];
uint8_t       *mem_exec_view[2][MEM_MAPPINGS_NO];

/* Processors in SMM, one bit each. Bus masters see SMRAM as the chipset
   decodes it with SMIACT# asserted, which it is while any processor is in
   SMM. */
static uint32_t mem_smm_cpus;

/* Moves on whenever the map is rebuilt: a pointer mem_phys_ptr gave holds
   while it is unchanged. */
uint32_t mem_mapping_gen;

static mem_mapping_t *base_mapping;
static mem_mapping_t *last_mapping;
static mem_mapping_t *read_mapping_bus[MEM_MAPPINGS_NO];
static mem_mapping_t *write_mapping_bus[MEM_MAPPINGS_NO];
static uint8_t       _mem_wp[MEM_MAPPINGS_NO];
static uint8_t       _mem_wp_bus[MEM_MAPPINGS_NO];
static mem_state_t    _mem_state[MEM_MAPPINGS_NO];
static uint32_t       remap_start_addr;
static uint32_t       remap_start_addr2;
static size_t         ram_size = 0;

/* The bytes of guest RAM behind ram[]. */
size_t
mem_ram_size(void)
{
    return ram_size;
}

#ifdef ENABLE_MEM_LOG
int mem_do_log = ENABLE_MEM_LOG;

static void
mem_log(const char *fmt, ...)
{
    va_list ap;

    if (mem_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define mem_log(fmt, ...)
#endif

/* Every processor's TLB set, so a flush in one reaches the others. */
mem_tlb_t *mem_tlbs[MEM_TLB_MAX];
int        mem_tlb_count;

void
mem_tlb_clear(mem_tlb_t *t)
{
    /* Initialize the page lookup table. */
    memset(t->page_lookup, 0x00, (1 << 20) * sizeof(page_t *));

    /* Initialize the rings. */
    for (uint16_t c = 0; c < 512; c++) {
        t->readlookup[c] = 0xffffffff;

        if (c < 256)
            t->writelookup[c] = 0xffffffff;
    }

    /* Initialize the tables. */
    memset(t->readlookup2, 0xff, (2 << 20) * sizeof(uintptr_t));
    memset(t->writelookup2, 0xff, (1 << 20) * sizeof(uintptr_t));

    t->readlnext[0] = 0;
    t->readlnext[1] = 0;
    t->writelnext   = 0;
    t->stale        = 0;
    memset(t->large_regions, 0, sizeof(t->large_regions));
    if (t->ubank != NULL)
        mem_tlb_clear(t->ubank);
}

/* A set joins the ones every flush reaches. */
void
mem_tlb_register(mem_tlb_t *t)
{
    for (int i = 0; i < mem_tlb_count; i++)
        if (mem_tlbs[i] == t)
            return;

    if (mem_tlb_count >= MEM_TLB_MAX)
        fatal("mem_tlb_register: too many sets\n");

    mem_tlbs[mem_tlb_count++] = t;
}

/* Another processor's TLB: empty, and flushed along with the others from
   now on. */
mem_tlb_t *
mem_tlb_create(void)
{
    mem_tlb_t *t = calloc(1, sizeof(mem_tlb_t));

    if (t == NULL)
        fatal("mem_tlb_create: out of memory\n");
    t->page_lookup  = malloc((1 << 20) * sizeof(page_t *));
    t->readlookup2  = malloc((2 << 20) * sizeof(uintptr_t));
    t->writelookup2 = malloc((1 << 20) * sizeof(uintptr_t));
    if ((t->page_lookup == NULL) || (t->readlookup2 == NULL) || (t->writelookup2 == NULL))
        fatal("mem_tlb_create: out of memory\n");
    mem_tlb_clear(t);

    mem_tlb_register(t);
    return t;
}

void
mem_tlb_destroy(mem_tlb_t *t)
{
    if ((t == NULL) || (t == mem_tlb_default()))
        return;

    if (t == mem_tlb_current())
        mem_tlb_select(mem_tlb_default());

    for (int i = 0; i < mem_tlb_count; i++) {
        if (mem_tlbs[i] == t) {
            mem_tlbs[i] = mem_tlbs[--mem_tlb_count];
            break;
        }
    }

    mem_tlb_bank_destroy(t->ubank);
    free(t->page_lookup);
    free(t->readlookup2);
    free(t->writelookup2);
    free(t);
}

/* A flush that reaches every set: the one in place is emptied by the
   caller, the others when they are next in place (mem_tlb_select). */
void
mem_tlb_mark_others(const mem_tlb_t *except, uint8_t level)
{
    for (int i = 0; i < mem_tlb_count; i++) {
        if (mem_tlbs[i] != except)
            mem_tlbs[i]->stale |= level;
    }
}

uint8_t
mem_addr_range_match(uint32_t addr, uint32_t start, uint32_t len)
{
    if (addr < start)
        return 0;
    else if (addr >= (start + len))
        return 0;
    else
        return 1;
}

uint32_t
mem_addr_translate(uint32_t addr, uint32_t chunk_start, uint32_t len)
{
    uint32_t mask = len - 1;

    return chunk_start + (addr & mask);
}

/* Where the len bytes at addr are, as the processor would read them, when
   they lie in one granule of plain memory (RAM or ROM behind no handler);
   NULL otherwise, for the caller to read them one at a time. */
const uint8_t *
mem_phys_ptr(uint32_t addr, uint32_t len)
{
    const mem_mapping_t *map = read_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint32_t             off;

    if (!len || (len > MEM_GRANULARITY_SIZE) || !cpu_use_exec || (map == NULL) || (map->exec == NULL) ||
        (((addr & MEM_GRANULARITY_MASK) + len) > MEM_GRANULARITY_SIZE))
        return NULL;
    off = (addr - map->base) & map->mask;
    if ((((addr + len - 1) - map->base) & map->mask) != (off + len - 1))
        return NULL; /* a small mirrored mapping wraps inside the granule */
    return &map->exec[off];
}

uint8_t
mem_readb_phys(uint32_t addr)
{
    mem_mapping_t *map = read_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint8_t        ret = 0xff;

    mem_logical_addr = 0xffffffff;

    if (map) {
        if (cpu_use_exec && map->exec)
            ret = map->exec[(addr - map->base) & map->mask];
        else if (map->read_b)
            ret = map->read_b(addr, map->priv);
    }

    return ret;
}

uint16_t
mem_readw_phys(uint32_t addr)
{
    mem_mapping_t  *map = read_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint16_t        ret;
    const uint16_t *p;

    mem_logical_addr = 0xffffffff;

    if (cpu_use_exec && ((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_HBOUND) && (map && map->exec)) {
        p   = (uint16_t *) &(map->exec[(addr - map->base) & map->mask]);
        ret = *p;
    } else if (((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_HBOUND) && (map && map->read_w))
        ret = map->read_w(addr, map->priv);
    else {
        ret = mem_readb_phys(addr + 1) << 8;
        ret |= mem_readb_phys(addr);
    }

    return ret;
}

uint32_t
mem_readl_phys(uint32_t addr)
{
    mem_mapping_t  *map = read_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint32_t        ret;
    const uint32_t *p;

    mem_logical_addr = 0xffffffff;

    if (cpu_use_exec && ((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_QBOUND) && (map && map->exec)) {
        p   = (uint32_t *) &(map->exec[(addr - map->base) & map->mask]);
        ret = *p;
    } else if (((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_QBOUND) && (map && map->read_l))
        ret = map->read_l(addr, map->priv);
    else {
        ret = mem_readw_phys(addr + 2) << 16;
        ret |= mem_readw_phys(addr);
    }

    return ret;
}

void
mem_read_phys(void *dest, uint32_t addr, int transfer_size)
{
    uint8_t  *pb;
    uint16_t *pw;
    uint32_t *pl;

    if (transfer_size == 4) {
        pl  = (uint32_t *) dest;
        *pl = mem_readl_phys(addr);
    } else if (transfer_size == 2) {
        pw  = (uint16_t *) dest;
        *pw = mem_readw_phys(addr);
    } else if (transfer_size == 1) {
        pb  = (uint8_t *) dest;
        *pb = mem_readb_phys(addr);
    }
}

void
mem_writeb_phys(uint32_t addr, uint8_t val)
{
    mem_mapping_t *map = write_mapping_bus[addr >> MEM_GRANULARITY_BITS];

    mem_logical_addr = 0xffffffff;

    if (map) {
        if (cpu_use_exec && map->exec)
            map->exec[(addr - map->base) & map->mask] = val;
        else if (map->write_b)
            map->write_b(addr, val, map->priv);
    }
}

void
mem_writew_phys(uint32_t addr, uint16_t val)
{
    mem_mapping_t *map = write_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint16_t      *p;

    mem_logical_addr = 0xffffffff;

    if (cpu_use_exec && ((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_HBOUND) && (map && map->exec)) {
        p  = (uint16_t *) &(map->exec[(addr - map->base) & map->mask]);
        *p = val;
    } else if (((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_HBOUND) && (map && map->write_w))
        map->write_w(addr, val, map->priv);
    else {
        mem_writeb_phys(addr, val & 0xff);
        mem_writeb_phys(addr + 1, (val >> 8) & 0xff);
    }
}

void
mem_writel_phys(uint32_t addr, uint32_t val)
{
    mem_mapping_t *map = write_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    uint32_t      *p;

    mem_logical_addr = 0xffffffff;

    if (cpu_use_exec && ((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_QBOUND) && (map && map->exec)) {
        p  = (uint32_t *) &(map->exec[(addr - map->base) & map->mask]);
        *p = val;
    } else if (((addr & MEM_GRANULARITY_MASK) <= MEM_GRANULARITY_QBOUND) && (map && map->write_l))
        map->write_l(addr, val, map->priv);
    else {
        mem_writew_phys(addr, val & 0xffff);
        mem_writew_phys(addr + 2, (val >> 16) & 0xffff);
    }
}

void
mem_write_phys(void *src, uint32_t addr, int transfer_size)
{
    const uint8_t  *pb;
    const uint16_t *pw;
    const uint32_t *pl;

    if (transfer_size == 4) {
        pl = (uint32_t *) src;
        mem_writel_phys(addr, *pl);
    } else if (transfer_size == 2) {
        pw = (uint16_t *) src;
        mem_writew_phys(addr, *pw);
    } else if (transfer_size == 1) {
        pb = (uint8_t *) src;
        mem_writeb_phys(addr, *pb);
    }
}

uint8_t
mem_read_ram(uint32_t addr, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Read  B       %02X from %08X\n", ram[addr], addr);
#endif

    if (is_pcjr)
        pcjr_waitstates(NULL);

    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);

    return ram[addr];
}

uint16_t
mem_read_ramw(uint32_t addr, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Read  W     %04X from %08X\n", *(uint16_t *) &ram[addr], addr);
#endif

    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);

    return *(uint16_t *) &ram[addr];
}

uint32_t
mem_read_raml(uint32_t addr, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Read  L %08X from %08X\n", *(uint32_t *) &ram[addr], addr);
#endif

    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);

    return *(uint32_t *) &ram[addr];
}

void
mem_write_ramb_page(uint32_t addr, uint8_t val, page_t *page)
{
    if (page == NULL)
        return;

#    ifdef USE_DYNAREC
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != page->mem[addr & 0xfff]) || codegen_in_recompile) {
#    else
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != page->mem[addr & 0xfff])) {
#    endif
        uint64_t mask        = (uint64_t) 1 << ((addr >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK);
        int      byte_offset = (addr >> PAGE_BYTE_MASK_SHIFT) & PAGE_BYTE_MASK_OFFSET_MASK;
        uint64_t byte_mask   = (uint64_t) 1 << (addr & PAGE_BYTE_MASK_MASK);

        page->mem[addr & 0xfff] = val;
        page->dirty_mask |= mask;
        if ((page->code_present_mask & mask) && !page_in_evict_list(page))
            mem_page_add_evict(page);
        page->byte_dirty_mask[byte_offset] |= byte_mask;
        if ((page->byte_code_present_mask[byte_offset] & byte_mask) && !page_in_evict_list(page))
            mem_page_add_evict(page);
#    ifdef CODEGEN_CHAIN
        mem_page_code_written(page, mask, byte_offset, byte_mask, 0);
#    endif
        if (mem_page_copy_count > 1)
            mem_page_mark_others(page, mask, byte_offset, byte_mask, 0);
    }
}

void
mem_write_ramw_page(uint32_t addr, uint16_t val, page_t *page)
{
    if (page == NULL)
        return;

#    ifdef USE_DYNAREC
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != *(uint16_t *) &page->mem[addr & 0xfff]) || codegen_in_recompile) {
#    else
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != *(uint16_t *) &page->mem[addr & 0xfff])) {
#    endif
        uint64_t mask        = (uint64_t) 1 << ((addr >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK);
        int      byte_offset = (addr >> PAGE_BYTE_MASK_SHIFT) & PAGE_BYTE_MASK_OFFSET_MASK;
        uint64_t byte_mask   = (uint64_t) 1 << (addr & PAGE_BYTE_MASK_MASK);
        uint64_t byte_mask_2 = 0;

        if ((addr & 0xf) == 0xf)
            mask |= (mask << 1);
        *(uint16_t *) &page->mem[addr & 0xfff] = val;
        page->dirty_mask |= mask;
        if ((page->code_present_mask & mask) && !page_in_evict_list(page))
            mem_page_add_evict(page);
        if ((addr & PAGE_BYTE_MASK_MASK) == PAGE_BYTE_MASK_MASK) {
            byte_mask_2 = 1;
            page->byte_dirty_mask[byte_offset + 1] |= 1;
            if ((page->byte_code_present_mask[byte_offset + 1] & 1) && !page_in_evict_list(page))
                mem_page_add_evict(page);
        } else
            byte_mask |= (byte_mask << 1);

        page->byte_dirty_mask[byte_offset] |= byte_mask;

        if ((page->byte_code_present_mask[byte_offset] & byte_mask) && !page_in_evict_list(page))
            mem_page_add_evict(page);
#    ifdef CODEGEN_CHAIN
        mem_page_code_written(page, mask, byte_offset, byte_mask, byte_mask_2);
#    endif
        if (mem_page_copy_count > 1)
            mem_page_mark_others(page, mask, byte_offset, byte_mask, byte_mask_2);
    }
}

void
mem_write_raml_page(uint32_t addr, uint32_t val, page_t *page)
{
    if (page == NULL)
        return;

#    ifdef USE_DYNAREC
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != *(uint32_t *) &page->mem[addr & 0xfff]) || codegen_in_recompile) {
#    else
    if ((page->mem == NULL) || (page->mem == page_ff) || (val != *(uint32_t *) &page->mem[addr & 0xfff])) {
#    endif
        uint64_t mask        = (uint64_t) 1 << ((addr >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK);
        int      byte_offset = (addr >> PAGE_BYTE_MASK_SHIFT) & PAGE_BYTE_MASK_OFFSET_MASK;
        uint64_t byte_mask   = (uint64_t) 0xf << (addr & PAGE_BYTE_MASK_MASK);

        if ((addr & 0xf) >= 0xd)
            mask |= (mask << 1);
        *(uint32_t *) &page->mem[addr & 0xfff] = val;
        page->dirty_mask |= mask;
        page->byte_dirty_mask[byte_offset] |= byte_mask;
        if (!page_in_evict_list(page) && ((page->code_present_mask & mask) || (page->byte_code_present_mask[byte_offset] & byte_mask)))
            mem_page_add_evict(page);
        uint64_t byte_mask_2 = 0;
        if ((addr & PAGE_BYTE_MASK_MASK) > (PAGE_BYTE_MASK_MASK - 3)) {
            byte_mask_2 = 0xf >> (4 - (addr & 3));

            page->byte_dirty_mask[byte_offset + 1] |= byte_mask_2;
            if ((page->byte_code_present_mask[byte_offset + 1] & byte_mask_2) && !page_in_evict_list(page))
                mem_page_add_evict(page);
        }
#    ifdef CODEGEN_CHAIN
        mem_page_code_written(page, mask, byte_offset, byte_mask, byte_mask_2);
#    endif
        if (mem_page_copy_count > 1)
            mem_page_mark_others(page, mask, byte_offset, byte_mask, byte_mask_2);
    }
}

void
mem_write_ram(uint32_t addr, uint8_t val, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Write B       %02X to   %08X\n", val, addr);
#endif
    if (is_pcjr)
        pcjr_waitstates(NULL);

    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramb_page(addr, val, &pages[addr >> 12]);
    } else
        ram[addr] = val;
}

void
mem_write_ramw(uint32_t addr, uint16_t val, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Write W     %04X to   %08X\n", val, addr);
#endif
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramw_page(addr, val, &pages[addr >> 12]);
    } else
        *(uint16_t *) &ram[addr] = val;
}

void
mem_write_raml(uint32_t addr, uint32_t val, UNUSED(void *priv))
{
#ifdef ENABLE_MEM_LOG
    if ((addr >= 0xa0000) && (addr <= 0xbffff))
        mem_log("Write L %08X to   %08X\n", val, addr);
#endif
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_raml_page(addr, val, &pages[addr >> 12]);
    } else
        *(uint32_t *) &ram[addr] = val;
}

static uint8_t
mem_read_remapped(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return ram[addr];
}

static uint16_t
mem_read_remappedw(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return *(uint16_t *) &ram[addr];
}

static uint32_t
mem_read_remappedl(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return *(uint32_t *) &ram[addr];
}

static uint8_t
mem_read_remapped2(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return ram[addr];
}

static uint16_t
mem_read_remappedw2(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return *(uint16_t *) &ram[addr];
}

static uint32_t
mem_read_remappedl2(uint32_t addr, UNUSED(void *priv))
{
    addr = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec)
        addreadlookup(mem_logical_addr, addr);
    return *(uint32_t *) &ram[addr];
}

static void
mem_write_remapped(uint32_t addr, uint8_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramb_page(addr, val, &pages[oldaddr >> 12]);
    } else
        ram[addr] = val;
}

static void
mem_write_remappedw(uint32_t addr, uint16_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramw_page(addr, val, &pages[oldaddr >> 12]);
    } else
        *(uint16_t *) &ram[addr] = val;
}

static void
mem_write_remappedl(uint32_t addr, uint32_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xA0000 + (addr - remap_start_addr);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_raml_page(addr, val, &pages[oldaddr >> 12]);
    } else
        *(uint32_t *) &ram[addr] = val;
}

static void
mem_write_remapped2(uint32_t addr, uint8_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramb_page(addr, val, &pages[oldaddr >> 12]);
    } else
        ram[addr] = val;
}

static void
mem_write_remappedw2(uint32_t addr, uint16_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_ramw_page(addr, val, &pages[oldaddr >> 12]);
    } else
        *(uint16_t *) &ram[addr] = val;
}

static void
mem_write_remappedl2(uint32_t addr, uint32_t val, UNUSED(void *priv))
{
    uint32_t oldaddr = addr;
    addr             = 0xD0000 + (addr - remap_start_addr2);
    if (cpu_use_exec) {
        addwritelookup(mem_logical_addr, addr);
        mem_write_raml_page(addr, val, &pages[oldaddr >> 12]);
    } else
        *(uint32_t *) &ram[addr] = val;
}

void
mem_invalidate_range(uint32_t start_addr, uint32_t end_addr)
{
    start_addr &= 0xfffff000;
    end_addr &= 0xfffff000;

    page_t *page;

    for (; start_addr <= end_addr; start_addr += 0x1000) {
        if ((start_addr >> 12) >= pages_sz)
            break;

        /* In every copy of the page table: each core's code there. */
        for (int i = 0; i < mem_page_copy_count; i++) {
            if (*mem_page_copies[i].pages == NULL)
                continue;

            page             = &(*mem_page_copies[i].pages)[start_addr >> 12];
            page->dirty_mask = 0xffffffffffffffffULL;

            if ((page->mem != page_ff) && page->byte_dirty_mask)
                memset(page->byte_dirty_mask, 0xff, 64 * sizeof(uint64_t));

            if (!page_in_evict_list(page))
                mem_page_copies[i].add_to_evict_list(page);
#    ifdef CODEGEN_CHAIN
            if (page->block || page->block_2)
                mem_page_copies[i].code_drop(page);
#    endif
        }
    }
}

static __inline int
mem_mapping_access_allowed(uint32_t flags, uint16_t access)
{
    int ret = 0;

    if (!(access & ACCESS_DISABLED)) {
        if (access & ACCESS_CACHE)
            ret = (flags & MEM_MAPPING_CACHE);
        else if (access & ACCESS_SMRAM)
            ret = (flags & MEM_MAPPING_SMRAM);
        else if (!(access & ACCESS_INTERNAL)) {
            if (flags & MEM_MAPPING_IS_ROM) {
                if (access & ACCESS_ROMCS)
                    ret = (flags & MEM_MAPPING_ROMCS);
                else
                    ret = !(flags & MEM_MAPPING_ROMCS);
            } else
                ret = 1;

            ret = ret && !(flags & MEM_MAPPING_INTERNAL) && !(flags & MEM_MAPPING_SMRAM);
        } else
            ret = !(flags & MEM_MAPPING_EXTERNAL) && !(flags & MEM_MAPPING_SMRAM);
    } else {
        /* Still allow SMRAM if access is DISABLED but also has CACHE and/or SMRAM flags set. */
        if (access & ACCESS_CACHE)
            ret = (flags & MEM_MAPPING_CACHE);
        else if (access & ACCESS_SMRAM)
            ret = (flags & MEM_MAPPING_SMRAM);
    }

    return ret;
}

/* One granule of one mapping, put where an access of each kind will find it:
   both processor views, and the bus. */
static void
mem_mapping_apply_granule(mem_mapping_t *map, uint64_t c)
{
    uint32_t g  = (uint32_t) (c >> MEM_GRANULARITY_BITS);
    uint8_t  wp = _mem_wp[g];
    int      n;

    for (n = 0; n < 2; n++) {
        if (map->exec && mem_mapping_access_allowed(map->flags, _mem_state[g].states[n].x))
            mem_exec_view[n][g] = map->exec + (c - map->base);
        if (!wp && (map->write_b || map->write_w || map->write_l) && mem_mapping_access_allowed(map->flags, _mem_state[g].states[n].w))
            mem_write_mapping_view[n][g] = map;
        if ((map->read_b || map->read_w || map->read_l) && mem_mapping_access_allowed(map->flags, _mem_state[g].states[n].r))
            mem_read_mapping_view[n][g] = map;
    }

    /* A bus master sees SMIACT# as the processor that last owned the bus
       left it ("It does not float during bus HOLD", Pentium Vol. 1,
       5.1.61). Which one that was is not tracked; any processor in SMM
       stands for it -- exact with one processor, and with two whenever
       both agree. */
    n  = STATE_BUS | !!mem_smm_cpus;
    wp = _mem_wp_bus[g];

    if (!wp && (map->write_b || map->write_w || map->write_l) && mem_mapping_access_allowed(map->flags, _mem_state[g].states[n].w))
        write_mapping_bus[g] = map;
    if ((map->read_b || map->read_w || map->read_l) && mem_mapping_access_allowed(map->flags, _mem_state[g].states[n].r))
        read_mapping_bus[g] = map;
}

static void
mem_mapping_clear_granule(uint64_t c)
{
    uint32_t g = (uint32_t) (c >> MEM_GRANULARITY_BITS);

    for (int n = 0; n < 2; n++) {
        mem_exec_view[n][g]          = NULL;
        mem_write_mapping_view[n][g] = NULL;
        mem_read_mapping_view[n][g]  = NULL;
    }
    write_mapping_bus[g] = NULL;
    read_mapping_bus[g]  = NULL;
}

/* Processor `cpu` enters or leaves SMM. Answers 1 when SMIACT# changes for
   the bus -- the first processor in, or the last out -- so the caller has
   the SMRAM ranges' bus view rebuilt; 0 when only this processor's view
   changes. */
int
mem_smm_enter(int cpu)
{
    const uint32_t old = mem_smm_cpus;

    mem_smm_cpus |= 1u << cpu;
    return !old;
}

int
mem_smm_leave(int cpu)
{
    const uint32_t old = mem_smm_cpus;

    mem_smm_cpus &= ~(1u << cpu);
    return (old & (1u << cpu)) && !(old & ~(1u << cpu));
}

/* A chipset that sets the execute pointer itself sets it in both views. */
void
mem_set_exec_all_views(uint32_t addr, uint8_t *exec)
{
    mem_exec_view[0][addr >> MEM_GRANULARITY_BITS] = exec;
    mem_exec_view[1][addr >> MEM_GRANULARITY_BITS] = exec;
}

/* The tables cover the 32-bit address space and end at 4 GB. */
#define MEM_ADDR_SPACE_END 0x100000000ULL

/* A mapping placed past 4 GB: what is known about it, then stop. */
static void
mem_mapping_past_4g(uint64_t base, uint64_t size, uint32_t aliases)
{
    const mem_mapping_t *map;

    pclog("MEM: a mapping at %08llX-%09llX (%llu KB), %s%08X, runs past 4 GB; guest at %04X:%08X, CR0 %08X\n",
          (unsigned long long) base, (unsigned long long) (base + size - 1), (unsigned long long) (size >> 10),
          aliases ? "aliased every " : "no aliases ", aliases ? ((~aliases) + 1) : 0, CS, cpu_state.pc, cr0);
    for (map = base_mapping; map != NULL; map = map->next) {
        if (((uint64_t) map->base == base) && ((uint64_t) map->size == size))
            pclog("MEM: mapping %p: priv %p, flags %08X, read_b %p, write_b %p\n",
                  (const void *) map, map->priv, map->flags, (void *) map->read_b, (void *) map->write_b);
    }
    fatal("A device mapped memory at %08llX-%09llX, past 4 GB, where no address lines reach.\n"
          "This is a bug in the device model; the log names the mapping.\n",
          (unsigned long long) base, (unsigned long long) (base + size - 1));
}

void
mem_mapping_recalc(uint64_t base, uint64_t size, uint32_t base_ignore)
{
    mem_mapping_t *map;
    uint64_t       c;
    uint64_t       mask = (cpu_16bitbus ?
                           (is6117 ? 0x03ffffffULL : 0x00ffffffULL) :
                           0xffffffffULL);

    if (!size || (base_mapping == NULL))
        return;

    /* No address lines reach past 4 GB, and the tables end there: a mapping
       that runs past it, or an alias of one, is a device model's bug. It is
       stopped here, before the walk below indexes off the end of the tables
       and overwrites whatever follows them. */
    if ((base + size + (base_ignore & mask)) > MEM_ADDR_SPACE_END)
        mem_mapping_past_4g(base, size, base_ignore & (uint32_t) mask);

    mem_mapping_gen++;

    map = base_mapping;

    /* Clear out old mappings. */
    uint64_t o_a   = ((~base_ignore) & 0xffffffffULL) + 0x00000001ULL;
    uint64_t o_s   = 0x00000000ULL;
    uint64_t o_e   = base_ignore & mask;
    uint64_t o_c   = 0x00000000ULL;

    if (o_e == 0x00000000ULL) {
        for (c = base; c < base + size; c += MEM_GRANULARITY_SIZE)
            mem_mapping_clear_granule(c);
    } else  for (o_c = o_s; o_c <= o_e; o_c += o_a) {
        for (c = (base + o_c); c < (base + size + o_c); c += MEM_GRANULARITY_SIZE)
            mem_mapping_clear_granule(c);
    }

    /* Walk mapping list. */
    while (map != NULL) {
        /* In range? */
        if (map->enable && (uint64_t) map->base < ((uint64_t) base + (uint64_t) size) &&
            ((uint64_t) map->base + (uint64_t) map->size) > (uint64_t) base) {
            uint32_t b_i   = base_ignore & map->base_ignore;
            uint64_t i_a   = ((~b_i) & 0xffffffffULL) + 0x00000001ULL;
            uint64_t i_s   = 0x00000000ULL;
            uint64_t i_e   = b_i & mask;
            uint64_t i_c   = 0x00000000ULL;
            uint64_t start = (map->base < base) ? map->base : base;
            uint64_t end   = (((uint64_t) map->base + (uint64_t) map->size) < (base + size)) ?
                             ((uint64_t) map->base + (uint64_t) map->size) : (base + size);
            if (start < map->base)
                start = map->base;

            if (i_e == 0x00000000ULL) {
                for (c = start; c < end; c += MEM_GRANULARITY_SIZE)
                    mem_mapping_apply_granule(map, c);
            } else  for (i_c = i_s; i_c <= i_e; i_c += i_a) {
                for (c = (start + i_c); c < (end + i_c); c += MEM_GRANULARITY_SIZE)
                    mem_mapping_apply_granule(map, c);
            }
        }
        map = map->next;
    }

    /* THE ALIASES OF THE RANGE WERE CLEARED ABOVE, and the walk put back only
       what aliases with it. Whatever else lives under an alias -- the
       machine's RAM sixteen megabytes above an ISA adapter's linear
       aperture, say -- was cleared and never restored, and that memory
       vanished the moment the adapter's aperture was switched on. Put back
       every mapping that does not itself alias, wherever an alias of the
       range crosses it. */
    if (o_e != 0x00000000ULL) {
        for (o_c = o_a; o_c <= o_e; o_c += o_a) {
            uint64_t a_base = base + o_c;
            uint64_t a_end  = base + size + o_c;

            for (map = base_mapping; map != NULL; map = map->next) {
                if (!map->enable || (base_ignore & map->base_ignore))
                    continue;
                uint64_t m_base = (uint64_t) map->base;
                uint64_t m_end  = (uint64_t) map->base + (uint64_t) map->size;
                uint64_t start  = (m_base > a_base) ? m_base : a_base;
                uint64_t end    = (m_end < a_end) ? m_end : a_end;

                for (c = start; c < end; c += MEM_GRANULARITY_SIZE)
                    mem_mapping_apply_granule(map, c);
            }
        }
    }

    flushmmucache_nopc();

#ifdef ENABLE_MEM_LOG
    pclog("\nMemory map:\n");
    mem_mapping_t *write = (mem_mapping_t *) -1, *read = (mem_mapping_t *) -1, *write_bus = (mem_mapping_t *) -1, *read_bus = (mem_mapping_t *) -1;
    for (c = 0; c < MEM_MAPPINGS_NO; c++) {
        if ((write_mapping[c] == write) && (read_mapping[c] == read) && (write_mapping_bus[c] == write_bus) && (read_mapping_bus[c] == read_bus))
            continue;
        write = write_mapping[c];
        read = read_mapping[c];
        write_bus = write_mapping_bus[c];
        read_bus = read_mapping_bus[c];

        pclog("%08X | ", c << MEM_GRANULARITY_BITS);
        if (read) {
            pclog("R%c%c%c %08X+% 8X",
                read->read_b ? 'b' : ' ', read->read_w ? 'w' : ' ', read->read_l ? 'l' : ' ',
                read->base, read->size);
        } else {
            pclog("                      ");
        }
        if (write) {
            pclog(" | W%c%c%c %08X+% 8X",
                write->write_b ? 'b' : ' ', write->write_w ? 'w' : ' ', write->write_l ? 'l' : ' ',
                write->base, write->size);
        } else {
            pclog(" |                       ");
        }
        pclog(" | %c\n", _mem_exec[c] ? 'X' : ' ');

        if ((write != write_bus) || (read != read_bus)) {
            pclog("   ^ bus | ");
            if (read_bus) {
                pclog("R%c%c%c %08X+% 8X",
                    read_bus->read_b ? 'b' : ' ', read_bus->read_w ? 'w' : ' ', read_bus->read_l ? 'l' : ' ',
                    read_bus->base, read_bus->size);
            } else {
                pclog("                      ");
            }
            if (write_bus) {
                pclog(" | W%c%c%c %08X+% 8X",
                    write_bus->write_b ? 'b' : ' ', write_bus->write_w ? 'w' : ' ', write_bus->write_l ? 'l' : ' ',
                    write_bus->base, write_bus->size);
            } else {
                pclog(" |                       ");
            }
            pclog(" |\n");
        }
    }
    pclog("\n");
#endif
}

void
mem_set_wp(uint64_t base, uint64_t size, uint8_t flags, uint8_t wp)
{
    uint64_t       c;
    uint64_t       end = base + size;

    for (c = base; c < end; c += MEM_GRANULARITY_SIZE) {
        if (flags & ACCESS_BUS)
            _mem_wp_bus[c >> MEM_GRANULARITY_BITS] = wp;
        if (flags & ACCESS_CPU)
            _mem_wp[c >> MEM_GRANULARITY_BITS] = wp;
    }

    mem_mapping_recalc(base, size, 0x00000000);
}

void
mem_mapping_set(mem_mapping_t *map,
                uint32_t       base,
                uint32_t       size,
                uint8_t (*read_b)(uint32_t addr, void *priv),
                uint16_t (*read_w)(uint32_t addr, void *priv),
                uint32_t (*read_l)(uint32_t addr, void *priv),
                void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                uint8_t *exec,
                uint32_t fl,
                void    *priv)
{
    if (size != 0x00000000)
        map->enable = 1;
    else
        map->enable = 0;
    map->base    = base;
    map->size    = size;
    map->mask    = (map->size ? 0xffffffff : 0x00000000);
    map->read_b  = read_b;
    map->read_w  = read_w;
    map->read_l  = read_l;
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;
    map->exec    = exec;
    map->flags   = fl;
    map->priv    = priv;
    map->next    = NULL;

    /*
       The ALi M6117 puts the RAM directly onto the internal 32-bit
       address bus but the external address bus is still 24-bit.
     */
    if (is6117 && (fl & MEM_MAPPING_ROMCS))
        map->base_ignore = 0xff000000;

    mem_log("mem_mapping_add(): Linked list structure: %08X -> %08X -> %08X\n", map->prev, map, map->next);

    /* If the mapping is disabled, there is no need to recalc anything. */
    if (size != 0x00000000)
        mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_add(mem_mapping_t *map,
                uint32_t       base,
                uint32_t       size,
                uint8_t (*read_b)(uint32_t addr, void *priv),
                uint16_t (*read_w)(uint32_t addr, void *priv),
                uint32_t (*read_l)(uint32_t addr, void *priv),
                void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                uint8_t *exec,
                uint32_t fl,
                void    *priv)
{
    /* Do a sanity check */
    if ((base_mapping == NULL) && (last_mapping != NULL)) {
        fatal("mem_mapping_add(): NULL base mapping with non-NULL last mapping\n");
        return;
    } else if ((base_mapping != NULL) && (last_mapping == NULL)) {
        fatal("mem_mapping_add(): Non-NULL base mapping with NULL last mapping\n");
        return;
    } else if ((base_mapping != NULL) && (base_mapping->prev != NULL)) {
        fatal("mem_mapping_add(): Base mapping with a preceding mapping\n");
        return;
    } else if ((last_mapping != NULL) && (last_mapping->next != NULL)) {
        fatal("mem_mapping_add(): Last mapping with a following mapping\n");
        return;
    }

    /* Add mapping to the beginning of the list if necessary.*/
    if (base_mapping == NULL)
        base_mapping = map;

    /* Add mapping to the end of the list.*/
    if (last_mapping == NULL)
        map->prev = NULL;
    else {
        map->prev          = last_mapping;
        last_mapping->next = map;
    }
    last_mapping = map;

    mem_mapping_set(map, base, size, read_b, read_w, read_l,
                    write_b, write_w, write_l, exec, fl, priv);
}

void
mem_mapping_do_recalc(mem_mapping_t *map)
{
    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_handler(mem_mapping_t *map,
                        uint8_t (*read_b)(uint32_t addr, void *priv),
                        uint16_t (*read_w)(uint32_t addr, void *priv),
                        uint32_t (*read_l)(uint32_t addr, void *priv),
                        void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                        void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                        void (*write_l)(uint32_t addr, uint32_t val, void *priv))
{
    map->read_b  = read_b;
    map->read_w  = read_w;
    map->read_l  = read_l;
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_write_handler(mem_mapping_t *map,
                              void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                              void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                              void (*write_l)(uint32_t addr, uint32_t val, void *priv))
{
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size)
{
    /* Remove old mapping. */
    map->enable = 0;
    mem_mapping_recalc(map->base, map->size, map->base_ignore);

    /* Set new mapping. */
    map->enable = 1;
    map->base   = base;
    map->size   = size;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_base_ignore(mem_mapping_t *map, uint32_t base_ignore)
{
    /* Remove old mapping. */
    map->enable      = 0;
    mem_mapping_recalc(map->base, map->size, map->base_ignore);

    /* Set new mapping. */
    map->enable      = 1;
    map->base_ignore = base_ignore;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_exec(mem_mapping_t *map, uint8_t *exec)
{
    map->exec = exec;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_mask(mem_mapping_t *map, uint32_t mask)
{
    map->mask = mask;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_set_p(mem_mapping_t *map, void *priv)
{
    map->priv = priv;
}

void
mem_mapping_disable(mem_mapping_t *map)
{
    map->enable = 0;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_mapping_enable(mem_mapping_t *map)
{
    map->enable = 1;

    mem_mapping_recalc(map->base, map->size, map->base_ignore);
}

void
mem_set_access(uint8_t bitmap, int mode, uint32_t base, uint32_t size, uint16_t access)
{
    uint16_t       mask;
    uint16_t       smstate = 0x0000;
    const uint16_t smstates[4] = { 0x0000, (MEM_READ_SMRAM | MEM_WRITE_SMRAM),
                                   MEM_READ_SMRAM_EX, (MEM_READ_DISABLED_EX | MEM_WRITE_DISABLED_EX) };

    if (mode)
        mask = 0x2d6b;
    else
        mask = 0x1084;

    if (mode) {
        if (mode == 1)
            access = !!access;

        if (mode == 3) {
            if (access & ACCESS_SMRAM_X)
                smstate |= MEM_EXEC_SMRAM;
            if (access & ACCESS_SMRAM_R)
                smstate |= MEM_READ_SMRAM_2;
            if (access & ACCESS_SMRAM_W)
                smstate |= MEM_WRITE_SMRAM;
        } else
            smstate = smstates[access & 0x07];
    } else
        smstate = access & 0x6f7b;

    for (uint32_t c = 0; c < size; c += MEM_GRANULARITY_SIZE) {
        for (uint8_t i = 0; i < 4; i++) {
            if (bitmap & (1 << i)) {
                _mem_state[(c + base) >> MEM_GRANULARITY_BITS].vals[i] = (_mem_state[(c + base) >> MEM_GRANULARITY_BITS].vals[i] & mask) | smstate;
            }
        }

#ifdef ENABLE_MEM_LOG
        if (((c + base) >= 0xa0000) && ((c + base) <= 0xbffff)) {
            mem_log("Set mem state for block at %08X to %04X with bitmap %02X\n",
                    c + base, smstate, bitmap);
        }
#endif
    }

    mem_mapping_recalc(base, size, 0x00000000);
}

void
mem_a20_init(void)
{
    if (is286) {
        mem_a20_key = mem_a20_alt = mem_a20_state = 0;
        rammask = cpu_16bitbus ? 0xffffff : 0xffffffff;
        if (is6117)
            rammask |= 0x3000000;
        flushmmucache();
#if 0
        mem_a20_state = mem_a20_key | mem_a20_alt;
#endif
    } else {
        rammask = 0xfffff;
        flushmmucache();
        mem_a20_key = mem_a20_alt = mem_a20_state = 0;
    }
}

/* Close all the memory mappings. */
void
mem_close(void)
{
    mem_mapping_t *map = base_mapping;
    mem_mapping_t *next;

    while (map != NULL) {
        next      = map->next;
        map->prev = map->next = NULL;
        map                   = next;
    }

    base_mapping = last_mapping = 0;
}

static void
mem_add_ram_mapping(mem_mapping_t *mapping, uint32_t base, uint32_t size)
{
    mem_mapping_add(mapping, base, size,
                    mem_read_ram, mem_read_ramw, mem_read_raml,
                    mem_write_ram, mem_write_ramw, mem_write_raml,
                    ram + base, MEM_MAPPING_INTERNAL, NULL);
}

static void
mem_init_ram_mapping(mem_mapping_t *mapping, uint32_t base, uint32_t size)
{
    mem_set_mem_state_both(base, size, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    mem_add_ram_mapping(mapping, base, size);
}

void
mem_zero(void)
{
    memset(ram, 0x00, ram_size + 16);
}

/* Reset the memory state. */
void
mem_reset(void)
{
    size_t m;
    uint8_t large_mem = 0;
    static int ram_guarded = 0;

    memset(page_ff, 0xff, sizeof(page_ff));

    mem_pages_free_copies();

    if (byte_dirty_mask) {
        free(byte_dirty_mask);
        byte_dirty_mask = NULL;
    }

    if (byte_code_present_mask) {
        free(byte_code_present_mask);
        byte_code_present_mask = NULL;
    }

    /* Free the old pages array, if necessary. */
    if (pages) {
        free(pages);
        pages = NULL;
    }

    if (ram != NULL) {
        if (ram_guarded)
            plat_munmap_guarded(ram, ram_size + 16);
        else
            plat_munmap(ram, ram_size + 16);
        ram      = NULL;
        ram_size = 0;
    }

    m = 1024UL * (size_t) mem_size;

    ram_size = m;
    /* Allocate 16 extra bytes of RAM to mitigate some dynarec recompiler memory access quirks. */
    ram_guarded = mem_guard_pages;
    if (ram_guarded) {
        /* For finding stray accesses: normal pages between two inaccessible
           ones, so that running off the block faults where it happens. */
        ram = (uint8_t *) plat_mmap_guarded(ram_size + 16);
        if (ram != NULL)
            pclog("RAM allocated between guard pages at %p\n", (void *) ram);
    } else
        ram = (uint8_t *) plat_mmap(ram_size + 16, 0, &large_mem); /* allocate and clear the RAM block */
    if (ram == NULL) {
        fatal("Failed to allocate RAM block. Make sure you have enough RAM available.\n");
        return;
    }

    if (large_mem)
        pclog("Allocated %.02lf megabytes of large pages for RAM\n", ram_size / (double)(1024 * 1024));

    /*
     * Allocate the page table based on how much RAM we have.
     * We re-allocate the table on each (hard) reset, as the
     * memory amount could have changed.
     */
    if (is286) {
        if (cpu_16bitbus) {
            /* 80286/386SX; maximum address space is 16MB + 16 MB for EMS. */
            m = 8192;
            /* ALi M6117; maximum address space is 4GB. */
            if (is6117)
                m <<= 2;
        } else {
            /* 80386DX+; maximum address space is 4GB. */
            m = 1048576;
        }
    } else {
        /* 8088/86; maximum address space is 1MB. */
        m = 256;
    }

    addr_space_size = m;

    /*
     * Allocate and initialize the (new) page table.
     */
    pages_sz = m;
    pages    = (page_t *) calloc(m, sizeof(page_t));

    memset(page_lookup, 0x00, (1 << 20) * sizeof(page_t *));

    byte_dirty_mask = calloc(1, ((size_t) mem_size * 1024) / 8);

    byte_code_present_mask = calloc(1, ((size_t) mem_size * 1024) / 8);

    for (uint32_t c = 0; c < pages_sz; c++) {
        if (((uint64_t) c << 12) >= ((uint64_t) mem_size << 10))
            pages[c].mem = page_ff;
        else {

            pages[c].mem = &ram[c << 12];
        }
        if (c < m) {
            pages[c].write_b = mem_write_ramb_page;
            pages[c].write_w = mem_write_ramw_page;
            pages[c].write_l = mem_write_raml_page;
        }
        pages[c].evict_prev             = EVICT_NOT_IN_LIST;
        mem_page_set_byte_masks(&pages[c], c);
    }

    mem_pages_alloc_copies();

    memset(mem_exec_view, 0x00, sizeof(mem_exec_view));
    mem_cpu_reset();
    memset(_mem_wp, 0x00, sizeof(_mem_wp));
    memset(_mem_wp_bus, 0x00, sizeof(_mem_wp_bus));
    memset(mem_write_mapping_view, 0x00, sizeof(mem_write_mapping_view));
    memset(mem_read_mapping_view, 0x00, sizeof(mem_read_mapping_view));
    /* No processor is in SMM on a fresh map. */
    mem_smm_cpus = 0;
    mem_smm_view(0);
    memset(write_mapping_bus, 0x00, sizeof(write_mapping_bus));
    memset(read_mapping_bus, 0x00, sizeof(read_mapping_bus));

    base_mapping = last_mapping = NULL;

    /* Set the entire memory space as external. */
    memset(_mem_state, 0x00, sizeof(_mem_state));

    /* Set the low RAM space as internal. */
    mem_init_ram_mapping(&ram_low_mapping, 0x000000, (mem_size > 640) ? 0xa0000 : mem_size * 1024);

    if (mem_size > 1024) {
        if (cpu_16bitbus && !is6117 && (mem_size > 16256))
            mem_init_ram_mapping(&ram_high_mapping, 0x100000, (16256 - 1024) * 1024);
        else if (cpu_16bitbus && is6117 && (mem_size > 65408))
            mem_init_ram_mapping(&ram_high_mapping, 0x100000, (65408 - 1024) * 1024);
        else
           mem_init_ram_mapping(&ram_high_mapping, 0x100000, (mem_size - 1024) * 1024);
    }

    if (mem_size > 768) {
        mem_add_ram_mapping(&ram_mid_mapping, 0xa0000, 0x60000);

        mem_add_ram_mapping(&ram_mid_mapping2, 0xa0000, 0x60000);
        mem_mapping_disable(&ram_mid_mapping2);
    }

    mem_mapping_add(&ram_remapped_mapping, mem_size * 1024, 256 * 1024,
                    mem_read_remapped, mem_read_remappedw, mem_read_remappedl,
                    mem_write_remapped, mem_write_remappedw, mem_write_remappedl,
                    ram + 0xa0000, MEM_MAPPING_INTERNAL, NULL);
    mem_mapping_disable(&ram_remapped_mapping);

    /* Mapping for SiS 471 relocation which relocates A0000-BFFFF, D0000-EFFFF, which is non-contiguous. */
    mem_mapping_add(&ram_remapped_mapping2, mem_size * 1024, 256 * 1024,
                    mem_read_remapped2, mem_read_remappedw2, mem_read_remappedl2,
                    mem_write_remapped2, mem_write_remappedw2, mem_write_remappedl2,
                    ram + 0xd0000, MEM_MAPPING_INTERNAL, NULL);
    mem_mapping_disable(&ram_remapped_mapping2);

    mem_a20_init();

    for (int i = 0; i < mem_page_copy_count; i++)
        mem_page_copies[i].reset_evict_list();
}

void
mem_init(void)
{
    /* Perform a one-time init. */
    ram = rom = NULL;
    pages     = NULL;

    mem_tlb_register(mem_tlb_default());
    mem_cpu_register_pages();
}

static void
mem_remap_top_ex_common(int kb, uint32_t start, int mid)
{
    uint32_t   c;
    int        offset;
    int        size = mem_size - 640;
    int        set        = 1;
    static int old_kb     = 0;
    int        sis_mode   = 0;
    uint32_t   start_addr = 0;
    uint32_t   addr = 0;

    mem_log("MEM: remapping top %iKB (mem=%i)\n", kb, mem_size);
    if (mem_size <= 640)
        return;

    /* SiS 471 special mode. */
    if (kb == -256) {
        kb       = 256;
        sis_mode = 1;
    }

    /* Do not remap if we're have more than (16 MB - RAM) memory. */
    if ((kb != 0) && (mem_size >= (16384 - kb)))
        return;

    if (kb == 0) {
        kb  = old_kb;
        set = 0;
    } else
        old_kb = kb;

    if (size > kb)
        size = kb;

    remap_start_addr  = start << 10;
    remap_start_addr2 = (start << 10) + 0x00020000;

    for (c = ((start * 1024) >> 12); c < (((start + size) * 1024) >> 12); c++) {
        offset = c - ((start * 1024) >> 12);
        /* Use A0000-BFFFF, D0000-EFFFF instead of C0000-DFFFF, E0000-FFFFF. */
        addr = 0xa0000 + (offset << 12);
        if (sis_mode) {
            /* A0000-DFFFF -> A0000-BFFFF, D0000-EFFFF */
            if (addr >= 0x000c0000)
                addr += 0x00010000;
        }
        if (start_addr == 0)
            start_addr = addr;
        pages[c].mem     = set ? &ram[addr] : page_ff;
        pages[c].write_b = set ? mem_write_ramb_page : NULL;
        pages[c].write_w = set ? mem_write_ramw_page : NULL;
        pages[c].write_l = set ? mem_write_raml_page : NULL;
        mem_page_set_byte_masks(&pages[c], addr >> 12);
    }

    mem_set_mem_state_both(start * 1024, size * 1024, set ? (MEM_READ_INTERNAL | MEM_WRITE_INTERNAL) : (MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL));

    for (c = 0xa0; c < 0xf0; c++) {
        if ((c >= 0xc0) && (c <= 0xcf))
            continue;

        if (sis_mode || ((c << 12) >= (mem_size << 10)))
            pages[c].mem = page_ff;
        else {
            pages[c].mem = &ram[c << 12];
        }
        if (!sis_mode && (c < addr_space_size)) {
            pages[c].write_b = mem_write_ramb_page;
            pages[c].write_w = mem_write_ramw_page;
            pages[c].write_l = mem_write_raml_page;
        } else {
            pages[c].write_b = NULL;
            pages[c].write_w = NULL;
            pages[c].write_l = NULL;
        }
        mem_page_set_byte_masks(&pages[c], c);
    }

    if (set) {
        if (sis_mode) {
            mem_mapping_set_addr(&ram_remapped_mapping, start * 1024, 0x00020000);
            mem_mapping_set_exec(&ram_remapped_mapping, ram + 0x000a0000);
            mem_mapping_set_addr(&ram_remapped_mapping2, (start * 1024) + 0x00020000, 0x00020000);
            mem_mapping_set_exec(&ram_remapped_mapping2, ram + 0x000d0000);

            if (mid) {
                mem_mapping_set_addr(&ram_mid_mapping, 0x000c0000, 0x00010000);
                mem_mapping_set_exec(&ram_mid_mapping, ram + 0x000c0000);
                mem_mapping_set_addr(&ram_mid_mapping2, 0x000f0000, 0x00010000);
                mem_mapping_set_exec(&ram_mid_mapping2, ram + 0x000f0000);
            }
        } else {
            mem_mapping_set_addr(&ram_remapped_mapping, start * 1024, size * 1024);
            mem_mapping_set_exec(&ram_remapped_mapping, ram + start_addr);
            mem_mapping_disable(&ram_remapped_mapping2);

            if (mid) {
                mem_mapping_set_addr(&ram_mid_mapping, 0x000a0000, 0x00060000);
                mem_mapping_set_exec(&ram_mid_mapping, ram + 0x000a0000);
                mem_mapping_disable(&ram_mid_mapping2);
            }
        }
    } else {
        mem_mapping_disable(&ram_remapped_mapping);
        mem_mapping_disable(&ram_remapped_mapping2);

        if (mid) {
            mem_mapping_set_addr(&ram_mid_mapping, 0x000a0000, 0x00060000);
            mem_mapping_set_exec(&ram_mid_mapping, ram + 0x000a0000);
            mem_mapping_disable(&ram_mid_mapping2);
        }
    }

    mem_pages_mirror(0, pages_sz, 0);

    flushmmucache();
}

void
mem_remap_top_ex(int kb, uint32_t start)
{
    mem_remap_top_ex_common(kb, start, 1);
}

void
mem_remap_top_ex_nomid(int kb, uint32_t start)
{
    mem_remap_top_ex_common(kb, start, 0);
}

void
mem_remap_top(int kb)
{
    mem_remap_top_ex(kb, (mem_size >= 1024) ? mem_size : 1024);
}

void
mem_remap_top_nomid(int kb)
{
    mem_remap_top_ex_nomid(kb, (mem_size >= 1024) ? mem_size : 1024);
}

void
mem_a20_recalc(void)
{
    int state;

    if (!is286) {
        rammask = 0xfffff;
        flushmmucache();
        mem_a20_key = mem_a20_alt = mem_a20_state = mem_a20_chipset = 0;

        return;
    }

    state = mem_a20_key | mem_a20_alt | mem_a20_chipset;
    if (state && !mem_a20_state) {
        rammask = cpu_16bitbus ? 0xffffff : 0xffffffff;
        if (is6117)
            rammask |= 0x3000000;
        flushmmucache();
    } else if (!state && mem_a20_state) {
        rammask = cpu_16bitbus ? 0xefffff : 0xffefffff;
        if (is6117)
            rammask |= 0x3000000;
        flushmmucache();
    }

    mem_a20_state = state;
}

/* ---- the debug console's view of the map --------------------------------- */

mem_mapping_t *
mem_debug_first_mapping(void)
{
    return base_mapping;
}

/* What a processor in view 0 (outside SMM) or 1 (inside) meets at addr. */
void
mem_debug_granule(uint32_t addr, int view, mem_mapping_t **rd, mem_mapping_t **wr, uint8_t **ex)
{
    uint32_t g = addr >> MEM_GRANULARITY_BITS;

    view = !!view;
    *rd  = mem_read_mapping_view[view][g];
    *wr  = mem_write_mapping_view[view][g];
    *ex  = mem_exec_view[view][g];
}

/* What a bus master meets at addr. */
void
mem_debug_bus(uint32_t addr, mem_mapping_t **rd, mem_mapping_t **wr)
{
    *rd = read_mapping_bus[addr >> MEM_GRANULARITY_BITS];
    *wr = write_mapping_bus[addr >> MEM_GRANULARITY_BITS];
}

/* The chipset's access state for addr: n = 0 processor, 1 processor in SMM,
   2 bus, 3 bus with SMIACT#. */
uint16_t
mem_debug_state(uint32_t addr, int n)
{
    return _mem_state[addr >> MEM_GRANULARITY_BITS].vals[n & 3];
}

/* The processors in SMM, one bit each: SMIACT# for the bus while any is. */
uint32_t
mem_debug_smm_cpus(void)
{
    return mem_smm_cpus;
}
