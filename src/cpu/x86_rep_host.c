/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The host's side of the REP string instructions (x86_rep_host.h):
 *          a generic version of each kernel and a Zen 5 one, picked by the
 *          single CPUID check in rep_host_detect().
 *
 *          The generic versions use only baseline instructions (SSE2 on
 *          x86-64, NEON on ARM64, plain C elsewhere): this file is built for
 *          the baseline whatever the rest of the build targets
 *          (cpu/CMakeLists.txt), so a build tuned for one host still has a
 *          true baseline path to fall back on and to measure against. The
 *          Zen 5 versions ask for AVX-512 themselves and use masked loads
 *          and stores, which need no scalar tails.
 *
 *          Stores go out in the guest's order, chunk by chunk: a later
 *          element of a run is never written before an earlier one, as the
 *          P5's in-order stores have it, not just the P6's looser fast
 *          strings.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdint.h>
#include <string.h>
#include <86box/mem.h>
#include "x86_rep_host.h"

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#    define REP_HOST_X86 1
#    include <cpuid.h>
#    include <immintrin.h>
#    define REP_ZEN5     __attribute__((target("avx512f,avx512bw,avx512vl,bmi,bmi2,lzcnt")))
#else
#    define REP_HOST_X86 0
#endif
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#    define REP_HOST_NEON 1
#    include <arm_neon.h>
#else
#    define REP_HOST_NEON 0
#endif

uint8_t rep_host_mode = REP_HOST_OFF;

/* A string run as one of the processor's accesses, as another processor
   running beside it sees them: after everything before it and before
   everything after, its own stores in any order among themselves (as an
   x86 fast string's). x86 hosts keep that order; ARM64 needs the fence
   (MEM_ORDERED, <86box/mem.h>). */
#if defined(__aarch64__) || defined(_M_ARM64)
#    define REP_ORDER()                                  \
        do {                                             \
            if (MEM_ORDERED)                             \
                __atomic_thread_fence(__ATOMIC_SEQ_CST); \
        } while (0)
#else
#    define REP_ORDER() \
        do {            \
        } while (0)
#endif

/* ------------------------------------------------------------------ */
/* Plain C, for hosts without either and for the ends of generic runs */
/* ------------------------------------------------------------------ */

static inline uint32_t
rep_elem_load(const uint8_t *p, uint32_t esz)
{
    uint32_t v = 0;

    memcpy(&v, p, esz);
    return v;
}

#if REP_HOST_X86 || !REP_HOST_NEON
static void
fill_c(uint8_t *p, uint32_t value, uint32_t n, uint32_t esz, int down)
{
    if (down) {
        for (uint8_t *q = p + (n - 1) * esz; n--; q -= esz)
            memcpy(q, &value, esz);
    } else {
        for (; n--; p += esz)
            memcpy(p, &value, esz);
    }
}

static uint32_t
scan_c(const uint8_t *lo, uint32_t n, uint32_t esz, int down, uint32_t value, int repe)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = down ? lo + (n - 1 - i) * esz : lo + i * esz;

        if ((rep_elem_load(p, esz) == value) != repe)
            return i;
    }
    return n;
}

static uint32_t
cmp_c(const uint8_t *alo, const uint8_t *blo, uint32_t n, uint32_t esz, int down, int repe)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t o = down ? (n - 1 - i) * esz : i * esz;

        if ((memcmp(alo + o, blo + o, esz) == 0) != repe)
            return i;
    }
    return n;
}
#endif

#if REP_HOST_NEON
/* ------------------------------------------------------------------ */
/* ARM64: NEON, sixteen bytes at a time                               */
/* ------------------------------------------------------------------ */

/* The lanes of a byte mask (0x00 or 0xff each) as four bits each. */
static inline uint64_t
rep_neon_bits(uint8x16_t m)
{
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}

static inline uint8x16_t
rep_neon_splat(uint32_t value, uint32_t esz)
{
    if (esz == 1)
        return vdupq_n_u8((uint8_t) value);
    if (esz == 2)
        return vreinterpretq_u8_u16(vdupq_n_u16((uint16_t) value));
    return vreinterpretq_u8_u32(vdupq_n_u32(value));
}

/* Element lanes equal, as a byte mask: each element's bytes all set or not. */
static inline uint8x16_t
rep_neon_eq(uint8x16_t a, uint8x16_t b, uint32_t esz)
{
    if (esz == 1)
        return vceqq_u8(a, b);
    if (esz == 2)
        return vreinterpretq_u8_u16(vceqq_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(b)));
    return vreinterpretq_u8_u32(vceqq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(b)));
}

static void
fill_neon(uint8_t *p, uint32_t value, uint32_t n, uint32_t esz, int down)
{
    const uint8x16_t v     = rep_neon_splat(value, esz);
    uint32_t         bytes = n * esz;
    uint8_t          tail[16];

    /* Each sixteen bytes stored start on an element (bytes is a whole
       number of them), so the splat lines up. Stored in the guest's order:
       going down from the top, the last few bytes being the lowest. */
    vst1q_u8(tail, v);
    if (!down) {
        for (; bytes >= 64; bytes -= 64, p += 64) {
            vst1q_u8(p, v);
            vst1q_u8(p + 16, v);
            vst1q_u8(p + 32, v);
            vst1q_u8(p + 48, v);
        }
        for (; bytes >= 16; bytes -= 16, p += 16)
            vst1q_u8(p, v);
    } else {
        for (; bytes >= 64; bytes -= 64) {
            vst1q_u8(p + bytes - 16, v);
            vst1q_u8(p + bytes - 32, v);
            vst1q_u8(p + bytes - 48, v);
            vst1q_u8(p + bytes - 64, v);
        }
        for (; bytes >= 16; bytes -= 16)
            vst1q_u8(p + bytes - 16, v);
    }
    if (bytes)
        memcpy(p, tail, bytes);
}

/* The first element, in the run's order, where the test fails: for each
   sixteen bytes, the elements whose (equal) differs from repe. */
static uint32_t
scan_neon(const uint8_t *lo, uint32_t n, uint32_t esz, int down, uint32_t value, int repe)
{
    const uint8x16_t v   = rep_neon_splat(value, esz);
    const uint32_t   per = 16 / esz;
    uint32_t         i   = 0;

    if (!down) {
        for (; (n - i) >= per; i += per) {
            uint8x16_t m = rep_neon_eq(vld1q_u8(lo + i * esz), v, esz);
            uint64_t   b;

            if (repe)
                m = vmvnq_u8(m);
            b = rep_neon_bits(m);
            if (b)
                return i + (uint32_t) (__builtin_ctzll(b) >> 2) / esz;
        }
    } else {
        for (; (n - i) >= per; i += per) {
            const uint8_t *chunk = lo + (n - i - per) * esz; /* the highest elements not yet looked at */
            uint8x16_t     m     = rep_neon_eq(vld1q_u8(chunk), v, esz);
            uint64_t       b;

            if (repe)
                m = vmvnq_u8(m);
            b = rep_neon_bits(m);
            if (b)
                return i + (uint32_t) (__builtin_clzll(b) >> 2) / esz;
        }
    }
    for (; i < n; i++) {
        const uint8_t *p = down ? lo + (n - 1 - i) * esz : lo + i * esz;
        uint32_t       e = 0;

        memcpy(&e, p, esz);
        if ((e == value) != repe)
            return i;
    }
    return n;
}

static uint32_t
cmp_neon(const uint8_t *alo, const uint8_t *blo, uint32_t n, uint32_t esz, int down, int repe)
{
    const uint32_t per = 16 / esz;
    uint32_t       i   = 0;

    if (!down) {
        for (; (n - i) >= per; i += per) {
            uint8x16_t m = rep_neon_eq(vld1q_u8(alo + i * esz), vld1q_u8(blo + i * esz), esz);
            uint64_t   b;

            if (repe)
                m = vmvnq_u8(m);
            b = rep_neon_bits(m);
            if (b)
                return i + (uint32_t) (__builtin_ctzll(b) >> 2) / esz;
        }
    } else {
        for (; (n - i) >= per; i += per) {
            const uint32_t o = (n - i - per) * esz;
            uint8x16_t     m = rep_neon_eq(vld1q_u8(alo + o), vld1q_u8(blo + o), esz);
            uint64_t       b;

            if (repe)
                m = vmvnq_u8(m);
            b = rep_neon_bits(m);
            if (b)
                return i + (uint32_t) (__builtin_clzll(b) >> 2) / esz;
        }
    }
    for (; i < n; i++) {
        const uint32_t o = down ? (n - 1 - i) * esz : i * esz;

        if ((memcmp(alo + o, blo + o, esz) == 0) != repe)
            return i;
    }
    return n;
}
#endif

/* ------------------------------------------------------------------ */
/* Generic x86-64: SSE2, sixteen bytes at a time                      */
/* ------------------------------------------------------------------ */

#if REP_HOST_X86
static void
copy_generic(uint8_t *d, const uint8_t *s, uint32_t n, int down)
{
    if (!down) {
        for (; n >= 16; n -= 16, d += 16, s += 16)
            _mm_storeu_si128((__m128i *) d, _mm_loadu_si128((const __m128i *) s));
    } else {
        for (; n >= 16; n -= 16)
            _mm_storeu_si128((__m128i *) (d + n - 16), _mm_loadu_si128((const __m128i *) (s + n - 16)));
    }
    /* The last few bytes, still in order: memmove copies them as the
       guest would, since the direction is the safe one. */
    if (n)
        memmove(d, s, n);
}

static __m128i
splat_generic(uint32_t value, uint32_t esz)
{
    if (esz == 1)
        return _mm_set1_epi8((char) value);
    if (esz == 2)
        return _mm_set1_epi16((short) value);
    return _mm_set1_epi32((int) value);
}

static void
fill_generic(uint8_t *p, uint32_t value, uint32_t n, uint32_t esz, int down)
{
    const __m128i v     = splat_generic(value, esz);
    const uint32_t per  = 16 / esz;
    uint32_t       left = n;

    if (!down) {
        for (; left >= per; left -= per, p += 16)
            _mm_storeu_si128((__m128i *) p, v);
        fill_c(p, value, left, esz, 0);
    } else {
        for (; left >= per; left -= per)
            _mm_storeu_si128((__m128i *) (p + (left - per) * esz), v);
        fill_c(p, value, left, esz, 1);
    }
}

/* Bit b set where byte b of the chunk belongs to an element that stops
   the run; an element's bytes are all set or all clear. */
static uint32_t
stops_generic(__m128i a, __m128i b, uint32_t esz, int repe)
{
    __m128i  eq;
    uint32_t m;

    if (esz == 1)
        eq = _mm_cmpeq_epi8(a, b);
    else if (esz == 2)
        eq = _mm_cmpeq_epi16(a, b);
    else
        eq = _mm_cmpeq_epi32(a, b);
    m = (uint32_t) _mm_movemask_epi8(eq);
    return repe ? (~m & 0xffff) : m;
}

static uint32_t
scan_generic(const uint8_t *lo, uint32_t n, uint32_t esz, int down, uint32_t value, int repe)
{
    const __m128i  v   = splat_generic(value, esz);
    const uint32_t per = 16 / esz;
    uint32_t       done;

    for (done = 0; (n - done) >= per; done += per) {
        const uint8_t *p = down ? lo + (n - done - per) * esz : lo + done * esz;
        uint32_t       s = stops_generic(_mm_loadu_si128((const __m128i *) p), v, esz, repe);

        if (s)
            return done + (down ? (per - 1 - (31 - __builtin_clz(s)) / esz) : (uint32_t) __builtin_ctz(s) / esz);
    }
    return done + scan_c(down ? lo : lo + done * esz, n - done, esz, down, value, repe);
}

static uint32_t
cmp_generic(const uint8_t *alo, const uint8_t *blo, uint32_t n, uint32_t esz, int down, int repe)
{
    const uint32_t per = 16 / esz;
    uint32_t       done;

    for (done = 0; (n - done) >= per; done += per) {
        const uint32_t o = down ? (n - done - per) * esz : done * esz;
        uint32_t       s = stops_generic(_mm_loadu_si128((const __m128i *) (alo + o)),
                                         _mm_loadu_si128((const __m128i *) (blo + o)), esz, repe);

        if (s)
            return done + (down ? (per - 1 - (31 - __builtin_clz(s)) / esz) : (uint32_t) __builtin_ctz(s) / esz);
    }
    {
        const uint32_t o = down ? 0 : done * esz;

        return done + cmp_c(alo + o, blo + o, n - done, esz, down, repe);
    }
}

/* ------------------------------------------------------------------ */
/* Zen 5: AVX-512, sixty-four bytes at a time, masked at the ends     */
/* ------------------------------------------------------------------ */

static REP_ZEN5 inline uint64_t
zen5_bytes(uint32_t n)
{
    return (n >= 64) ? ~0ULL : _bzhi_u64(~0ULL, n);
}

static REP_ZEN5 void
copy_zen5(uint8_t *d, const uint8_t *s, uint32_t n, int down)
{
    if (!down) {
        for (; n >= 64; n -= 64, d += 64, s += 64)
            _mm512_storeu_si512(d, _mm512_loadu_si512(s));
        if (n) {
            const __mmask64 m = zen5_bytes(n);

            _mm512_mask_storeu_epi8(d, m, _mm512_maskz_loadu_epi8(m, s));
        }
    } else {
        for (; n >= 64; n -= 64)
            _mm512_storeu_si512(d + n - 64, _mm512_loadu_si512(s + n - 64));
        if (n) {
            const __mmask64 m = zen5_bytes(n);

            _mm512_mask_storeu_epi8(d, m, _mm512_maskz_loadu_epi8(m, s));
        }
    }
}

static REP_ZEN5 __m512i
splat_zen5(uint32_t value, uint32_t esz)
{
    if (esz == 1)
        return _mm512_set1_epi8((char) value);
    if (esz == 2)
        return _mm512_set1_epi16((short) value);
    return _mm512_set1_epi32((int) value);
}

static REP_ZEN5 void
fill_zen5(uint8_t *p, uint32_t value, uint32_t n, uint32_t esz, int down)
{
    const __m512i v     = splat_zen5(value, esz);
    uint32_t      bytes = n * esz;

    if (!down) {
        for (; bytes >= 64; bytes -= 64, p += 64)
            _mm512_storeu_si512(p, v);
        if (bytes)
            _mm512_mask_storeu_epi8(p, zen5_bytes(bytes), v);
    } else {
        for (; bytes >= 64; bytes -= 64)
            _mm512_storeu_si512(p + bytes - 64, v);
        if (bytes)
            _mm512_mask_storeu_epi8(p, zen5_bytes(bytes), v);
    }
}

/* One bit per element of the chunk: set where the element stops the run.
   Lanes past cnt are clear. */
static REP_ZEN5 uint64_t
stops_zen5(__m512i a, __m512i b, uint32_t esz, uint32_t cnt, int repe)
{
    const uint64_t valid = zen5_bytes(cnt);
    uint64_t       eq;

    if (esz == 1)
        eq = _mm512_cmpeq_epi8_mask(a, b);
    else if (esz == 2)
        eq = _mm512_cmpeq_epi16_mask(a, b);
    else
        eq = _mm512_cmpeq_epi32_mask(a, b);
    return (repe ? ~eq : eq) & valid;
}

/* Of the chunk's cnt elements, taken in the guest's order, the first that
   stops the run, from its stop mask. */
static REP_ZEN5 inline uint32_t
first_zen5(uint64_t s, uint32_t cnt, int down)
{
    return down ? (cnt - 1 - (63 - (uint32_t) _lzcnt_u64(s))) : (uint32_t) _tzcnt_u64(s);
}

static REP_ZEN5 uint32_t
scan_zen5(const uint8_t *lo, uint32_t n, uint32_t esz, int down, uint32_t value, int repe)
{
    const __m512i  v   = splat_zen5(value, esz);
    const uint32_t per = 64 / esz;

    for (uint32_t done = 0; done < n;) {
        const uint32_t cnt = ((n - done) < per) ? (n - done) : per;
        const uint8_t *p   = down ? lo + (n - done - cnt) * esz : lo + done * esz;
        const __m512i  x   = _mm512_maskz_loadu_epi8(zen5_bytes(cnt * esz), p);
        const uint64_t s   = stops_zen5(x, v, esz, cnt, repe);

        if (s)
            return done + first_zen5(s, cnt, down);
        done += cnt;
    }
    return n;
}

static REP_ZEN5 uint32_t
cmp_zen5(const uint8_t *alo, const uint8_t *blo, uint32_t n, uint32_t esz, int down, int repe)
{
    const uint32_t per = 64 / esz;

    for (uint32_t done = 0; done < n;) {
        const uint32_t  cnt = ((n - done) < per) ? (n - done) : per;
        const uint32_t  o   = down ? (n - done - cnt) * esz : done * esz;
        const __mmask64 m   = zen5_bytes(cnt * esz);
        const uint64_t  s   = stops_zen5(_mm512_maskz_loadu_epi8(m, alo + o),
                                         _mm512_maskz_loadu_epi8(m, blo + o), esz, cnt, repe);

        if (s)
            return done + first_zen5(s, cnt, down);
        done += cnt;
    }
    return n;
}
#endif

/* ------------------------------------------------------------------ */
/* The entry points: one flag test each                               */
/* ------------------------------------------------------------------ */

void
rep_host_copy(uint8_t *dlo, const uint8_t *slo, uint32_t bytes, int down)
{
    REP_ORDER();
#if REP_HOST_X86
    if (rep_host_mode == REP_HOST_ZEN5)
        copy_zen5(dlo, slo, bytes, down);
    else
        copy_generic(dlo, slo, bytes, down);
#else
    (void) down;
    memmove(dlo, slo, bytes);
#endif
    REP_ORDER();
}

void
rep_host_copy_elems(uint8_t *d0, const uint8_t *s0, uint32_t n, uint32_t esz, int down)
{
    const intptr_t step = down ? -(intptr_t) esz : (intptr_t) esz;

    REP_ORDER();
    /* Each element read whole and then written, as the guest does, so
       what an earlier element wrote is what a later one reads. */
    for (; n--; d0 += step, s0 += step) {
        const uint32_t v = rep_elem_load(s0, esz);

        memcpy(d0, &v, esz);
    }
    REP_ORDER();
}

void
rep_host_fill(uint8_t *dlo, uint32_t value, uint32_t n, uint32_t esz, int down)
{
    REP_ORDER();
#if REP_HOST_X86
    if (rep_host_mode == REP_HOST_ZEN5)
        fill_zen5(dlo, value, n, esz, down);
    else
        fill_generic(dlo, value, n, esz, down);
#elif REP_HOST_NEON
    fill_neon(dlo, value, n, esz, down);
#else
    fill_c(dlo, value, n, esz, down);
#endif
    REP_ORDER();
}

uint32_t
rep_host_scan(const uint8_t *lo, uint32_t n, uint32_t esz, int down, uint32_t value, int repe)
{
    uint32_t r;

    REP_ORDER();
#if REP_HOST_X86
    if (rep_host_mode == REP_HOST_ZEN5)
        r = scan_zen5(lo, n, esz, down, value, repe);
    else
        r = scan_generic(lo, n, esz, down, value, repe);
#elif REP_HOST_NEON
    r = scan_neon(lo, n, esz, down, value, repe);
#else
    r = scan_c(lo, n, esz, down, value, repe);
#endif
    REP_ORDER();
    return r;
}

uint32_t
rep_host_cmp(const uint8_t *alo, const uint8_t *blo, uint32_t n, uint32_t esz, int down, int repe)
{
    uint32_t r;

    REP_ORDER();
#if REP_HOST_X86
    if (rep_host_mode == REP_HOST_ZEN5)
        r = cmp_zen5(alo, blo, n, esz, down, repe);
    else
        r = cmp_generic(alo, blo, n, esz, down, repe);
#elif REP_HOST_NEON
    r = cmp_neon(alo, blo, n, esz, down, repe);
#else
    r = cmp_c(alo, blo, n, esz, down, repe);
#endif
    REP_ORDER();
    return r;
}

/* ------------------------------------------------------------------ */
/* The one CPUID check                                                */
/* ------------------------------------------------------------------ */

#if REP_HOST_X86
/* AMD family 1Ah (Zen 5) or later, with AVX-512 F/BW/VL, BMI1/2 and
   LZCNT, and the operating system saving the AVX-512 state. */
static int
rep_host_is_zen5(void)
{
    unsigned int a, b, c, d;
    unsigned int family;
    unsigned int xcr0_lo, xcr0_hi;

    if (!__get_cpuid(0, &a, &b, &c, &d) || (a < 7))
        return 0;
    if ((b != 0x68747541) || (d != 0x69746e65) || (c != 0x444d4163)) /* AuthenticAMD */
        return 0;

    __get_cpuid(1, &a, &b, &c, &d);
    family = (a >> 8) & 0x0f;
    if (family == 0x0f)
        family += (a >> 20) & 0xff;
    if ((family < 0x1a) || !(c & (1u << 27))) /* OSXSAVE */
        return 0;

    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0xe6) != 0xe6) /* SSE, AVX, opmask, ZMM0-15 and 16-31 */
        return 0;

    __get_cpuid_count(7, 0, &a, &b, &c, &d);
    if (!(b & (1u << 16)) || !(b & (1u << 30)) || !(b & (1u << 31)) || /* AVX-512 F, BW, VL */
        !(b & (1u << 3)) || !(b & (1u << 8)))                          /* BMI1, BMI2 */
        return 0;

    if (!__get_cpuid(0x80000001, &a, &b, &c, &d) || !(c & (1u << 5))) /* LZCNT */
        return 0;

    return 1;
}
#endif

void
rep_host_detect(void)
{
    int zen5 = 0;

#if REP_HOST_X86
    zen5 = rep_host_is_zen5();
#endif
    rep_host_mode = zen5 ? REP_HOST_ZEN5 : REP_HOST_GENERIC;
}
