/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Whole runs of a REP string instruction's elements, for the
 *          handlers in x86_ops_rep.h and x86_ops_rep_dyn.h. A run is the
 *          elements that lie in one page on each side, pass the segment
 *          checks, and fit the cycles the handler would have spent before
 *          stopping anyway; it is done only when the TLB already maps each
 *          side straight to RAM (readlookup2, writelookup2), which is when
 *          the per-element path would have done each element straight to
 *          RAM too. A direct write entry exists only for a page without
 *          compiled code, so nothing needs marking dirty.
 *          Anything else -- the first element of a page not yet in the
 *          TLB, a page holding code, MMIO or ROM, an element across a
 *          page, a segment fault, a data breakpoint, the trap flag -- is
 *          left to the per-element path, which runs as it always did.
 *
 *          The registers, the flags and the cycles charged come out as they
 *          would have element by element; the host does the memory.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#ifndef EMU_X86_REP_FAST_H
#define EMU_X86_REP_FAST_H

#include "x86_rep_host.h"

/* The offset registers' width, by the handlers' size suffix: 16-bit
   addressing must not wrap SI or DI inside a run. */
#define REP_AMASK_a16    0xffffu
#define REP_AMASK_a32    0xffffffffu
#define REP_AMASK_a16_E  0xffffu
#define REP_AMASK_a16_NE 0xffffu
#define REP_AMASK_a32_E  0xffffffffu
#define REP_AMASK_a32_NE 0xffffffffu

/* How many elements from off, of esz bytes each and in the direction of
   travel, one side of a run may take, at most n; 0 when this side cannot
   be done as a run at all. *host is element 0 in host memory. amask is
   0xffff for 16-bit addressing (the offset must not wrap) or 0xffffffff. */
static inline uint32_t
rep_fast_span(const x86seg *seg, uint32_t off, uint32_t esz, int down, uint32_t amask, int write, uint32_t n,
              uint32_t *linp)
{
    const uint64_t lo = seg->limit_low;
    const uint64_t hi = seg->limit_high;
    uint32_t       lin;
    uint32_t       room;
    uint64_t       k;

    /* As SEG_CHECK_*_REP and CHECK_*_REP would find each element: a
       usable, present segment, writable for a store, and every byte of the
       run inside its limits. */
    if (seg->base == 0xffffffff)
        return 0;
    if (write && SEG_NOT_WRITABLE(seg))
        return 0;
    if ((msw & 1) && !(cpu_state.eflags & VM_FLAG) && !(seg->access & 0x80))
        return 0;
    if (((uint64_t) off + esz - 1) > amask)
        return 0;
    if ((off < lo) || (((uint64_t) off + esz - 1) > hi))
        return 0;

    lin = seg->base + off;

    /* Whole elements inside this page, starting with element 0. */
    if (!down)
        room = (0x1000 - (lin & 0xfff)) / esz;
    else
        room = (((lin & 0xfff) + esz) <= 0x1000) ? ((lin & 0xfff) / esz + 1) : 0;
    if (room < n)
        n = room;

    /* And inside the segment and the address size. */
    if (!down) {
        k = (hi - off + 1) / esz;
        if (k < n)
            n = (uint32_t) k;
        k = ((uint64_t) amask - off + 1) / esz;
        if (k < n)
            n = (uint32_t) k;
    } else {
        k = (off - lo) / esz + 1;
        if (k < n)
            n = (uint32_t) k;
        k = off / esz + 1;
        if (k < n)
            n = (uint32_t) k;
    }

    *linp = lin;
    return n;
}

/* The same, and only when the TLB maps this side straight to host memory:
   *host is element 0 there. */
static inline uint32_t
rep_fast_side(const x86seg *seg, uint32_t off, uint32_t esz, int down, uint32_t amask, int write, uint32_t n,
              uint8_t **host)
{
    uint32_t  lin;
    uintptr_t e;

    n = rep_fast_span(seg, off, esz, down, amask, write, n, &lin);
    if (!n)
        return 0;
    e = write ? writelookup2[lin >> 12] : readlookup2[lin >> 12];
    if (e == (uintptr_t) LOOKUP_INV)
        return 0;
    *host = (uint8_t *) (e + (uintptr_t) lin);
    return n;
}

/* Elements the per-element loop would do before its cycle test stops it:
   it stops after the first element that leaves cycles below cycles_end. */
static inline uint32_t
rep_fast_budget(int cyc, int cycles_end, int per)
{
    if (cyc < cycles_end)
        return 1;
    return (uint32_t) ((cyc - cycles_end) / per) + 1;
}

/* The lowest byte of a run of n elements whose element 0 is at p. */
static inline uint8_t *
rep_fast_low(uint8_t *p, uint32_t n, uint32_t esz, int down)
{
    return down ? p - (n - 1) * esz : p;
}

/* MOVS: up to n elements, at most budget; returns how many were done
   (0: take the per-element path). */
static inline uint32_t
rep_fast_movs(const x86seg *sseg, uint32_t soff, uint32_t doff, uint32_t esz, uint32_t amask, uint32_t n,
              uint32_t budget)
{
    const int down = !!(cpu_state.flags & D_FLAG);
    uint8_t  *s;
    uint8_t  *d;
    uint8_t  *slo;
    uint8_t  *dlo;
    uint32_t  bytes;

    if (dr[7] & 0xff)
        return 0;
    /* A destination outside RAM (the VGA window, a frame buffer) fails the
       write side whatever the source; found before the source is looked at,
       as a copy to one fails here at every element. */
    if (writelookup2[(uint32_t) (cpu_state.seg_es.base + doff) >> 12] == (uintptr_t) LOOKUP_INV)
        return 0;
    if (budget < n)
        n = budget;
    n = rep_fast_side(sseg, soff, esz, down, amask, 0, n, &s);
    if (!n)
        return 0;
    n = rep_fast_side(&cpu_state.seg_es, doff, esz, down, amask, 1, n, &d);
    if (!n)
        return 0;

    bytes = n * esz;
    slo   = rep_fast_low(s, n, esz, down);
    dlo   = rep_fast_low(d, n, esz, down);

    /* Apart, or the destination trailing the source in the direction of
       travel: a bulk copy in that direction is exactly the element by
       element one. Leading it, each element reads what an earlier one
       wrote, and the host goes element by element too. */
    if (((dlo + bytes) <= slo) || ((slo + bytes) <= dlo) || (down ? (dlo >= slo) : (dlo <= slo)))
        rep_host_copy(dlo, slo, bytes, down);
    else
        rep_host_copy_elems(d, s, n, esz, down);
    return n;
}

/* MOVSD from host memory into device memory (the VGA window, a frame
   buffer), element by element through the device's handler as the
   per-element path goes, without the checks and the translation it makes
   again for each (mem_rep_writel_device). Charges the cycles itself and
   stops where that loop would; 0: take the per-element path. */
static inline uint32_t
rep_fast_movsl_device(const x86seg *sseg, uint32_t soff, uint32_t doff, uint32_t amask, uint32_t n, int per,
                      int cycles_end)
{
    const int down = !!(cpu_state.flags & D_FLAG);
    uint8_t  *s;
    uint32_t  lin;

    if (dr[7] & 0xff)
        return 0;
    n = rep_fast_side(sseg, soff, 4, down, amask, 0, n, &s);
    if (!n)
        return 0;
    n = rep_fast_span(&cpu_state.seg_es, doff, 4, down, amask, 1, n, &lin);
    if (!n)
        return 0;
    return mem_rep_writel_device(lin, s, n, down, per, cycles_end);
}

/* STOS. */
static inline uint32_t
rep_fast_stos(uint32_t doff, uint32_t esz, uint32_t amask, uint32_t value, uint32_t n, uint32_t budget)
{
    const int down = !!(cpu_state.flags & D_FLAG);
    uint8_t  *d;

    if (dr[7] & 0xff)
        return 0;
    if (budget < n)
        n = budget;
    n = rep_fast_side(&cpu_state.seg_es, doff, esz, down, amask, 1, n, &d);
    if (!n)
        return 0;
    rep_host_fill(rep_fast_low(d, n, esz, down), value, n, esz, down);
    return n;
}

/* SCAS and CMPS: how many elements to go straight past, all of them ones
   the instruction continues after. The last element of a run is always
   left to the per-element path -- the element that stops it, the last of
   the count or the last of the budget -- so it sets the flags, and the
   loop ends exactly where it would have. */
static inline uint32_t
rep_fast_scas(uint32_t doff, uint32_t esz, uint32_t amask, uint32_t value, int repe, uint32_t n, uint32_t budget)
{
    const int down = !!(cpu_state.flags & D_FLAG);
    uint8_t  *d;
    uint32_t  go;

    if (dr[7] & 0xff)
        return 0;
    if (budget < n)
        n = budget;
    if (n < 2)
        return 0;
    n = rep_fast_side(&cpu_state.seg_es, doff, esz, down, amask, 0, n - 1, &d);
    if (!n)
        return 0;
    go = rep_host_scan(rep_fast_low(d, n, esz, down), n, esz, down, value, repe);
    return go;
}

static inline uint32_t
rep_fast_cmps(const x86seg *sseg, uint32_t soff, uint32_t doff, uint32_t esz, uint32_t amask, int repe, uint32_t n,
              uint32_t budget)
{
    const int down = !!(cpu_state.flags & D_FLAG);
    uint8_t  *s;
    uint8_t  *d;

    if (dr[7] & 0xff)
        return 0;
    /* CMPS checks its reads with CHECK_READ, not the REP form: an execute-
       only code segment faults, so leave that to the per-element path. */
    if ((msw & 1) && !(cpu_state.eflags & VM_FLAG) &&
        (((sseg->access & 10) == 8) || ((cpu_state.seg_es.access & 10) == 8)))
        return 0;
    if (budget < n)
        n = budget;
    if (n < 2)
        return 0;
    n = rep_fast_side(sseg, soff, esz, down, amask, 0, n - 1, &s);
    if (!n)
        return 0;
    n = rep_fast_side(&cpu_state.seg_es, doff, esz, down, amask, 0, n, &d);
    if (!n)
        return 0;
    return rep_host_cmp(rep_fast_low(s, n, esz, down), rep_fast_low(d, n, esz, down), n, esz, down, repe);
}

#endif /*EMU_X86_REP_FAST_H*/
