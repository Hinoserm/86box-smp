/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Idle loops skipped: a loop that provably does nothing but wait
 *          has its passes charged without being run.
 *
 *          A compiled PAUSE calls x86_pause() with its EIP (codegen.c). The
 *          first time at a PAUSE the processor proves the loop: it empties
 *          its TLB and keeps it empty, so every load and store of one pass
 *          goes through the slow paths (mem.c), which log each access here.
 *          The pass proves the loop when it comes back to the same PAUSE
 *          with every register, flag, segment, control register and
 *          FPU/MMX register as it left, having stored only values already
 *          there, touched nothing but RAM and no port, and seen no TLB flush
 *          that may change a translation. Such a pass, run again from the
 *          same state on the same memory, is the same pass.
 *
 *          From then on, each time the processor is at that PAUSE with the
 *          same state and the same values at every address the pass touched,
 *          and nothing is pending (the signal word, an interrupt, an NMI or
 *          an SMI), the passes up to the next event are charged at once: as
 *          many whole passes as fit before the next timer is due (the
 *          chain's floor). The block then goes on, and the loop reaches its
 *          end as it would have. A visit in another state does not skip; the
 *          loop is proven again after a few such visits in a row, or after a
 *          TLB flush.
 *
 *          A loop polling a port whose reads only repeat (the device says so
 *          for the read, io_poll_pure: the VGA's status register 3DAh, a
 *          DOS game's wait for the vertical retrace) is proven and skipped
 *          the same way from its IN AL,DX (x86_idle_in), over two passes:
 *          the register may alternate between two values from one read to
 *          the next, so the state after two passes is the one it started
 *          from, and the two reads are the only port I/O allowed. Whole
 *          pairs of passes are charged, which leave the device as they found
 *          it.
 *
 *          A maskable interrupt waiting does not stop the skip when the
 *          loop was proven with one waiting and IF clear at the site (a
 *          spinlock taken with interrupts off): a pass that set IF anywhere
 *          (STI, POPF, IRET) would have taken it and failed the proof, so no
 *          pass takes one. A loop proven with none waiting is proven again
 *          when one comes with IF clear.
 *
 *          Only in compiled code while the recompiler chains blocks
 *          (CODEGEN_CHAIN): the chain's floor and signal word say how far a
 *          skip may go.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_ops.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "x87_sf.h"
#include "x87.h"
#include "x86_interrupt.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/plat_fallthrough.h>
#include <86box/plat_unused.h>
#ifdef USE_DYNAREC
#    include "codegen_public.h"
#endif
#include "x86_idle.h"

int x86_idle_watch;

#ifdef CODEGEN_CHAIN
extern int mmuflush;                       /* mem.c */
extern int codegen_chain_service(void);    /* 386_dynarec.c */

#    define IDLE_LOG_MAX   32     /* accesses a proven pass may make */
#    define IDLE_PASS_MAX  20000  /* cycles a pass may take */
#    define IDLE_RETRY     200000 /* cycles before a failed proof is tried again, doubled per failure */
#    define IDLE_RETRY_MAX 6      /* ... up to 64 times that */
#    define IDLE_MISSES    8      /* visits in a row a proof may not match before the loop is proven again */
#    define IDLE_VARIANTS  4      /* proofs kept for one site: the state it waits in may vary (a status bit) */

enum {
    IDLE_NONE = 0,
    IDLE_PROVING,
    IDLE_PROVEN
};

/* What a pass must leave as it found: everything an instruction can change
   but memory. */
typedef struct idle_state_t {
    x86reg   regs[8];
    uint16_t flags;
    uint16_t eflags;
    int      flags_op;
    uint32_t flags_res;
    uint32_t flags_op1;
    uint32_t flags_op2;
    struct {
        uint32_t base;
        uint32_t limit_low;
        uint32_t limit_high;
        uint16_t seg;
        uint8_t  access;
        uint8_t  ar_high;
    } seg[6];
    uint32_t c0;
    uint32_t c3;
    uint32_t c4;
    int      TOP;
    uint8_t  tag[8];
    uint16_t npxs;
    uint16_t npxc;
    uint8_t  ismmx;
    uint64_t st[8];
    uint64_t mm[8];
    uint16_t sf_cwd;
    uint16_t sf_swd;
    uint16_t sf_tag;
    uint8_t  sf_tos;
    uint8_t  sf_st[8][10];
} idle_state_t;

typedef struct idle_access_t {
    uint8_t *p;
    int      size;
    uint64_t v;
} idle_access_t;

/* A proof made: the state at the site, the values the passes read, the
   cycles they take, and whether an interrupt was waiting, IF clear. */
typedef struct idle_proof_t {
    idle_state_t  snap;
    idle_access_t log[IDLE_LOG_MAX];
    int           n;
    int           pass;
    int           masked;
} idle_proof_t;

static int           idle_mode;
static uint32_t      idle_site;
static idle_state_t  idle_snap;
static idle_access_t idle_log[IDLE_LOG_MAX];
static int           idle_n;
static int           idle_bad;
static uint64_t      idle_start_tsc;
static int           idle_pass;
static int           idle_mmuflush;
static uint64_t      idle_io;
static uint64_t      idle_retry_tsc;
static int           idle_fails;
static int           idle_misses;
static int           idle_period;      /* passes a proof spans: 1 at a PAUSE, 2 at a polling IN */
static int           idle_visits;      /* visits at the site since the proof began */
static int           idle_masked;      /* proven with an interrupt waiting, IF clear */
static int           idle_masked_from; /* one was waiting, IF clear, as the proof began */
/* The site's proofs: a waiting loop may find one of a few values between
   events (a status bit that changes on its own), each a state of its own;
   each proven once and kept. */
static idle_proof_t idle_proofs[IDLE_VARIANTS];
static int          idle_nproofs;
static int          idle_next_proof; /* the one a new proof replaces when all are in use */

enum {
    IDLE_PEND_NONE = 0,
    IDLE_PEND_MASKED, /* a maskable interrupt waits, IF clear, and the proof is not one with it */
    IDLE_PEND_OTHER
};

static void
idle_state_take(idle_state_t *s)
{
    const x86seg *sg[6] = { &cpu_state.seg_cs, &cpu_state.seg_ds, &cpu_state.seg_es,
                            &cpu_state.seg_ss, &cpu_state.seg_fs, &cpu_state.seg_gs };

    memset(s, 0, sizeof(idle_state_t));
    memcpy(s->regs, cpu_state.regs, sizeof(s->regs));
    s->flags     = cpu_state.flags;
    s->eflags    = cpu_state.eflags;
    s->flags_op  = cpu_state.flags_op;
    s->flags_res = cpu_state.flags_res;
    s->flags_op1 = cpu_state.flags_op1;
    s->flags_op2 = cpu_state.flags_op2;
    for (int i = 0; i < 6; i++) {
        s->seg[i].base       = sg[i]->base;
        s->seg[i].limit_low  = sg[i]->limit_low;
        s->seg[i].limit_high = sg[i]->limit_high;
        s->seg[i].seg        = sg[i]->seg;
        s->seg[i].access     = sg[i]->access;
        s->seg[i].ar_high    = sg[i]->ar_high;
    }
    s->c0    = cr0;
    s->c3    = cr3;
    s->c4    = cr4;
    s->TOP   = cpu_state.TOP;
    memcpy(s->tag, cpu_state.tag, sizeof(s->tag));
    s->npxs  = cpu_state.npxs;
    s->npxc  = cpu_state.npxc;
    s->ismmx = cpu_state.ismmx;
    memcpy(s->st, cpu_state.ST, sizeof(s->st));
    for (int i = 0; i < 8; i++)
        s->mm[i] = cpu_state.MM[i].q;
    if (fpu_softfloat) {
        s->sf_cwd = fpu_state.cwd;
        s->sf_swd = fpu_state.swd;
        s->sf_tag = fpu_state.tag;
        s->sf_tos = fpu_state.tos;
        for (int i = 0; i < 8; i++)
            memcpy(s->sf_st[i], &fpu_state.st_space[i], 10);
    }
}

/* Whether the processor is in state s now: what idle_state_take would take
   compared with it, without taking it, the registers first. */
static int
idle_state_same(const idle_state_t *s)
{
    const x86seg *sg[6] = { &cpu_state.seg_cs, &cpu_state.seg_ds, &cpu_state.seg_es,
                            &cpu_state.seg_ss, &cpu_state.seg_fs, &cpu_state.seg_gs };

    if (memcmp(s->regs, cpu_state.regs, sizeof(s->regs)) || (s->flags != cpu_state.flags) ||
        (s->eflags != cpu_state.eflags) || (s->flags_op != cpu_state.flags_op) || (s->flags_res != cpu_state.flags_res) ||
        (s->flags_op1 != cpu_state.flags_op1) || (s->flags_op2 != cpu_state.flags_op2))
        return 0;
    for (int i = 0; i < 6; i++) {
        if ((s->seg[i].base != sg[i]->base) || (s->seg[i].limit_low != sg[i]->limit_low) ||
            (s->seg[i].limit_high != sg[i]->limit_high) || (s->seg[i].seg != sg[i]->seg) ||
            (s->seg[i].access != sg[i]->access) || (s->seg[i].ar_high != sg[i]->ar_high))
            return 0;
    }
    if ((s->c0 != cr0) || (s->c3 != cr3) || (s->c4 != cr4) || (s->TOP != cpu_state.TOP) ||
        memcmp(s->tag, cpu_state.tag, sizeof(s->tag)) || (s->npxs != cpu_state.npxs) || (s->npxc != cpu_state.npxc) ||
        (s->ismmx != cpu_state.ismmx) || memcmp(s->st, cpu_state.ST, sizeof(s->st)))
        return 0;
    for (int i = 0; i < 8; i++) {
        if (s->mm[i] != cpu_state.MM[i].q)
            return 0;
    }
    if (fpu_softfloat) {
        if ((s->sf_cwd != fpu_state.cwd) || (s->sf_swd != fpu_state.swd) || (s->sf_tag != fpu_state.tag) ||
            (s->sf_tos != fpu_state.tos))
            return 0;
        for (int i = 0; i < 8; i++) {
            if (memcmp(s->sf_st[i], &fpu_state.st_space[i], 10))
                return 0;
        }
    } else if (s->sf_cwd || s->sf_swd || s->sf_tag || s->sf_tos)
        return 0; /* taken in the other mode */
    return 1;
}

/* The access's bytes in RAM, or NULL: anything but plain RAM -- the adapter
   and BIOS area, a device mapped in, beyond RAM, across a page -- cannot be
   proven. */
static uint8_t *
idle_ram(uint32_t addr, int size, int write)
{
    uint64_t phys;

    if (((addr & 0xfff) + size) > 0x1000)
        return NULL;
    if (cr0 >> 31) {
        phys = mmutranslate_noabrt(addr, write ? 1 : 0);
        if (phys > 0xffffffffULL)
            return NULL;
    } else
        phys = addr & rammask;
    if ((phys >= 0xa0000) && (phys < 0x100000))
        return NULL;
    if ((phys + size) > ((uint64_t) mem_size << 10))
        return NULL;
    if (!mem_addr_is_ram((uint32_t) phys) ||
        (write_mapping[phys >> MEM_GRANULARITY_BITS] != read_mapping[phys >> MEM_GRANULARITY_BITS]))
        return NULL;
    return &ram[phys];
}

static void
idle_stop(void)
{
    x86_idle_watch = 0;
}

/* A proof that did not hold: tried again later, the longer the more often
   it has failed in a row, so a loop that never qualifies stops costing
   anything. */
static void
idle_fail(void)
{
    idle_stop();
    /* The site's proofs made before still hold. */
    idle_mode      = idle_nproofs ? IDLE_PROVEN : IDLE_NONE;
    idle_retry_tsc = exec386_now_tsc() + ((uint64_t) IDLE_RETRY << idle_fails);
    if (idle_fails < IDLE_RETRY_MAX)
        idle_fails++;
}

/* One access of the pass being proven (mem.c, while x86_idle_watch). A
   store is proven only when it stores what is there. */
void
x86_idle_note(uint32_t addr, int size, int write, uint64_t val)
{
    uint8_t *p;
    uint64_t old = 0;

    if (idle_bad)
        return;
    if ((exec386_now_tsc() - idle_start_tsc) > IDLE_PASS_MAX) {
        idle_bad = 1;
        idle_fail();
        return;
    }
    p = idle_ram(addr, size, write);
    if (p == NULL) {
        idle_bad = 1;
        return;
    }
    memcpy(&old, p, size);
    if (write && (memcmp(&old, &val, size) != 0)) {
        idle_bad = 1;
        return;
    }
    for (int i = 0; i < idle_n; i++) {
        if ((idle_log[i].p == p) && (idle_log[i].size == size))
            return;
    }
    if (idle_n >= IDLE_LOG_MAX) {
        idle_bad = 1;
        return;
    }
    idle_log[idle_n].p    = p;
    idle_log[idle_n].size = size;
    idle_log[idle_n].v    = old;
    idle_n++;
}

/* A block ended with a pass still being proven: given up once it has run
   longer than a pass may (the loop left, or never came back). */
void
x86_idle_tick(void)
{
    if ((exec386_now_tsc() - idle_start_tsc) > IDLE_PASS_MAX)
        idle_fail();
}

static int
idle_memory_holds(const idle_proof_t *pr)
{
    for (int i = 0; i < pr->n; i++) {
        uint64_t v = 0;

        memcpy(&v, pr->log[i].p, pr->log[i].size);
        if (v != pr->log[i].v)
            return 0;
    }
    return 1;
}

/* A maskable interrupt waiting with IF clear. */
static int
idle_masked_now(void)
{
    return !(cpu_state.flags & I_FLAG) && x86_external_interrupt_pending();
}

static int
idle_pending(int masked)
{
    /* The signal word as a link stub reads it: code written, a relink or a
       translation change is the loop's work, and the break is answered by
       the chain's service, which also brings the chain's floor forward to
       a timer armed sooner. A polling IN raises the break with its own
       read. When the service says the chain must leave, the break is raised
       again for the chain's next link to see. */
    if (__atomic_load_n(&mem_chain_signal.pending, __ATOMIC_SEQ_CST) & 0xffffff00)
        return IDLE_PEND_OTHER;
    if (__atomic_load_n(&mem_chain_signal.brk, __ATOMIC_SEQ_CST) && codegen_chain_service()) {
        mem_chain_break();
        return IDLE_PEND_OTHER;
    }
    if (smi_line || NMI_TAKEABLE() || cpu_state.abrt)
        return IDLE_PEND_OTHER;
    if (!x86_external_interrupt_pending())
        return IDLE_PEND_NONE;
    if (cpu_state.flags & I_FLAG)
        return IDLE_PEND_OTHER;
    return masked ? IDLE_PEND_NONE : IDLE_PEND_MASKED;
}

/* A proof begun at eip; keep, the site's proofs made so far stay (another
   state of the same loop). */
static void
idle_prove(uint32_t eip, int period, int keep)
{
    if (!keep) {
        idle_nproofs    = 0;
        idle_next_proof = 0;
    }
    /* Every access of the pass through the slow paths: the TLB emptied,
       and kept empty while the watch is on (mem.c). */
    mem_tlb_empty();
    idle_site        = eip;
    idle_period      = period;
    idle_visits      = 0;
    idle_mode        = IDLE_PROVING;
    idle_state_take(&idle_snap);
    idle_n           = 0;
    idle_bad         = 0;
    idle_start_tsc   = exec386_now_tsc();
    idle_pass        = cycles;
    idle_mmuflush    = mmuflush;
    idle_io          = io_port_count;
    idle_masked_from = idle_masked_now();
    x86_idle_watch   = 1;
}

/* The cycles passes may be charged now: up to the chain's floor. */
static int64_t
idle_avail(void)
{
    return (int64_t) cycles - cpu_state.chain_floor - 1;
}

static void
idle_skip(int pass)
{
    const int64_t avail = idle_avail();

    if (avail <= pass)
        return;
    cycles -= (int) ((avail / pass) * pass);
}

/* A visit at an idle loop's site, eip: a compiled PAUSE (period 1) or a
   polling IN (period 2), the block's registers in cpu_state. */
static void
idle_visit(uint32_t eip, int period)
{
    if (!inrecomp)
        return;

    switch (idle_mode) {
        case IDLE_PROVING:
            /* A proof over two passes goes on, watching, past the first. */
            if ((eip == idle_site) && (period == idle_period) && !idle_bad && (++idle_visits < idle_period))
                return;
            idle_stop();
            /* The port I/O of the passes: none at a PAUSE; at a polling IN
               its own reads, one a pass. */
            if ((eip != idle_site) || (period != idle_period) || idle_bad || (mmuflush != idle_mmuflush) ||
                ((io_port_count - idle_io) != (uint64_t) ((period > 1) ? period : 0)) ||
                !idle_state_same(&idle_snap) || ((idle_pass - cycles) <= 0)) {
                idle_fail();
                return;
            }
            idle_pass   = idle_pass - cycles;
            idle_mode   = IDLE_PROVEN;
            idle_misses = 0;
            idle_masked = idle_masked_from && idle_masked_now();
            idle_fails  = 0;
            {
                idle_proof_t *pr;

                if (idle_nproofs < IDLE_VARIANTS)
                    pr = &idle_proofs[idle_nproofs++];
                else {
                    pr              = &idle_proofs[idle_next_proof];
                    idle_next_proof = (idle_next_proof + 1) % IDLE_VARIANTS;
                }
                memcpy(&pr->snap, &idle_snap, sizeof(idle_state_t));
                memcpy(pr->log, idle_log, sizeof(idle_access_t) * idle_n);
                pr->n      = idle_n;
                pr->pass   = idle_pass;
                pr->masked = idle_masked;
            }
            return;

        case IDLE_PROVEN:
            /* The proof holds as long as the translations do: from exactly
               this state over exactly these values a pass changes nothing.
               A visit that does not match only does not skip; the loop is
               proven again when many in a row do not match (it has settled
               elsewhere) or a translation may have changed. */
            if (mmuflush == idle_mmuflush) {
                if ((eip == idle_site) && (period == idle_period)) {
                    /* No room for a pass before the next event (as after a
                       skip): nothing to look at. */
                    int shortest = idle_proofs[0].pass;

                    for (int v = 1; v < idle_nproofs; v++) {
                        if (idle_proofs[v].pass < shortest)
                            shortest = idle_proofs[v].pass;
                    }
                    if (idle_avail() <= shortest)
                        return;
                    for (int v = 0; v < idle_nproofs; v++) {
                        const idle_proof_t *pr = &idle_proofs[v];

                        if (!idle_state_same(&pr->snap) || !idle_memory_holds(pr))
                            continue;
                        idle_misses = 0;
                        switch (idle_pending(pr->masked)) {
                            case IDLE_PEND_NONE:
                                idle_skip(pr->pass);
                                break;
                            case IDLE_PEND_MASKED:
                                idle_prove(eip, period, 1); /* again, with it waiting */
                                break;
                            default:
                                break;
                        }
                        return;
                    }
                    /* A state of the loop no proof has: proven too, after
                       some visits (a pass's other phase matches nothing
                       either), kept beside the others. */
                    if (++idle_misses < IDLE_MISSES)
                        return;
                    idle_misses = 0;
                    if ((int64_t) (exec386_now_tsc() - idle_retry_tsc) >= 0)
                        idle_prove(eip, period, 1);
                    return;
                }
                if (++idle_misses < IDLE_MISSES)
                    return;
            }
            idle_mode   = IDLE_NONE;
            idle_misses = 0;
            fallthrough;

        case IDLE_NONE:
        default:
            if ((int64_t) (exec386_now_tsc() - idle_retry_tsc) < 0)
                return;
            idle_prove(eip, period, 0);
            return;
    }
}

/* A compiled PAUSE at eip. */
void
x86_pause(uint32_t eip)
{
    idle_visit(eip, 1);
}

/* An IN AL,DX at eip has read a port whose reads only repeat
   (io_poll_pure). */
void
x86_idle_in(uint32_t eip)
{
    idle_visit(eip, 2);
}
#else
/* Without chains nothing is ever proven: the hooks in mem.c and the IN
   handler find the watch off and nothing to visit. */
void
x86_idle_note(UNUSED(uint32_t addr), UNUSED(int size), UNUSED(int write), UNUSED(uint64_t val))
{
}

void
x86_idle_in(UNUSED(uint32_t eip))
{
}
#endif
