/* XCHG with memory is locked whether or not LOCK is written (Pentium
   Vol. 3, 19.1.2): its read and write of device memory are one bus
   operation (CPU_LOCKED_BEGIN). Like any locked instruction, the code
   after it is what was fetched when it began (cpu_fetch_ahead_take,
   386_common.c); another processor rewriting that code after the exchange
   ends compiled code's block here, so the next instruction is decoded from
   what was fetched. With a register operand (mod 3) nothing is locked. */
#define XCHG_MEM_OP(name)                      \
    static int name##_body(uint32_t fetchdat); \
    static int name(uint32_t fetchdat)         \
    {                                          \
        int r_;                                \
        if ((fetchdat & 0xc0) == 0xc0)         \
            return name##_body(fetchdat);      \
        cpu_fetch_ahead_take();                \
        CPU_LOCKED_BEGIN();                    \
        r_ = name##_body(fetchdat);            \
        CPU_LOCKED_END();                      \
        return cpu_fetch_ahead_after() | r_;   \
    }                                          \
    static int name##_body(uint32_t fetchdat)

XCHG_MEM_OP(opXCHG_b_a16)
{
    uint8_t temp;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteab();
    if (cpu_state.abrt)
        return 1;
    seteab_rmw(temp, getr8(cpu_reg));
    if (cpu_state.abrt)
        return 1;
    setr8(cpu_reg, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 0, 0);
    return 0;
}
XCHG_MEM_OP(opXCHG_b_a32)
{
    uint8_t temp;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteab();
    if (cpu_state.abrt)
        return 1;
    seteab_rmw(temp, getr8(cpu_reg));
    if (cpu_state.abrt)
        return 1;
    setr8(cpu_reg, temp);
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 0, 1);
    return 0;
}

XCHG_MEM_OP(opXCHG_w_a16)
{
    uint16_t temp;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteaw();
    if (cpu_state.abrt)
        return 1;
    seteaw_rmw(temp, cpu_state.regs[cpu_reg].w);
    if (cpu_state.abrt)
        return 1;
    cpu_state.regs[cpu_reg].w = temp;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 0, 0);
    return 0;
}
XCHG_MEM_OP(opXCHG_w_a32)
{
    uint16_t temp;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteaw();
    if (cpu_state.abrt)
        return 1;
    seteaw_rmw(temp, cpu_state.regs[cpu_reg].w);
    if (cpu_state.abrt)
        return 1;
    cpu_state.regs[cpu_reg].w = temp;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 0, 1);
    return 0;
}

XCHG_MEM_OP(opXCHG_l_a16)
{
    uint32_t temp;

    fetch_ea_16(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteal();
    if (cpu_state.abrt)
        return 1;
    seteal_rmw(temp, cpu_state.regs[cpu_reg].l);
    if (cpu_state.abrt)
        return 1;
    cpu_state.regs[cpu_reg].l = temp;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, 0, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 0);
    return 0;
}
XCHG_MEM_OP(opXCHG_l_a32)
{
    uint32_t temp;

    fetch_ea_32(fetchdat);
    if (cpu_mod != 3)
        SEG_CHECK_WRITE(cpu_state.ea_seg);
    temp = geteal();
    if (cpu_state.abrt)
        return 1;
    seteal_rmw(temp, cpu_state.regs[cpu_reg].l);
    if (cpu_state.abrt)
        return 1;
    cpu_state.regs[cpu_reg].l = temp;
    CLOCK_CYCLES((cpu_mod == 3) ? 3 : 5);
    PREFETCH_RUN((cpu_mod == 3) ? 3 : 5, 2, rmdat, 0, (cpu_mod == 3) ? 0 : 1, 0, (cpu_mod == 3) ? 0 : 1, 1);
    return 0;
}

static int
opXCHG_AX_BX(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = BX;
    BX            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_CX(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = CX;
    CX            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_DX(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = DX;
    DX            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_SI(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = SI;
    SI            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_DI(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = DI;
    DI            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_BP(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = BP;
    BP            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_AX_SP(UNUSED(uint32_t fetchdat))
{
    uint16_t temp = AX;
    AX            = SP;
    SP            = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}

static int
opXCHG_EAX_EBX(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = EBX;
    EBX           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_ECX(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = ECX;
    ECX           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_EDX(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = EDX;
    EDX           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_ESI(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = ESI;
    ESI           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_EDI(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = EDI;
    EDI           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_EBP(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = EBP;
    EBP           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}
static int
opXCHG_EAX_ESP(UNUSED(uint32_t fetchdat))
{
    uint32_t temp = EAX;
    EAX           = ESP;
    ESP           = temp;
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}

#define opBSWAP(reg)                                                                                      \
    static int opBSWAP_##reg(UNUSED(uint32_t fetchdat))                                                   \
    {                                                                                                     \
        reg = (reg >> 24) | ((reg >> 8) & 0xff00) | ((reg << 8) & 0xff0000) | ((reg << 24) & 0xff000000); \
        CLOCK_CYCLES(1);                                                                                  \
        PREFETCH_RUN(1, 1, -1, 0, 0, 0, 0, 0);                                                            \
        return 0;                                                                                         \
    }

// clang-format off
opBSWAP(EAX)
opBSWAP(EBX)
opBSWAP(ECX)
opBSWAP(EDX)
opBSWAP(ESI)
opBSWAP(EDI)
opBSWAP(EBP)
opBSWAP(ESP)
// clang-format on
