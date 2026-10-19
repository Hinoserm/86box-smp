/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Copyright 2026 skiretic.
 */
#include <stdint.h>
#include <86box/thread.h> /* thread_spin_pause */
#include <86box/apic_lock.h>
#include <86box/cpu_mp.h>
#include "apic_bus.h"
#include "lapic.h"

/* The APIC bus's lock (86box/apic_lock.h). A thread takes it again as it
   likes: its depth is its own, and only the outermost take and release
   touch the word. Held for a register access or a delivery, a few hundred
   instructions: a waiter spins. */
static int          apic_lock_word;
static __thread int apic_lock_depth;

/* The processor asking, for as long as the lock is held: named by the
   outermost take, and what the APIC code means by "the current processor"
   (cpu_current_lapic_index). A processor's own thread names itself
   (apic_lock_as); every other entry is on the machine's thread, for the
   processor it has in place or is serving. */
int apic_bus_asker;

void
apic_lock_as(int who)
{
    if (apic_lock_depth++)
        return;
    while (__atomic_exchange_n(&apic_lock_word, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&apic_lock_word, __ATOMIC_RELAXED))
            thread_spin_pause();
    }
    apic_bus_asker = who;
}

void
apic_lock(void)
{
    apic_lock_as(cpu_mp_cur);
}

/* Whether anyone holds it: the current processor is known only then. */
int
apic_lock_held(void)
{
    return __atomic_load_n(&apic_lock_word, __ATOMIC_RELAXED);
}

void
apic_unlock(void)
{
    if (--apic_lock_depth)
        return;
    __atomic_store_n(&apic_lock_word, 0, __ATOMIC_RELEASE);
}

/* Let go of it however deep this thread holds it, and take it back as
   deep: around a wait for the other processors to park
   (cpu_mp_hold_others), as one waiting for the lock is inside an
   instruction and parks only after it. Answers the depth. */
int
apic_lock_release_all(void)
{
    const int depth = apic_lock_depth;

    if (depth) {
        apic_lock_depth = 0;
        __atomic_store_n(&apic_lock_word, 0, __ATOMIC_RELEASE);
    }
    return depth;
}

void
apic_lock_reacquire(int depth, int asker)
{
    if (!depth)
        return;
    apic_lock_as(asker);
    apic_lock_depth = depth;
}

apic_bus_result_t
apic_bus_result(apic_bus_status_t status)
{
    apic_bus_result_t result = {
        .status     = status,
        .source_esr = 0,
        .target_esr = 0,
        .accepted   = status == APIC_BUS_ACCEPTED
    };

    /*
     * Default failures are send-side; receive-side helpers override this when
     * a target detects the fault after routing.
     */
    switch (status) {
        case APIC_BUS_NO_TARGET:
        case APIC_BUS_REJECTED:
            result.source_esr = LAPIC_ESR_SEND_ACCEPT;
            break;

        case APIC_BUS_INVALID_VECTOR:
            result.source_esr = LAPIC_ESR_SEND_ILLEGAL_VECTOR;
            break;

        case APIC_BUS_INVALID_REGISTER:
            result.source_esr = LAPIC_ESR_ILLEGAL_REG_ADDR;
            break;

        default:
            break;
    }

    return result;
}

int
apic_bus_result_accepted(apic_bus_result_t result)
{
    return result.accepted && (result.status == APIC_BUS_ACCEPTED);
}

apic_bus_result_t
apic_bus_result_invalid_send_vector(void)
{
    return apic_bus_result(APIC_BUS_INVALID_VECTOR);
}

apic_bus_result_t
apic_bus_result_invalid_recv_vector(void)
{
    apic_bus_result_t result = apic_bus_result(APIC_BUS_INVALID_VECTOR);

    /* Vector decode reached a target, so only the receiving LAPIC owns ESR. */
    result.source_esr = 0;
    result.target_esr = LAPIC_ESR_RECV_ILLEGAL_VECTOR;
    return result;
}

apic_bus_result_t
apic_bus_result_invalid_register(void)
{
    return apic_bus_result(APIC_BUS_INVALID_REGISTER);
}

apic_bus_result_t
apic_bus_result_invalid_mode(void)
{
    return apic_bus_result(APIC_BUS_INVALID_MODE);
}

apic_bus_result_t
apic_bus_result_no_target(void)
{
    return apic_bus_result(APIC_BUS_NO_TARGET);
}

apic_bus_result_t
apic_bus_result_rejected(void)
{
    return apic_bus_result(APIC_BUS_REJECTED);
}

apic_bus_result_t
apic_bus_result_retry(void)
{
    return apic_bus_result(APIC_BUS_RETRY);
}

apic_bus_result_t
apic_bus_result_rejected_recv_accept(void)
{
    apic_bus_result_t result = apic_bus_result_rejected();

    /* Accept rejection after target selection is a receive-side ESR event. */
    result.target_esr = LAPIC_ESR_RECV_ACCEPT;
    return result;
}
