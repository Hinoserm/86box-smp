static int
opCMPXCHG_b_a16(uint32_t fetchdat)
{
    uint8_t temp;
    uint8_t temp2 = AL;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr);
    }
    temp = geteab();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint8_t, 1, seteab, temp, AL, getr8(cpu_reg));
    if (cpu_state.abrt)
        return 1;
    setsub8(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}
static int
opCMPXCHG_b_a32(uint32_t fetchdat)
{
    uint8_t temp;
    uint8_t temp2 = AL;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr);
    }
    temp = geteab();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint8_t, 1, seteab, temp, AL, getr8(cpu_reg));
    if (cpu_state.abrt)
        return 1;
    setsub8(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}

static int
opCMPXCHG_w_a16(uint32_t fetchdat)
{
    uint16_t temp;
    uint16_t temp2 = AX;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr + 1UL);
    }
    temp = geteaw();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint16_t, 2, seteaw, temp, AX, cpu_state.regs[cpu_reg].w);
    if (cpu_state.abrt)
        return 1;
    setsub16(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}
static int
opCMPXCHG_w_a32(uint32_t fetchdat)
{
    uint16_t temp;
    uint16_t temp2 = AX;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr + 1UL);
    }
    temp = geteaw();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint16_t, 2, seteaw, temp, AX, cpu_state.regs[cpu_reg].w);
    if (cpu_state.abrt)
        return 1;
    setsub16(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}

static int
opCMPXCHG_l_a16(uint32_t fetchdat)
{
    uint32_t temp;
    uint32_t temp2 = EAX;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr + 3UL);
    }
    temp = geteal();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint32_t, 4, seteal, temp, EAX, cpu_state.regs[cpu_reg].l);
    if (cpu_state.abrt)
        return 1;
    setsub32(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}
static int
opCMPXCHG_l_a32(uint32_t fetchdat)
{
    uint32_t temp;
    uint32_t temp2 = EAX;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3) {
        SEG_CHECK_WRITE(cpu_state.ea_seg);
        CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, cpu_state.eaaddr + 3UL);
    }
    temp = geteal();
    if (cpu_state.abrt)
        return 1;
    /* As one bus operation, the old value written back on a mismatch
       (386_common.h). */
    CMPXCHG_RMW(uint32_t, 4, seteal, temp, EAX, cpu_state.regs[cpu_reg].l);
    if (cpu_state.abrt)
        return 1;
    setsub32(temp2, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 6 : 10);
    return 0;
}

#ifndef OPS_286_386
static int
opCMPXCHG8B_a16(uint32_t fetchdat)
{
    uint32_t temp;
    uint32_t temp_hi;
    uint32_t temp2    = EAX;
    uint32_t temp2_hi = EDX;

    fetch_ea_16(fetchdat);
    if (cpu_mod == 3) {
        /* The register form of CMPXCHG8B is illegal on real CPUs. The Microsoft
           Virtual PC 2007 BIOS abuses this encoding as a hypervisor call during
           its removable-media boot path; treat it as a no-op there so the guest
           does not get stuck in an endless #UD loop. Everywhere else, raise #UD
           (required e.g. by the Windows 7 DEC Tulip driver). */
        if (is_vpc) {
            cycles -= 6;
            return 0;
        }
        return ILLEGAL(fetchdat);
    }
    if ((fetchdat & 0x38) != 0x08)
        return ILLEGAL(fetchdat); /* Only 0F C7 /1 is CMPXCHG8B. */
    SEG_CHECK_WRITE(cpu_state.ea_seg);
    CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, (uint64_t) cpu_state.eaaddr + 7);
    temp    = geteal();
    if (cpu_state.abrt)
        return 0;
    temp_hi = readmeml(easeg, cpu_state.eaaddr + 4);
    if (cpu_state.abrt)
        return 0;
    {
        /* One 8-byte compare-and-swap while another processor could be
           running beside this one (386_common.h). */
        uint64_t *ap = cpu_mp_atomic ? (uint64_t *) mem_atomic_ptr(easeg + cpu_state.eaaddr, 8) : NULL;

        if (cpu_state.abrt)
            return 0;
        if (ap) {
            uint64_t ex = ((uint64_t) EDX << 32) | EAX;

            if (__atomic_compare_exchange_n(ap, &ex, ((uint64_t) ECX << 32) | EBX, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                temp    = EAX;
                temp_hi = EDX;
                mem_atomic_written((const uint8_t *) ap, 8);
            } else {
                temp    = (uint32_t) ex;
                temp_hi = (uint32_t) (ex >> 32);
                EAX     = temp;
                EDX     = temp_hi;
            }
        } else {
            /* Across a page or a line: read again and written under the
               split lock (386_common.h). */
            const int split = cpu_mp_atomic && mem_atomic_split;

            /* Validate both pages before either write. Probing with a write of
               the old value would cause an extra write to a memory-mapped device. */
            if (cr0 & 0x80000000) {
                mmutranslatereal(easeg + cpu_state.eaaddr, 1);
                if (cpu_state.abrt)
                    return 0;
                mmutranslatereal(easeg + cpu_state.eaaddr + 7, 1);
                if (cpu_state.abrt)
                    return 0;
            }
            if (split) {
                mem_split_lock_take();
                temp    = geteal();
                temp_hi = readmeml(easeg, cpu_state.eaaddr + 4);
            }
            if (cpu_state.abrt) {
            } else if (EAX == temp && EDX == temp_hi) {
                seteal(EBX);
                if (!cpu_state.abrt) {
                    writememl(easeg, cpu_state.eaaddr + 4, ECX);
                }
            } else {
                /* CMPXCHG8B writes back the complete old value on a mismatch. */
                seteal(temp);
                if (!cpu_state.abrt) {
                    writememl(easeg, cpu_state.eaaddr + 4, temp_hi);
                }
                if (!cpu_state.abrt) {
                    EAX = temp;
                    EDX = temp_hi;
                }
            }
            if (split)
                mem_split_lock_give();
        }
    }
    if (cpu_state.abrt)
        return 0;
    flags_rebuild();
    if (temp == temp2 && temp_hi == temp2_hi)
        cpu_state.flags |= Z_FLAG;
    else
        cpu_state.flags &= ~Z_FLAG;
    cycles -= (cpu_mod == 3) ? 6 : 10;
    return 0;
}
static int
opCMPXCHG8B_a32(uint32_t fetchdat)
{
    uint32_t temp;
    uint32_t temp_hi;
    uint32_t temp2    = EAX;
    uint32_t temp2_hi = EDX;

    fetch_ea_32(fetchdat);
    if (cpu_mod == 3) {
        /* See the a16 variant: allow the Virtual PC 2007 BIOS hypervisor-call
           encoding, otherwise raise #UD as a real CPU would. */
        if (is_vpc) {
            cycles -= 6;
            return 0;
        }
        return ILLEGAL(fetchdat);
    }
    if ((fetchdat & 0x38) != 0x08)
        return ILLEGAL(fetchdat); /* Only 0F C7 /1 is CMPXCHG8B. */
    SEG_CHECK_WRITE(cpu_state.ea_seg);
    CHECK_WRITE(cpu_state.ea_seg, cpu_state.eaaddr, (uint64_t) cpu_state.eaaddr + 7);
    temp    = geteal();
    if (cpu_state.abrt)
        return 0;
    temp_hi = readmeml(easeg, cpu_state.eaaddr + 4);
    if (cpu_state.abrt)
        return 0;
    {
        /* One 8-byte compare-and-swap while another processor could be
           running beside this one (386_common.h). */
        uint64_t *ap = cpu_mp_atomic ? (uint64_t *) mem_atomic_ptr(easeg + cpu_state.eaaddr, 8) : NULL;

        if (cpu_state.abrt)
            return 0;
        if (ap) {
            uint64_t ex = ((uint64_t) EDX << 32) | EAX;

            if (__atomic_compare_exchange_n(ap, &ex, ((uint64_t) ECX << 32) | EBX, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                temp    = EAX;
                temp_hi = EDX;
                mem_atomic_written((const uint8_t *) ap, 8);
            } else {
                temp    = (uint32_t) ex;
                temp_hi = (uint32_t) (ex >> 32);
                EAX     = temp;
                EDX     = temp_hi;
            }
        } else {
            /* Across a page or a line: read again and written under the
               split lock (386_common.h). */
            const int split = cpu_mp_atomic && mem_atomic_split;

            /* Validate both pages before either write. Probing with a write of
               the old value would cause an extra write to a memory-mapped device. */
            if (cr0 & 0x80000000) {
                mmutranslatereal(easeg + cpu_state.eaaddr, 1);
                if (cpu_state.abrt)
                    return 0;
                mmutranslatereal(easeg + cpu_state.eaaddr + 7, 1);
                if (cpu_state.abrt)
                    return 0;
            }
            if (split) {
                mem_split_lock_take();
                temp    = geteal();
                temp_hi = readmeml(easeg, cpu_state.eaaddr + 4);
            }
            if (cpu_state.abrt) {
            } else if (EAX == temp && EDX == temp_hi) {
                seteal(EBX);
                if (!cpu_state.abrt) {
                    writememl(easeg, cpu_state.eaaddr + 4, ECX);
                }
            } else {
                /* CMPXCHG8B writes back the complete old value on a mismatch. */
                seteal(temp);
                if (!cpu_state.abrt) {
                    writememl(easeg, cpu_state.eaaddr + 4, temp_hi);
                }
                if (!cpu_state.abrt) {
                    EAX = temp;
                    EDX = temp_hi;
                }
            }
            if (split)
                mem_split_lock_give();
        }
    }
    if (cpu_state.abrt)
        return 0;
    flags_rebuild();
    if (temp == temp2 && temp_hi == temp2_hi)
        cpu_state.flags |= Z_FLAG;
    else
        cpu_state.flags &= ~Z_FLAG;
    cycles -= (cpu_mod == 3) ? 6 : 10;
    return 0;
}
#endif

/* Write the source after a successful destination write. With two distinct
   registers it receives the old destination; with the same register the sum
   must remain. A faulting memory destination must leave the source unchanged. */
/* dest = eab, src = r8 */
static int
opXADD_b_a16(uint32_t fetchdat)
{
    uint8_t src;
    uint8_t dest;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = getr8(cpu_reg);
    dest = geteab();
    if (cpu_state.abrt)
        return 1;
    seteab_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd8(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm)) {
        setr8(cpu_reg, dest);
    }
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}
static int
opXADD_b_a32(uint32_t fetchdat)
{
    uint8_t src;
    uint8_t dest;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = getr8(cpu_reg);
    dest = geteab();
    if (cpu_state.abrt)
        return 1;
    seteab_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd8(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm)) {
        setr8(cpu_reg, dest);
    }
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}

static int
opXADD_w_a16(uint32_t fetchdat)
{
    uint16_t src;
    uint16_t dest;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = cpu_state.regs[cpu_reg].w;
    dest = geteaw();
    if (cpu_state.abrt)
        return 1;
    seteaw_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd16(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm))
        cpu_state.regs[cpu_reg].w = dest;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}
static int
opXADD_w_a32(uint32_t fetchdat)
{
    uint16_t src;
    uint16_t dest;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = cpu_state.regs[cpu_reg].w;
    dest = geteaw();
    if (cpu_state.abrt)
        return 1;
    seteaw_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd16(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm))
        cpu_state.regs[cpu_reg].w = dest;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}

static int
opXADD_l_a16(uint32_t fetchdat)
{
    uint32_t src;
    uint32_t dest;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = cpu_state.regs[cpu_reg].l;
    dest = geteal();
    if (cpu_state.abrt)
        return 1;
    seteal_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd32(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm))
        cpu_state.regs[cpu_reg].l = dest;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}
static int
opXADD_l_a32(uint32_t fetchdat)
{
    uint32_t src;
    uint32_t dest;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    src  = cpu_state.regs[cpu_reg].l;
    dest = geteal();
    if (cpu_state.abrt)
        return 1;
    seteal_rmw(dest, src + dest);
    if (cpu_state.abrt)
        return 1;
    setadd32(src, dest);
    /* With one register as both operands, leave the sum in place. */
    if ((cpu_mod != 3) || (cpu_reg != cpu_rm))
        cpu_state.regs[cpu_reg].l = dest;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 4);
    return 0;
}
