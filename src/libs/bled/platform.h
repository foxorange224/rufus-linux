#pragma once

#if defined(__GNUC__) || defined(__clang__)
#if defined(__i386__)
#define FAST_FUNC __attribute__((regparm(0)))
#else
#define FAST_FUNC
#endif
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define ALWAYS_ALIGNED(x) __attribute__((aligned(x)))
#define UNUSED_PARAM __attribute__((unused))
#define NORETURN __attribute__((noreturn))
#define PACKED __attribute__((packed))
#else
#define FAST_FUNC
#define ALWAYS_INLINE inline
#define ALWAYS_ALIGNED(x)
#define UNUSED_PARAM
#define NORETURN
#define PACKED
#endif

#define UNUSED_PARAM_UNIMPL UNUSED_PARAM

typedef unsigned char smallint;

#if defined(__linux__)
#include <endian.h>
#include <byteswap.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/endian.h>
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
#define BB_BIG_ENDIAN 1
#define BB_LITTLE_ENDIAN 0
#else
#define BB_BIG_ENDIAN 0
#define BB_LITTLE_ENDIAN 1
#endif

#define IF_BIG_ENDIAN(x) (BB_BIG_ENDIAN ? (x) : 0)
#define IF_LITTLE_ENDIAN(x) (BB_LITTLE_ENDIAN ? (x) : 0)
