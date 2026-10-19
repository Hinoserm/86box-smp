/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The other processors' host threads.
 *
 *          Each processor after the first runs in a core of its own
 *          (cpu_core.h), on a thread of its own, at the same time as the
 *          first. The machine's thread -- devices, timers, the clock, the
 *          first processor -- does their device work for them: port I/O
 *          and any memory that is not plain RAM are handed to it and
 *          carried out between two of the first processor's blocks, so
 *          device code only ever runs there. Plain RAM each core reaches
 *          itself.
 *
 *          TIME. The machine's clock is the first core's tsc; each other
 *          core keeps its own. None may be more than the bound W (the SMP
 *          drift setting) ahead of the machine's clock, and the machine's
 *          clock no more than W ahead of any of them: the one ahead waits,
 *          so the slowest thread sets the pace for all, as one thread did.
 *          Two of the others are therefore within 2W of each other. A
 *          processor with nothing to do -- halted with nothing to wake it,
 *          or waiting for a STARTUP -- sleeps and holds nobody back; woken,
 *          it goes on from the machine's time.
 *
 *          PARKING. The others run only while the machine's thread is in
 *          cpu_exec: leaving, the machine's thread parks them at a block
 *          boundary and waits for that. Pause, the debug console, settings,
 *          hard reset and power off all happen outside cpu_exec, so all of
 *          them find every one parked.
 *
 *          MAILBOX. What reaches a processor from outside -- NMI, SMI,
 *          INIT, STARTUP, a board reset -- is posted (cpu_mp.c) and taken
 *          by its own thread at its next block boundary.
 *
 *          A wait spins for a short while before sleeping: most waits are a
 *          few microseconds, far shorter than waking a sleeping thread.
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
#if defined(__x86_64__) || defined(_M_X64)
#    include <x86intrin.h>
#endif
#ifdef __APPLE__
#    include <pthread.h>
#    include <pthread/qos.h>
#endif
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/thread.h>
#include <86box/cpu_mp.h>
#include <86box/pic.h>
#include <86box/smram.h>
#include "cpu_core.h"
#include "cpu_thread.h"

/* Spins before a waiting side sleeps on its event. */
#define CPU_THREAD_SPINS 20000

enum {
    REQ_NONE = 0,
    REQ_POSTED,
    REQ_DONE
};

enum {
    FWD_INB,
    FWD_INW,
    FWD_INL,
    FWD_OUTB,
    FWD_OUTW,
    FWD_OUTL,
    FWD_MAP_RB,
    FWD_MAP_RW,
    FWD_MAP_RL,
    FWD_MAP_WB,
    FWD_MAP_WW,
    FWD_MAP_WL,
    FWD_INTVEC,
    FWD_PICINT,
    FWD_SMRAM_BACKUP,
    FWD_SMRAM_RECALC,
    FWD_APICBASE_R,
    FWD_APICBASE_W,
    FWD_CPUID_APICID,
    FWD_PHYS_WB,
    FWD_PHYS_WL,
    FWD_SPLIT_BEGIN,
    FWD_SPLIT_END
};

typedef struct cpu_thread_req_t {
    int            state;
    int            kind;
    mem_mapping_t *map;
    uint32_t       addr;
    uint32_t       val;
    uint32_t       result;
    void          *ptr;
    uint64_t       val64;
    uint64_t       result64;
    int32_t        charge;    /* cycles the device took, the asking core's to pay */
    uint32_t       map_gen;   /* mem_mapping_gen when a memory access was handed over */
    int            discarded; /* dropped: a RESET is on its way */
} cpu_thread_req_t;

/* One of the other processors: its core, its thread, and everything the
   machine's thread and its own share about it. */
typedef struct cpu_ap_t {
    const cpu_core_ops_t *core;
    thread_t             *handle;
    event_t              *wake;        /* to its thread */
    int                   exit;
    int                   exited;
    int                   park_req;    /* it is to park: park_leave, or held */
    int                   park_leave;  /* the machine's thread is outside cpu_exec */
    int                   park_seq;    /* counts the machine's thread's asks */
    int                   parked_seq;  /* the last ask it is parked for */
    int                   park_wanted; /* the ask the machine's thread waits on */
    int                   sleeping;    /* its thread is asleep on wake */
    int                   hold_depth;  /* cpu_thread_hold_mask nesting, the machine's thread's */
    int                   hold_parked; /* the hold asked for the park */
    int                   hold_quiet;  /* the hold found it asleep idle and did not wake it */
    cpu_thread_req_t      req;

    /* Something for it to look at: bumped by whoever posts to it; its
       thread leaves its blocks when the count moves. */
    int attn;
    int attn_seen;

    /* Its time as last published, and whether it is idle (AP_*). */
    uint64_t time;
    int      idle;

    /* The machine's thread gave it work while it was idle (the poke's
       count): until its thread has taken that, the machine's thread counts
       it as running from the moment of the wake, so it cannot run ahead
       while the host takes its time waking the thread. Guest time for the
       processor starts at the wake, as it would on the board. */
    int woken;

    /* Its core's TLB set, whose attn flag its blocks look at. */
    mem_tlb_t *tlb;

    /* Its locked instruction (LOCK, or XCHG with memory), its thread's
       only: its first device access stops the others until the instruction
       ends, so the handler's read and write are one bus operation (Pentium
       Vol. 3, 19.1: "requests for control of the bus are ignored during
       locked cycles"). */
    int locked_ins;
    int locked_split;

    unsigned int cfg_taken;
} cpu_ap_t;

static cpu_ap_t cpu_aps[CPU_MP_MAX];        /* [0] unused: the first is the machine's thread */
static int      cpu_ap_started;             /* how many of them have a thread */
static event_t *cpu_thread_notify;          /* to the machine's thread */
static int      cpu_thread_machine_asleep;  /* the machine's thread is asleep on notify */
static uint64_t cpu_thread_fpenv;           /* the machine thread's MXCSR, or FPCR on ARM64 */

/* The host FPU control register each processor thread takes from the
   machine's: rounding, flush to zero and the exception masks the
   recompiler brackets its own changes with. */
static inline uint64_t
cpu_thread_fpenv_get(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return _mm_getcsr();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static inline void
cpu_thread_fpenv_set(uint64_t v)
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr((unsigned int) v);
#elif defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#else
    (void) v;
#endif
}

/* The bound W, in cycles. */
static int64_t cpu_thread_bound;

/* The machine's settings the other cores copy (cpu_core.h), published by
   the machine's thread under a sequence count. */
static cpu_core_config_t cpu_thread_cfg;
static unsigned int      cpu_thread_cfg_seq;

/* The machine core's TLB set: its attn flag makes it serve a device access
   handed to it at its next block boundary (cpu_thread_main_poll). */
static mem_tlb_t *cpu_thread_c0_tlb;

/* Which processor the calling thread runs: 0 on the machine's thread. */
static __thread int cpu_thread_self_index;

/* Sequentially consistent: each side stores "asleep" then looks for
   work, the other stores work then looks for "asleep"; with anything
   weaker x86 lets both loads pass both stores and each misses the other. */
static inline int
ld(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline void
st(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static inline int64_t
ld64(const int64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline int
cpu_ap_live(int i)
{
    return (i > 0) && (i < CPU_MP_MAX) && (cpu_aps[i].handle != NULL);
}

/* ---- the machine's thread doing another processor's device work -------- */

extern int ioapic_external_interrupt_pending(void);
extern int ioapic_external_interrupt_vector(void);

/* An interrupt for the processor in place (cpu_mp_cur) taken: its APIC's
   IRR into ISR, or the 8259's vector through its LINT0; -1 if there is
   none now. */
static int
cpu_thread_take_vector(void)
{
    if (ioapic_external_interrupt_pending())
        return ioapic_external_interrupt_vector();
    return -1;
}

static void cpu_thread_hold_mask(unsigned int mask);
static void cpu_thread_release_mask(unsigned int mask);

#define CPU_AP_ALL (((1u << CPU_MP_MAX) - 1u) & ~1u)

static void
cpu_thread_serve(int index)
{
    cpu_ap_t         *ap      = &cpu_aps[index];
    cpu_thread_req_t *r       = &ap->req;
    const int         was     = cpu_mp_cur;
    void (*fill_r)(uint32_t)  = mem_fill_read_running;
    void (*fill_w)(uint32_t)  = mem_fill_write_running;
    const int32_t     cyc     = cycles;

    /* Asked for before a RESET of the processor was taken: the access never
       happens, as a cycle cut off by RESET. An INIT waits for the
       instruction boundary, so the access completes. */
    if (cpu_mp_ap_hard_reset_outstanding(index)) {
        r->result    = 0xffffffff;
        r->result64  = 0xffffffffffffffffULL;
        r->charge    = 0;
        r->discarded = 1;
        st(&r->state, REQ_DONE);
        return;
    }
    r->discarded = 0;

    /* A locked operation across a page or a 64-byte line, or on device
       memory: the first processor stays stopped here, between its blocks,
       and every other one parked, until the operation's end is posted, as
       LOCK# holds the bus (Pentium Vol. 3, 19.1.1). This one's accesses
       meanwhile (device memory's reads and writes) are served here. */
    if (r->kind == FWD_SPLIT_BEGIN) {
        const unsigned int others = CPU_AP_ALL & ~(1u << index);

        cpu_thread_hold_mask(others);
        r->charge = 0;
        st(&r->state, REQ_DONE);
        for (;;) {
            int spins = 0;

            /* A region is a few instructions; a host that takes the
               thread away inside one is waited for asleep, as
               cpu_thread_machine_wait waits: its next request wakes this. */
            while ((ld(&r->state) != REQ_POSTED) && !ld(&ap->exited)) {
                if (++spins < CPU_THREAD_SPINS) {
                    thread_spin_pause();
                    continue;
                }
                st(&cpu_thread_machine_asleep, 1);
                if ((ld(&r->state) != REQ_POSTED) && !ld(&ap->exited)) {
                    (void) thread_wait_event(cpu_thread_notify, 1);
                    thread_reset_event(cpu_thread_notify); /* manual-reset on unix */
                }
                st(&cpu_thread_machine_asleep, 0);
                spins = 0;
            }
            if ((r->kind == FWD_SPLIT_END) || ld(&ap->exited))
                break;
            cpu_thread_serve(index);
        }
        cpu_thread_release_mask(others);
        r->charge = 0;
        /* A RESET the region's own accesses raised ends the instruction
           there: "terminate all execution and bus cycle activity within 2
           clocks" (Pentium Vol. 1, 3.3). */
        r->discarded = cpu_mp_ap_hard_reset_outstanding(index);
        st(&r->state, REQ_DONE);
        return;
    }

    /* The map was rebuilt since the access was looked up: look it up
       again, as the processor would now. Nothing mapped there: a read
       floats high, a write goes nowhere. */
    if ((r->kind >= FWD_MAP_RB) && (r->kind <= FWD_MAP_WL) &&
        (r->map_gen != __atomic_load_n(&mem_mapping_gen, __ATOMIC_SEQ_CST))) {
        r->map = mem_mapping_at(index, r->kind >= FWD_MAP_WB, r->addr);
        if ((r->map == NULL) ||
            ((r->kind == FWD_MAP_RB) && !r->map->read_b) || ((r->kind == FWD_MAP_RW) && !r->map->read_w) ||
            ((r->kind == FWD_MAP_RL) && !r->map->read_l) || ((r->kind == FWD_MAP_WB) && !r->map->write_b) ||
            ((r->kind == FWD_MAP_WW) && !r->map->write_w) || ((r->kind == FWD_MAP_WL) && !r->map->write_l)) {
            r->result = 0xffffffff;
            r->charge = 0;
            st(&r->state, REQ_DONE);
            return;
        }
    }

    /* The device code runs for this processor: which processor is asking,
       and whose TLB a RAM handler fills, are its own. */
    cpu_mp_cur             = index;
    mem_fill_read_running  = ap->core->fill_read;
    mem_fill_write_running = ap->core->fill_write;

    switch (r->kind) {
        case FWD_INB:
            r->result = inb((uint16_t) r->addr);
            break;
        case FWD_INW:
            r->result = inw((uint16_t) r->addr);
            break;
        case FWD_INL:
            r->result = inl((uint16_t) r->addr);
            break;
        case FWD_OUTB:
            outb((uint16_t) r->addr, (uint8_t) r->val);
            break;
        case FWD_OUTW:
            outw((uint16_t) r->addr, (uint16_t) r->val);
            break;
        case FWD_OUTL:
            outl((uint16_t) r->addr, r->val);
            break;
        case FWD_MAP_RB:
            r->result = r->map->read_b(r->addr, r->map->priv);
            break;
        case FWD_MAP_RW:
            r->result = r->map->read_w(r->addr, r->map->priv);
            break;
        case FWD_MAP_RL:
            r->result = r->map->read_l(r->addr, r->map->priv);
            break;
        case FWD_MAP_WB:
            r->map->write_b(r->addr, (uint8_t) r->val, r->map->priv);
            break;
        case FWD_MAP_WW:
            r->map->write_w(r->addr, (uint16_t) r->val, r->map->priv);
            break;
        case FWD_MAP_WL:
            r->map->write_l(r->addr, r->val, r->map->priv);
            break;
        case FWD_INTVEC:
            r->result = (uint32_t) cpu_thread_take_vector();
            break;
        case FWD_PICINT:
            picint_common((uint16_t) r->addr, r->val & 1, (r->val >> 1) & 1, (uint8_t *) r->ptr);
            break;
        case FWD_SMRAM_BACKUP:
            smram_backup_all();
            break;
        case FWD_SMRAM_RECALC:
            smram_recalc_all((int) r->val);
            break;
        case FWD_APICBASE_R:
            r->result64 = cpu_apic_base_msr_read_current();
            break;
        case FWD_APICBASE_W:
            cpu_apic_base_msr_write_current(r->val64);
            break;
        case FWD_CPUID_APICID:
            r->result = cpu_cpuid_apic_id_ebx();
            break;
        case FWD_PHYS_WB:
            mem_writeb_phys(r->addr, (uint8_t) r->val);
            break;
        case FWD_PHYS_WL:
            mem_writel_phys(r->addr, r->val);
            break;
        default:
            break;
    }

    cpu_mp_cur             = was;
    mem_fill_read_running  = fill_r;
    mem_fill_write_running = fill_w;

    /* What the device charged was the asking processor's time. */
    r->charge = cyc - cycles;
    cycles    = cyc;

    /* A reset, NMI or SMI the access raised, done before the processor
       goes on: a reset it caused reaches its mailbox first, and a RESET
       ends the instruction that caused it (r->discarded). */
    cpu_mp_after_serve();
    if (cpu_mp_ap_hard_reset_outstanding(index))
        r->discarded = 1;

    st(&r->state, REQ_DONE);
}

/* Serve whatever the others have asked for; answers whether anything
   was. */
static int
cpu_thread_serve_posted(void)
{
    int served = 0;

    for (int i = 1; i < CPU_MP_MAX; i++) {
        if (!cpu_aps[i].core)
            continue;
        if (ld(&cpu_aps[i].req.state) == REQ_POSTED) {
            cpu_thread_serve(i);
            served = 1;
        }
    }
    return served;
}

/* The machine's thread between two of its blocks, and in every wait. */
void
cpu_thread_main_poll(void)
{
    (void) cpu_thread_serve_posted();
    cpu_mp_after_serve(); /* an NMI, SMI or INIT for the first that came meanwhile */
}

/* Something is waiting for the machine's thread between its blocks
   (cpu_thread_main_poll) that is no request: it looks there soon, and is
   woken if it sleeps. From any thread. */
void
cpu_thread_machine_attn(void)
{
    if (cpu_thread_c0_tlb != NULL)
        __atomic_store_n(&cpu_thread_c0_tlb->attn, 1, __ATOMIC_SEQ_CST);
    if (ld(&cpu_thread_machine_asleep))
        thread_set_event(cpu_thread_notify);
}

/* ---- another processor's thread asking for it --------------------------- */

static uint32_t cpu_thread_forward(int kind, mem_mapping_t *map, uint32_t addr, uint32_t val);

/* Its locked instruction begins and ends (x86_ops_misc.h opLOCK, and XCHG
   with memory, x86_ops_xchg.h). */
void
cpu_thread_locked_begin(void)
{
    if (cpu_thread_self_index)
        cpu_aps[cpu_thread_self_index].locked_ins = 1;
}

void
cpu_thread_locked_end(void)
{
    cpu_ap_t *ap;

    if (!cpu_thread_self_index)
        return;
    ap             = &cpu_aps[cpu_thread_self_index];
    ap->locked_ins = 0;
    if (ap->locked_split) {
        ap->locked_split = 0;
        (void) cpu_thread_forward(FWD_SPLIT_END, NULL, 0, 0);
    }
}

static uint32_t
cpu_thread_forward(int kind, mem_mapping_t *map, uint32_t addr, uint32_t val)
{
    cpu_ap_t         *ap = &cpu_aps[cpu_thread_self_index];
    cpu_thread_req_t *r  = &ap->req;

    /* A locked instruction's first device access: the others are stopped
       from here to the instruction's end (cpu_thread_locked_end). */
    if (ap->locked_ins && !ap->locked_split && (kind >= FWD_MAP_RB) && (kind <= FWD_MAP_WL)) {
        ap->locked_split = 1;
        (void) cpu_thread_forward(FWD_SPLIT_BEGIN, NULL, 0, 0);
    }

    r->kind    = kind;
    r->map     = map;
    r->addr    = addr;
    r->val     = val;
    r->map_gen = __atomic_load_n(&mem_mapping_gen, __ATOMIC_SEQ_CST);

    /* Waiting, this core uses no TLB entry; the machine's thread may flush
       it meanwhile, and that flush is taken before going on. */
    ap->core->tlb_outside(1);
    st(&r->state, REQ_POSTED);
    __atomic_store_n(&cpu_thread_c0_tlb->attn, 1, __ATOMIC_SEQ_CST);
    if (ld(&cpu_thread_machine_asleep))
        thread_set_event(cpu_thread_notify);

    /* The machine's thread answers between two of its blocks, or from
       whatever it is waiting in; asleep, the event wakes it. The emulator
       closing without the machine's thread having stopped (the SDL
       frontend): no answer is coming, so the wait ends. */
    while ((ld(&r->state) != REQ_DONE) && !ld(&ap->exit))
        thread_spin_pause();

    st(&r->state, REQ_NONE);
    ap->core->tlb_outside(0);
    if (r->charge > 0)
        ap->core->charge(r->charge);
    /* Dropped for a RESET on its way: its run ends here, not after more
       instructions on data that never came. */
    if (r->discarded)
        ap->core->discard();

    return r->result;
}

/* Plain RAM the other cores take themselves (mem_cpu.c MAP_READ_*); what
   comes here is anything else, done on the machine's thread. The other
   cores' calls to these by name are redirected here at link time
   (cpu_ap_rename.cmake). On the machine's thread -- a core taking its turn
   there -- a plain call. */

uint8_t
cpu_ap_inb(uint16_t port)
{
    if (!cpu_thread_self_index)
        return inb(port);
    return (uint8_t) cpu_thread_forward(FWD_INB, NULL, port, 0);
}

uint16_t
cpu_ap_inw(uint16_t port)
{
    if (!cpu_thread_self_index)
        return inw(port);
    return (uint16_t) cpu_thread_forward(FWD_INW, NULL, port, 0);
}

uint32_t
cpu_ap_inl(uint16_t port)
{
    if (!cpu_thread_self_index)
        return inl(port);
    return cpu_thread_forward(FWD_INL, NULL, port, 0);
}

void
cpu_ap_outb(uint16_t port, uint8_t val)
{
    if (!cpu_thread_self_index)
        outb(port, val);
    else
        (void) cpu_thread_forward(FWD_OUTB, NULL, port, val);
}

void
cpu_ap_outw(uint16_t port, uint16_t val)
{
    if (!cpu_thread_self_index)
        outw(port, val);
    else
        (void) cpu_thread_forward(FWD_OUTW, NULL, port, val);
}

void
cpu_ap_outl(uint16_t port, uint32_t val)
{
    if (!cpu_thread_self_index)
        outl(port, val);
    else
        (void) cpu_thread_forward(FWD_OUTL, NULL, port, val);
}

/* Machine work the other cores ask for by name: the 8259 (FERR's IRQ 13),
   SMRAM's place in the map as SMM is entered and left, and the local
   APIC's base and ID, which the APIC code finds by the processor asking. */
void
cpu_ap_picint_common(uint16_t num, int level, int set, uint8_t *irq_state)
{
    if (!cpu_thread_self_index) {
        picint_common(num, level, set, irq_state);
        return;
    }
    cpu_aps[cpu_thread_self_index].req.ptr = irq_state;
    (void) cpu_thread_forward(FWD_PICINT, NULL, num, (level ? 1 : 0) | (set ? 2 : 0));
}

void
cpu_ap_smram_backup_all(void)
{
    if (!cpu_thread_self_index)
        smram_backup_all();
    else
        (void) cpu_thread_forward(FWD_SMRAM_BACKUP, NULL, 0, 0);
}

void
cpu_ap_smram_recalc_all(int ret)
{
    if (!cpu_thread_self_index)
        smram_recalc_all(ret);
    else
        (void) cpu_thread_forward(FWD_SMRAM_RECALC, NULL, 0, (uint32_t) ret);
}

uint64_t
cpu_ap_cpu_apic_base_msr_read_current(void)
{
    if (!cpu_thread_self_index)
        return cpu_apic_base_msr_read_current();
    (void) cpu_thread_forward(FWD_APICBASE_R, NULL, 0, 0);
    return cpu_aps[cpu_thread_self_index].req.result64;
}

void
cpu_ap_cpu_apic_base_msr_write_current(uint64_t val)
{
    if (!cpu_thread_self_index) {
        cpu_apic_base_msr_write_current(val);
        return;
    }
    cpu_aps[cpu_thread_self_index].req.val64 = val;
    (void) cpu_thread_forward(FWD_APICBASE_W, NULL, 0, 0);
}

uint32_t
cpu_ap_cpu_cpuid_apic_id_ebx(void)
{
    if (!cpu_thread_self_index)
        return cpu_cpuid_apic_id_ebx();
    return cpu_thread_forward(FWD_CPUID_APICID, NULL, 0, 0);
}

/* A locked operation across a page or a 64-byte line
   (mem_split_lock_take): the other processors are kept off the bus until
   it is done -- held, from the first; stopped between its blocks with the
   rest parked, from another (cpu_thread_serve). */
void
cpu_thread_split_begin(void)
{
    if (!cpu_ap_started)
        return;
    if (cpu_thread_self_index)
        (void) cpu_thread_forward(FWD_SPLIT_BEGIN, NULL, 0, 0);
    else
        cpu_thread_hold();
}

void
cpu_thread_split_end(void)
{
    if (!cpu_ap_started)
        return;
    if (cpu_thread_self_index)
        (void) cpu_thread_forward(FWD_SPLIT_END, NULL, 0, 0);
    else
        cpu_thread_release();
}

/* A physical write another processor's core makes outside its paging
   (the TR5 cache test's fill): done on the machine's thread, as the
   board's bus-master accesses are. */
void
cpu_ap_mem_writeb_phys(uint32_t addr, uint8_t val)
{
    if (!cpu_thread_self_index)
        mem_writeb_phys(addr, val);
    else
        (void) cpu_thread_forward(FWD_PHYS_WB, NULL, addr, val);
}

void
cpu_ap_mem_writel_phys(uint32_t addr, uint32_t val)
{
    if (!cpu_thread_self_index)
        mem_writel_phys(addr, val);
    else
        (void) cpu_thread_forward(FWD_PHYS_WL, NULL, addr, val);
}

/* Another processor taking an interrupt (x86_interrupt.h): its APIC's IRR
   into ISR, or an 8259 acknowledge through its LINT0, both on the
   machine's thread, where that state is kept. */
int
cpu_ap_interrupt_vector(void)
{
    if (!cpu_thread_self_index)
        return cpu_thread_take_vector();
    return (int) cpu_thread_forward(FWD_INTVEC, NULL, 0, 0);
}

/* Device memory, its local APIC's window among it. */
uint8_t
cpu_ap_map_read_b(mem_mapping_t *m, uint32_t a)
{
    if (!cpu_thread_self_index)
        return m->read_b(a, m->priv);
    return (uint8_t) cpu_thread_forward(FWD_MAP_RB, m, a, 0);
}

uint16_t
cpu_ap_map_read_w(mem_mapping_t *m, uint32_t a)
{
    if (!cpu_thread_self_index)
        return m->read_w(a, m->priv);
    return (uint16_t) cpu_thread_forward(FWD_MAP_RW, m, a, 0);
}

uint32_t
cpu_ap_map_read_l(mem_mapping_t *m, uint32_t a)
{
    if (!cpu_thread_self_index)
        return m->read_l(a, m->priv);
    return cpu_thread_forward(FWD_MAP_RL, m, a, 0);
}

void
cpu_ap_map_write_b(mem_mapping_t *m, uint32_t a, uint8_t v)
{
    if (!cpu_thread_self_index)
        m->write_b(a, v, m->priv);
    else
        (void) cpu_thread_forward(FWD_MAP_WB, m, a, v);
}

void
cpu_ap_map_write_w(mem_mapping_t *m, uint32_t a, uint16_t v)
{
    if (!cpu_thread_self_index)
        m->write_w(a, v, m->priv);
    else
        (void) cpu_thread_forward(FWD_MAP_WW, m, a, v);
}

void
cpu_ap_map_write_l(mem_mapping_t *m, uint32_t a, uint32_t v)
{
    if (!cpu_thread_self_index)
        m->write_l(a, v, m->priv);
    else
        (void) cpu_thread_forward(FWD_MAP_WL, m, a, v);
}

/* ---- the other processors' threads -------------------------------------- */

/* Processor index's thread looks at its mailbox now: from any thread. */
void
cpu_thread_poke(int index)
{
    cpu_ap_t *ap;

    if (!cpu_ap_live(index))
        return;
    ap = &cpu_aps[index];
    __atomic_fetch_add(&ap->attn, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ap->tlb->attn, 1, __ATOMIC_SEQ_CST);
    if (ld(&ap->sleeping))
        thread_set_event(ap->wake);
}

void
cpu_thread_poke_all(void)
{
    for (int i = 1; i < CPU_MP_MAX; i++)
        cpu_thread_poke(i);
}

/* Processor index has work now -- a STARTUP, NMI or SMI, an interrupt for
   it halted: from any thread. From the machine's thread, and while it was
   idle, the machine holds itself to the bound from now. */
void
cpu_thread_wake(int index)
{
    cpu_ap_t *ap;
    int       a;

    if (!cpu_ap_live(index))
        return;
    ap = &cpu_aps[index];
    if (cpu_thread_self_index || (ld(&ap->idle) == AP_RUNNING)) {
        cpu_thread_poke(index);
        return;
    }
    /* The time of the first wake holds until its thread takes it: woken
       again meanwhile (each slice while it has not run), it is only poked. */
    if (ld(&ap->woken)) {
        cpu_thread_poke(index);
        return;
    }
    __atomic_store_n(&ap->time, tsc, __ATOMIC_SEQ_CST);
    a = __atomic_add_fetch(&ap->attn, 1, __ATOMIC_SEQ_CST);
    if (a == 0) /* 0 means no wake */
        a = __atomic_add_fetch(&ap->attn, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ap->tlb->attn, 1, __ATOMIC_SEQ_CST);
    st(&ap->woken, a);
    if (ld(&ap->sleeping))
        thread_set_event(ap->wake);
}

/* On its thread, once its mailbox was looked at after a wake: the machine
   no longer holds itself for that wake. */
static void
cpu_thread_wake_taken(cpu_ap_t *ap)
{
    int w = ld(&ap->woken);

    /* Counts wrap: compared as a distance, in unsigned arithmetic. */
    if (w && ((int) ((unsigned int) ap->attn_seen - (unsigned int) w) >= 0))
        (void) __atomic_compare_exchange_n(&ap->woken, &w, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void
cpu_thread_tell_machine(void)
{
    if (ld(&cpu_thread_machine_asleep))
        thread_set_event(cpu_thread_notify);
}

/* A processor's thread sleeps until poked, or for at most 1 ms; anything
   it waits for either pokes it or is looked at again then. */
static void
cpu_thread_ap_sleep(cpu_ap_t *ap)
{
    st(&ap->sleeping, 1);
    if ((ld(&ap->attn) == ap->attn_seen) && !ld(&ap->exit)) {
        (void) thread_wait_event(ap->wake, 1);
        thread_reset_event(ap->wake); /* manual-reset on unix */
    }
    st(&ap->sleeping, 0);
}

/* The machine's settings, taken when they have changed. */
static void
cpu_thread_take_config(cpu_ap_t *ap)
{
    cpu_core_config_t c;
    unsigned int      seq = __atomic_load_n(&cpu_thread_cfg_seq, __ATOMIC_ACQUIRE);

    if ((seq == ap->cfg_taken) || (seq & 1))
        return;
    memcpy(&c, &cpu_thread_cfg, sizeof(c));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&cpu_thread_cfg_seq, __ATOMIC_RELAXED) != seq)
        return; /* changed while copied: next time */
    ap->core->config_set(&c);
    ap->cfg_taken = seq;
}

static void
cpu_thread_main(void *priv)
{
    const int index         = (int) (intptr_t) priv;
    cpu_ap_t *ap            = &cpu_aps[index];
    int       prepared      = AP_IDLE_OTHER;
    int       prepared_attn = 0;

    /* The emulation thread's own settings: this thread runs processor
       code, and the recompiler brackets its rounding changes with the
       MXCSR it found. */
    is_cpu_thread         = 1;
    cpu_thread_self_index = index;
    cpu_thread_fpenv_set(cpu_thread_fpenv);
#ifdef __APPLE__
    /* As the emulation thread (qt_main.cpp): on a performance core. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#ifdef _WIN32
    /* The emulation thread runs at the highest priority (qt_main.cpp);
       each spins while it waits for another, so this one must not be
       preempted by it. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif

    for (;;) {
        int64_t  ahead;
        int64_t  run;
        int64_t  bound;
        uint64_t now;
        int      idle;

        /* The flag its blocks leave for is cleared before the mailbox is
           looked at: anything posted after this sets it again. */
        __atomic_store_n(&ap->tlb->attn, 0, __ATOMIC_SEQ_CST);
        ap->attn_seen = ld(&ap->attn);

        if (ld(&ap->exit))
            break;

        /* Parked: at a block boundary, outside its blocks, until let go. */
        if (ld(&ap->park_req)) {
            /* Answering each ask by its number: a leave that follows an
               enter at once is never answered by the park before it. */
            while (ld(&ap->park_req) && !ld(&ap->exit)) {
                int seq = ld(&ap->park_seq);
                int spins;

                if (ld(&ap->parked_seq) != seq) {
                    st(&ap->parked_seq, seq);
                    cpu_thread_tell_machine();
                }
                /* A hold (a map rebuild) lets go within microseconds: spin
                   a little before paying for a wake. */
                for (spins = 0; (spins < (CPU_THREAD_SPINS / 10)) && ld(&ap->park_req) &&
                                (ld(&ap->park_seq) == seq) && !ld(&ap->exit); spins++)
                    thread_spin_pause();
                if (!ld(&ap->park_req) || (ld(&ap->park_seq) != seq))
                    continue;
                cpu_thread_ap_sleep(ap);
                ap->attn_seen = ld(&ap->attn);
            }
            continue;
        }

        cpu_thread_take_config(ap);

        /* Its mailbox, and whether it has anything to run: looked at only
           when something was posted since, or it was not simply running
           last time. Everything posted to it pokes it (and a reset it
           asks for itself too), so between runs of an unhalted processor
           with nothing new there is nothing to look at. */
        if ((prepared == AP_RUNNING) && (prepared_attn == ap->attn_seen) && !ap->core->halted_get())
            idle = AP_RUNNING;
        else {
            idle          = cpu_mp_ap_prepare(index);
            prepared      = idle;
            prepared_attn = ap->attn_seen;
        }
        if (idle != AP_RUNNING) {
            if (ld(&ap->idle) != idle) {
                st(&ap->idle, idle);
                cpu_thread_tell_machine();
            }
            if (ld(&ap->woken)) {
                cpu_thread_wake_taken(ap); /* woken for nothing it can run */
                cpu_thread_tell_machine();
            }
            cpu_thread_ap_sleep(ap);
            continue;
        }

        /* Running: its time first, then that it runs, so the machine's
           thread never measures it by a time from before it slept. */
        now = ap->core->tsc_get();
        __atomic_store_n(&ap->time, now, __ATOMIC_SEQ_CST);
        if (ld(&ap->idle) != AP_RUNNING)
            st(&ap->idle, AP_RUNNING);
        if (ld(&ap->woken))
            cpu_thread_wake_taken(ap); /* its own time is published: it counts now */

        /* No further than W ahead of the machine's clock. Close to it, it
           waits until it is half W ahead or less and then runs half W at
           once: following the machine's clock a sliver at a time would
           spend its thread on getting ready to run. */
        bound = ld64(&cpu_thread_bound);
        ahead = (int64_t) (now - __atomic_load_n(&tsc, __ATOMIC_SEQ_CST));
        run   = bound - ahead;
        if (run < (bound >> 2)) {
            int spins;

            cpu_thread_tell_machine();
            for (spins = 0; spins < CPU_THREAD_SPINS; spins++) {
                if ((ld(&ap->attn) != ap->attn_seen) ||
                    ((int64_t) (now - __atomic_load_n(&tsc, __ATOMIC_SEQ_CST)) <= (bound >> 1)))
                    break;
                thread_spin_pause();
            }
            if (spins == CPU_THREAD_SPINS)
                cpu_thread_ap_sleep(ap);
            continue;
        }
        if (run > (bound >> 1))
            run = bound >> 1;
        if (run < 1)
            run = 1;

        (void) ap->core->run((int32_t) run);
        /* Left early for its mailbox: what it did not run is not owed. */
        ap->core->trim();

        __atomic_store_n(&ap->time, ap->core->tsc_get(), __ATOMIC_SEQ_CST);
        cpu_mp_ap_after_run(index);
        cpu_thread_tell_machine();
    }

    st(&ap->exited, 1);
    thread_set_event(cpu_thread_notify);
}

/* Whether the other processors run on threads of their own. */
int
cpu_thread_active(void)
{
    return cpu_ap_started > 0;
}

/* The processor the calling thread runs: 0 on the machine's thread. */
int
cpu_thread_self(void)
{
    return cpu_thread_self_index;
}

/* The machine's thread waits for `done`, serving the others' device work
   meanwhile: spins, then sleeps until told. */
static void
cpu_thread_machine_wait(int (*done)(void))
{
    int spins = 0;

    for (;;) {
        if (cpu_thread_serve_posted()) {
            spins = 0;
            continue;
        }
        if (done())
            break;
        if (++spins < CPU_THREAD_SPINS) {
            thread_spin_pause();
            continue;
        }
        /* Asleep only after saying so, and only if nothing arrived in
           between; the other side tells after every change. */
        st(&cpu_thread_machine_asleep, 1);
        {
            int posted = 0;

            for (int i = 1; i < CPU_MP_MAX; i++)
                posted |= cpu_aps[i].core && (ld(&cpu_aps[i].req.state) == REQ_POSTED);
            if (!done() && !posted) {
                (void) thread_wait_event(cpu_thread_notify, 1);
                thread_reset_event(cpu_thread_notify); /* manual-reset on unix */
            }
        }
        st(&cpu_thread_machine_asleep, 0);
        spins = 0;
    }
}

static int
cpu_thread_ap_parked(const cpu_ap_t *ap)
{
    /* A thread that has exited is as parked as it gets. */
    return (ld(&ap->parked_seq) == ap->park_wanted) || ld(&ap->exited);
}

static int
cpu_thread_all_parked(void)
{
    for (int i = 1; i < CPU_MP_MAX; i++)
        if (cpu_ap_live(i) && !cpu_thread_ap_parked(&cpu_aps[i]))
            return 0;
    return 1;
}

/* Not so far behind the machine's clock that the machine has to wait. */
static int
cpu_thread_ap_in_bound(const cpu_ap_t *ap)
{
    /* A thread that has exited holds nothing back. */
    if (ld(&ap->exited))
        return 1;
    if ((ld(&ap->idle) != AP_RUNNING) && !ld(&ap->woken))
        return 1;
    return (int64_t) (tsc - __atomic_load_n(&ap->time, __ATOMIC_SEQ_CST)) <= ld64(&cpu_thread_bound);
}

static int
cpu_thread_all_in_bound(void)
{
    for (int i = 1; i < CPU_MP_MAX; i++)
        if (cpu_ap_live(i) && !cpu_thread_ap_in_bound(&cpu_aps[i]))
            return 0;
    return 1;
}

/* The machine's settings for the other cores, when they have changed. */
static void
cpu_thread_publish_config(void)
{
    cpu_core_config_t c;

    memset(&c, 0, sizeof(c));
    cpu_core_ops()->config_get(&c);
    if (!memcmp(&c, &cpu_thread_cfg, sizeof(c)))
        return;

    __atomic_fetch_add(&cpu_thread_cfg_seq, 1, __ATOMIC_SEQ_CST); /* odd: being written */
    memcpy(&cpu_thread_cfg, &c, sizeof(c));
    __atomic_fetch_add(&cpu_thread_cfg_seq, 1, __ATOMIC_SEQ_CST);
}

/* What a processor is to do about parking, from its leave flag and its
   holds: the machine's thread only. */
static void
cpu_thread_park_update(cpu_ap_t *ap)
{
    st(&ap->park_req, ap->park_leave || (ap->hold_parked && (ap->hold_depth > 0)));
}

/* The machine's thread enters cpu_exec: the others may run, within `bound`
   cycles of the machine's clock. */
void
cpu_thread_enter(int64_t bound)
{
    if (bound < 2)
        bound = 2;
    __atomic_store_n(&cpu_thread_bound, bound, __ATOMIC_SEQ_CST);
    cpu_thread_publish_config();
    for (int i = 1; i < CPU_MP_MAX; i++) {
        if (!cpu_ap_live(i))
            continue;
        cpu_aps[i].park_leave = 0;
        cpu_thread_park_update(&cpu_aps[i]);
        cpu_thread_poke(i);
    }
}

/* And leaves it: every one parks, and nothing of the machine is touched
   from their threads until the next enter. */
void
cpu_thread_leave(void)
{
    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_ap_t *ap = &cpu_aps[i];

        if (!cpu_ap_live(i))
            continue;
        ap->park_wanted = __atomic_add_fetch(&ap->park_seq, 1, __ATOMIC_SEQ_CST);
        ap->park_leave  = 1;
        cpu_thread_park_update(ap);
        cpu_thread_poke(i);
    }
    cpu_thread_machine_wait(cpu_thread_all_parked);
}

/* The machine's thread keeps the processors in mask out of the machine's
   shared tables while it rebuilds them (the memory map, a board reset), or
   off the bus for a locked operation: each parked at a block boundary, or
   waiting on a device access, which it stays in until the machine's
   thread serves it -- after the release. Nothing is served while held:
   the machine's thread is inside device code. Nestable; one parked
   already (outside cpu_exec) is left so. */
static void
cpu_thread_hold_mask(unsigned int mask)
{
    unsigned int wait  = 0;
    int          spins = 0;

    if (!cpu_ap_started || cpu_thread_self_index)
        return;

    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_ap_t *ap = &cpu_aps[i];

        if (!(mask & (1u << i)) || !cpu_ap_live(i))
            continue;
        if (ap->hold_depth++ > 0)
            continue;
        if (ap->park_leave) {
            ap->hold_parked = 0; /* outside cpu_exec: parked for the leave */
            continue;
        }
        ap->hold_parked = 1;
        ap->park_wanted = __atomic_add_fetch(&ap->park_seq, 1, __ATOMIC_SEQ_CST);
        cpu_thread_park_update(ap);
        /* Asleep with nothing to run (halted, or waiting for a STARTUP),
           it cannot touch memory, and when it wakes it looks at park_req
           before it runs anything (the top of its loop). The request is
           stored before its sleep is looked at, as it stores its sleep
           before it looks at the request: one of the two sees the other.
           So it is not woken just to be held -- two host wakes for each
           locked operation across a line while it idles. */
        ap->hold_quiet = ld(&ap->sleeping) && (ld(&ap->idle) != AP_RUNNING);
        if (ap->hold_quiet)
            continue;
        cpu_thread_poke(i);
        wait |= 1u << i;
    }

    while (wait) {
        for (int i = 1; i < CPU_MP_MAX; i++) {
            if ((wait & (1u << i)) &&
                (cpu_thread_ap_parked(&cpu_aps[i]) || (ld(&cpu_aps[i].req.state) == REQ_POSTED)))
                wait &= ~(1u << i);
        }
        if (!wait)
            break;
        /* One may be waiting for this core's boundary (a write flush it
           left here, mem_flush_write_page): take the flush and count it,
           as that wait does, or each waits for the other. */
        if (__atomic_load_n(&mem_tlb_cur->stale, __ATOMIC_SEQ_CST) & MEM_TLB_STALE_WRITE) {
            __atomic_fetch_and(&mem_tlb_cur->stale, (uint8_t) ~MEM_TLB_STALE_WRITE, __ATOMIC_SEQ_CST);
            flushmmucache_write_cpu();
            __atomic_store_n(&mem_tlb_cur->epoch, mem_tlb_cur->epoch + 1, __ATOMIC_SEQ_CST);
        }
        if (++spins > CPU_THREAD_SPINS) {
            /* The host took a thread away; let it run to the boundary. */
            for (int i = 1; i < CPU_MP_MAX; i++)
                if (wait & (1u << i))
                    thread_set_event(cpu_aps[i].wake);
            spins = 0;
        }
        thread_spin_pause();
    }
}

static void
cpu_thread_release_mask(unsigned int mask)
{
    if (!cpu_ap_started || cpu_thread_self_index)
        return;

    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_ap_t *ap = &cpu_aps[i];

        if (!(mask & (1u << i)) || !cpu_ap_live(i) || (ap->hold_depth == 0))
            continue;
        if (--ap->hold_depth > 0)
            continue;
        if (ap->hold_parked) {
            ap->hold_parked = 0;
            cpu_thread_park_update(ap);
            /* Not woken for the hold, not woken for its end: a park it
               made meanwhile ends with its next wake, as its idle sleep
               would. */
            if (!ap->hold_quiet)
                cpu_thread_poke(i);
            ap->hold_quiet = 0;
        }
    }
}

void
cpu_thread_hold(void)
{
    cpu_thread_hold_mask(CPU_AP_ALL);
}

void
cpu_thread_release(void)
{
    cpu_thread_release_mask(CPU_AP_ALL);
}

/* The machine's thread after each of the first processor's slices: the
   settings, a sleeping processor that now has work, a reset one of them
   asked for, and the bound. */
void
cpu_thread_tick(void)
{
    cpu_thread_publish_config();
    cpu_mp_after_serve();

    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_ap_t *ap = &cpu_aps[i];

        if (!cpu_ap_live(i))
            continue;
        switch (ld(&ap->idle)) {
            case AP_IDLE_HALTED:
                if (cpu_mp_ap_wakes(i))
                    cpu_thread_wake(i);
                break;
            case AP_RUNNING:
                /* Waiting for the clock, which has moved. */
                if (ld(&ap->sleeping))
                    thread_set_event(ap->wake);
                break;
            default:
                break; /* poked by whatever it waits for */
        }
    }

    if (!cpu_thread_all_in_bound())
        cpu_thread_machine_wait(cpu_thread_all_in_bound);
}

/* Processor index's core, before its thread starts: where its blocks look
   for attention. */
void
cpu_thread_attach(int index, const cpu_core_ops_t *core)
{
    cpu_ap_t *ap = &cpu_aps[index];

    ap->core = core;
    ap->tlb  = core->tlb_set();
}

/* Processor index gets its thread, parked until the machine's thread first
   enters cpu_exec; on the machine's thread. Answers whether it has one. */
int
cpu_thread_start(int index, const cpu_core_ops_t *core)
{
    cpu_ap_t *ap = &cpu_aps[index];
    char      name[32];

    if ((index <= 0) || (index >= CPU_MP_MAX))
        return 0;
    if (ap->handle != NULL)
        return 1;

    if (cpu_ap_started == 0) {
        cpu_thread_fpenv          = cpu_thread_fpenv_get();
        cpu_thread_machine_asleep = 0;
        cpu_thread_c0_tlb         = cpu_core_ops()->tlb_set();
        cpu_thread_bound          = 2;
        cpu_thread_cfg_seq        = 0;
        memset(&cpu_thread_cfg, 0, sizeof(cpu_thread_cfg));
        if (cpu_thread_notify == NULL)
            cpu_thread_notify = thread_create_event();
    }

    cpu_thread_attach(index, core);
    ap->exit         = 0;
    ap->exited       = 0;
    ap->park_leave   = 1;
    ap->park_req     = 1;
    ap->park_seq     = 0;
    ap->parked_seq   = -1;
    ap->park_wanted  = 0;
    ap->sleeping     = 0;
    ap->attn         = 0;
    ap->attn_seen    = 0;
    ap->time         = 0;
    ap->idle         = AP_IDLE_OTHER;
    ap->woken        = 0;
    ap->hold_depth   = 0;
    ap->hold_parked  = 0;
    ap->hold_quiet   = 0;
    ap->locked_ins   = 0;
    ap->locked_split = 0;
    ap->cfg_taken    = 0;
    memset(&ap->req, 0, sizeof(ap->req));
    if (ap->wake == NULL)
        ap->wake = thread_create_event();

    snprintf(name, sizeof(name), "86Box CPU %i", index);
    ap->handle = thread_create_named(cpu_thread_main, (void *) (intptr_t) index, name);
    if ((ap->handle == NULL) || (ap->handle == (thread_t *) (uintptr_t) -1)) {
        ap->handle = NULL;
        pclog("CPU MP: could not start processor %i's thread\n", index);
        return 0;
    }
    cpu_ap_started++;
    pclog("CPU MP: processor %i runs on a thread of its own, beside the first\n", index);
    return 1;
}

/* And gives them back: outside cpu_exec, so every one is parked. */
void
cpu_thread_stop(void)
{
    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_ap_t *ap = &cpu_aps[i];

        if (ap->handle == NULL)
            continue;
        st(&ap->exit, 1);
        cpu_thread_poke(i);
        thread_set_event(ap->wake);
        while (!ld(&ap->exited)) {
            (void) thread_wait_event(cpu_thread_notify, 10);
            thread_reset_event(cpu_thread_notify);
        }
#ifndef _WIN32
        /* A pthread is joined; _beginthread's handle closes itself. */
        thread_wait(ap->handle);
#endif
        ap->handle = NULL;
        cpu_ap_started--;
        pclog("CPU MP: processor %i's thread has stopped\n", i);
    }
    for (int i = 1; i < CPU_MP_MAX; i++) {
        cpu_aps[i].core = NULL;
        cpu_aps[i].tlb  = NULL;
    }
}
