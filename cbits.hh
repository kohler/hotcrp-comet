/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef MASSTREE_CBITS_HH
#define MASSTREE_CBITS_HH 1
#include <cstdint>
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#include <arpa/inet.h>
#if HAVE_TYPE_TRAITS
#include <type_traits>
#endif

#define arraysize(a) (sizeof(a) / sizeof((a)[0]))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#if !HAVE_CXX_STATIC_ASSERT
#define static_assert(x, msg) switch (x) case 0: case !!(x):
#endif

#if !HAVE_CXX_CONSTEXPR
#define constexpr const
#endif

#if HAVE_OFF_T_IS_LONG_LONG
#define PRIdOFF_T "lld"
#else
#define PRIdOFF_T "ld"
#endif

#if HAVE_SIZE_T_IS_UNSIGNED_LONG_LONG
#define PRIdSIZE_T "llu"
#define PRIdSSIZE_T "lld"
#elif HAVE_SIZE_T_IS_UNSIGNED_LONG
#define PRIdSIZE_T "lu"
#define PRIdSSIZE_T "ld"
#else
#define PRIdSIZE_T "u"
#define PRIdSSIZE_T "d"
#endif

#if (__i386__ || __x86_64__) && !defined(__x86__)
# define __x86__ 1
#endif
#define PREFER_X86 1
#define ALLOW___SYNC_BUILTINS 1

#if !defined(HAVE_INDIFFERENT_ALIGMENT) && (__i386__ || __x86_64__ || __arch_um__)
# define HAVE_INDIFFERENT_ALIGNMENT 1
#endif


/** @brief Return the index of the most significant bit set in @a x.
 * @return 0 if @a x = 0; otherwise the index of first bit set, where the
 * most significant bit is numbered 1.
 */
inline int ffs_msb(unsigned x) {
    return (x ? __builtin_clz(x) + 1 : 0);
}
/** @overload */
inline int ffs_msb(unsigned long x) {
    return (x ? __builtin_clzl(x) + 1 : 0);
}
/** @overload */
inline int ffs_msb(unsigned long long x) {
    return (x ? __builtin_clzll(x) + 1 : 0);
}


/** @brief Do-nothing function object. */
struct do_nothing {
    void operator()() const {
    }
    template <typename T>
    void operator()(const T&) const {
    }
    template <typename T, typename U>
    void operator()(const T&, const U&) const {
    }
};


/** Bit counting. */

/** @brief Return the number of leading 0 bits in @a x.
 * @pre @a x != 0
 *
 * "Leading" means "most significant." */
#if HAVE___BUILTIN_CLZ
inline int clz(int x) {
    return __builtin_clz(x);
}
inline int clz(unsigned x) {
    return __builtin_clz(x);
}
#endif

#if HAVE___BUILTIN_CLZL
inline int clz(long x) {
    return __builtin_clzl(x);
}
inline int clz(unsigned long x) {
    return __builtin_clzl(x);
}
#endif

#if HAVE___BUILTIN_CLZLL
inline int clz(long long x) {
    return __builtin_clzll(x);
}
inline int clz(unsigned long long x) {
    return __builtin_clzll(x);
}
#endif

/** @brief Return the number of trailing 0 bits in @a x.
 * @pre @a x != 0
 *
 * "Trailing" means "least significant." */
#if HAVE___BUILTIN_CTZ
inline int ctz(int x) {
    return __builtin_ctz(x);
}
inline int ctz(unsigned x) {
    return __builtin_ctz(x);
}
#endif

#if HAVE___BUILTIN_CTZL
inline int ctz(long x) {
    return __builtin_ctzl(x);
}
inline int ctz(unsigned long x) {
    return __builtin_ctzl(x);
}
#endif

#if HAVE___BUILTIN_CTZLL
inline int ctz(long long x) {
    return __builtin_ctzll(x);
}
inline int ctz(unsigned long long x) {
    return __builtin_ctzll(x);
}
#endif

template <typename T, typename U>
inline T iceil(T x, U y) {
    U mod = x % y;
    return x + (mod ? y - mod : 0);
}

/** @brief Return the smallest power of 2 greater than or equal to @a x.
    @pre @a x != 0
    @pre the result is representable in type T (that is, @a x can't be
    larger than the largest power of 2 representable in type T) */
template <typename T>
inline T iceil_log2(T x) {
    return T(1) << (sizeof(T) * 8 - clz(x) - !(x & (x - 1)));
}

/** @brief Return the largest power of 2 less than or equal to @a x.
    @pre @a x != 0 */
template <typename T>
inline T ifloor_log2(T x) {
    return T(1) << (sizeof(T) * 8 - 1 - clz(x));
}

/** @brief Return the index of the lowest 0 nibble in @a x.
 *
 * 0 is the lowest-order nibble. Returns -1 if no nibbles are 0. */
template <typename T>
inline int find_lowest_zero_nibble(T x) {
    static_assert(sizeof(T) <= sizeof(unsigned long long), "T is too big");
#if SIZEOF_LONG_LONG == 16
    T h = T(0x88888888888888888888888888888888ULL), l = T(0x11111111111111111111111111111111ULL);
#else
    T h = T(0x8888888888888888ULL), l = T(0x1111111111111111ULL);
#endif
    T t = h & (x - l) & ~x;
    return t ? ctz(t) >> 2 : -1;
}


struct uninitialized_type {};

#endif
