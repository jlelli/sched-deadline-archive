#include <linux/math128.h>

#ifndef mul_u64_u64
/*
 * a * b = (ah * 2^32 + al) * (bh * 2^32 + bl) =
 *   ah*bh * 2^64 + (ah*bl + bh*al) * 2^32 + al*bl
 */
u128 mul_u64_u64(u64 a, u64 b)
{
	u128 t1, t2, t3, t4;
	u32 ah, al;
	u32 bh, bl;

	ah = a >> 32;
	al = a;

	bh = b >> 32;
	bl = b;

	t1.lo = 0;
	t1.hi = (u64)ah * bh;

	t2.lo = (u64)ah * bl;
	t2.hi = t2.lo >> 32;
	t2.lo <<= 32;

	t3.lo = (u64)al * bh;
	t3.hi = t3.lo >> 32;
	t3.lo <<= 32;

	t4.lo = (u64)al * bl;
	t4.hi = 0;

	t1 = add_u128(t1, t2);
	t1 = add_u128(t1, t3);
	t1 = add_u128(t1, t4);

	return t1;
}
#endif /* mul_u64_u64 */
