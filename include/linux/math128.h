#ifndef _LINUX_MATH128_H
#define _LINUX_MATH128_H

#include <linux/types.h>

typedef union {
	struct {
#if __BYTE_ORDER__  == __ORDER_LITTLE_ENDIAN__
		u64 lo, hi;
#else
		u64 hi, lo;
#endif
	};
#ifdef __SIZEOF_INT128__ /* gcc-4.6+ */
	unsigned __int128 val;
#endif
} u128;

#define U128_INIT(_hi, _lo) (u128){{ .hi = (_hi), .lo = (_lo) }}

#include <asm/math128.h>

/*
 * Make usage of __int128 dependent on arch code so they can
 * judge if gcc is doing the right thing for them and can over-ride
 * any funnies.
 */

#ifndef ARCH_HAS_INT128

#ifndef add_u128
static inline u128 add_u128(u128 a, u128 b)
{
	a.hi += b.hi;
	a.lo += b.lo;
	if (a.lo < b.lo)
		a.hi++;

	return a;
}
#endif /* add_u128 */

#ifndef mul_u64_u64
extern u128 mul_u64_u64(u64 a, u64 b);
#endif

#ifndef mul_u64_u32_shr
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift)
{
	u32 ah, al;
	u64 t1, t2;

	ah = a >> 32;
	al = a;

	t1 = ((u64)al * mul) >> shift;
	t2 = ((u64)ah * mul) << (32 - shift);

	return t1 + t2;
}
#endif /* mul_u64_u32_shr */

#ifndef shl_u128
static inline u128 shl_u128(u128 x, unsigned int n)
{
	u128 res;

	if (!n)
		return x;

	if (n < 64) {
		res.hi = x.hi << n;
		res.hi |= x.lo >> (64 - n);
		res.lo = x.lo << n;
	} else {
		res.lo = 0;
		res.hi = x.lo << (n - 64);
	}

	return res;
}
#endif /* shl_u128 */

#ifndef shr_u128
static inline u128 shr_u128(u128 x, unsigned int n)
{
	u128 res;

	if (!n)
		return x;

	if (n < 64) {
		res.lo = x.lo >> n;
		res.lo |= x.hi << (64 - n);
		res.hi = x.hi >> n;
	} else {
		res.hi = 0;
		res.lo = x.hi >> (n - 64);
	}

	return res;
}
#endif /* shr_u128 */

#ifndef cmp_u128
static inline int cmp_u128(u128 a, u128 b)
{
	if (a.hi > b.hi)
		return 1;
	if (a.hi < b.hi)
		return -1;
	if (a.lo > b.lo)
		return 1;
	if (a.lo < b.lo)
		return -1;

	return 0;
}
#endif /* cmp_u128 */

#else /* ARCH_HAS_INT128 */

#ifndef add_u128
static inline u128 add_u128(u128 a, u128 b)
{
	a.val += b.val;
	return a;
}
#endif /* add_u128 */

#ifndef mul_u64_u64
static inline u128 mul_u64_u64(u64 a, u64 b)
{
	u128 res;

	res.val = a;
	res.val *= b;

	return res;
}
#define mul_u64_u64 mul_u64_u64
#endif

#ifndef mul_u64_u32_shr
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift)
{
	return (u64)(((unsigned __int128)a * mul) >> shift);
}
#endif /* mul_u64_u32_shr */

#ifndef shl_u128
static inline u128 shl_u128(u128 x, unsigned int n)
{
	x.val <<= n;
	return x;
}
#endif /* shl_u128 */

#ifndef shr_u128
static inline u128 shr_u128(u128 x, unsigned int n)
{
	x.val >>= n;
	return x;
}
#endif /* shr_u128 */

#ifndef cmp_u128
static inline int cmp_u128(u128 a, u128 b)
{
	if (a.val < b.val)
		return -1;
	if (a.val > b.val)
		return 1;
	return 0;
}
#endif /* cmp_u128 */

#endif /* ARCH_HAS_INT128 */

#endif /* _LINUX_MATH128_H */
