#ifndef LIBMCU_COMPILER_H
#define LIBMCU_COMPILER_H 202012L

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(UNITTEST)
#define STATIC
#else
#define STATIC				static
#endif

#define unused(x)			(void)(x)

#define LIBMCU_UNUSED			__attribute__((unused))
#define LIBMCU_USED			__attribute__((used))
#define LIBMCU_ALWAYS_INLINE		__attribute__((always_inline))
#define LIBMCU_WEAK			__attribute__((weak))
#define LIBMCU_NORETURN			__attribute__((noreturn))
#define LIBMCU_PACKED			__attribute__((packed))

#define LIBMCU_STATIC_ASSERT(exp, msg)	_Static_assert(exp, msg)

#define barrier()			__asm__ __volatile__("" ::: "memory")
#define ACCESS_ONCE(x)			(*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(ptr, val)
#define READ_ONCE(x)

#define NO_OPTIMIZE			__attribute__((optimize("O0")))

#define stringify(x)			#x
#define def2str(x)			stringify(x)

/** Align down */
#define BASE(x, unit)			((x) & ~((__typeof__(x))(unit) - 1UL))
/** Align up */
#define ALIGN(x, unit)			BASE((x) + ((__typeof__(x))(unit) - 1UL), unit)

#define container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#if defined(__cplusplus)
}
#endif

#endif /* LIBMCU_COMPILER_H */
