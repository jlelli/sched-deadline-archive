#ifndef _ASM_MATH128_H
#define _ASM_MATH128_H

#ifdef CONFIG_X86_64

#ifdef __SIZEOF_INT128__
#define ARCH_HAS_INT128
#endif

#ifndef ARCH_HAS_INT128

static inline u128 mul_u64_u64(u64 a, u64 b)
{
       u128 res;

       asm("mulq %2"
               : "=a" (res.lo), "=d" (res.hi)
               :  "rm" (b), "0" (a));

       return res;
}
#define mul_u64_u64 mul_u64_u64

static inline u128 add_u128(u128 a, u128 b)
{
       u128 res;

       asm("addq %2,%0;\n"
           "adcq %3,%1;\n"
               : "=rm" (res.lo), "=rm" (res.hi)
               : "r" (b.lo), "r" (b.hi), "0" (a.lo), "1" (a.hi));

       return res;
}
#define add_u128 add_u128

#endif /* ARCH_HAS_INT128 */
#endif /* CONFIG_X86_64 */
#endif /* _ASM_MATH128_H */
