dnl Process this file with autoconf to produce a configure script.

AC_INIT([hotcrp-comet],[0.1])
AC_CONFIG_AUX_DIR([tamer])
AC_PREREQ([2.69])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([GNUmakefile])
AC_CONFIG_SUBDIRS([tamer])
AC_SUBST([ac_configure_args])

ac_user_cxx=${CXX+y}
ac_user_cxxflags=${CXXFLAGS+y}

AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_LANG([C++])

AC_DEFINE([WORDS_BIGENDIAN_SET], [1], [Define if WORDS_BIGENDIAN has been set.])
AC_C_BIGENDIAN()

AC_CHECK_HEADERS([sys/epoll.h numa.h])

AC_SEARCH_LIBS([numa_available], [numa], [AC_DEFINE([HAVE_LIBNUMA], [1], [Define if you have libnuma.])])


dnl Builtins

AC_DEFUN([KVDB_CHECK_BUILTIN], [
    AC_CACHE_CHECK([for $1 builtin], [ac_cv_have_$1],
	[AC_LINK_IFELSE([AC_LANG_PROGRAM([$2], [])],
	    [ac_cv_have_$1=yes], [ac_cv_have_$1=no])])
    if test $ac_cv_have_$1 = yes; then
	AC_DEFINE(AS_TR_CPP([HAVE_$1]), [1], [Define if you have the $1 builtin.])
    fi
])

KVDB_CHECK_BUILTIN([__builtin_clz],
    [[unsigned f(unsigned x) { return __builtin_clz(x); }]])

KVDB_CHECK_BUILTIN([__builtin_clzl],
    [[unsigned long f(unsigned long x) { return __builtin_clzl(x); }]])

KVDB_CHECK_BUILTIN([__builtin_clzll],
    [[unsigned long long f(unsigned long long x) { return __builtin_clzll(x); }]])

KVDB_CHECK_BUILTIN([__builtin_ctz],
    [[unsigned f(unsigned x) { return __builtin_ctz(x); }]])

KVDB_CHECK_BUILTIN([__builtin_ctzl],
    [[unsigned long f(unsigned long x) { return __builtin_ctzl(x); }]])

KVDB_CHECK_BUILTIN([__builtin_ctzll],
    [[unsigned long long f(unsigned long long x) { return __builtin_ctzll(x); }]])

KVDB_CHECK_BUILTIN([__sync_synchronize], [[long x = 11;
    void f(long i) { long* y = &x; __sync_synchronize(); *y = i; }]])

KVDB_CHECK_BUILTIN([__sync_fetch_and_add],
    [[long f(long* x) { return __sync_fetch_and_add(x, 2L); }]])

KVDB_CHECK_BUILTIN([__sync_add_and_fetch],
    [[long f(long* x) { return __sync_add_and_fetch(x, 2L); }]])

KVDB_CHECK_BUILTIN([__sync_fetch_and_add_8],
    [[#include <stdint.h>
    int64_t f(int64_t* x) { return __sync_fetch_and_add(x, (int64_t) 2); }]])

KVDB_CHECK_BUILTIN([__sync_add_and_fetch_8],
    [[#include <stdint.h>
    int64_t f(int64_t* x) { return __sync_add_and_fetch(x, (int64_t) 2); }]])

KVDB_CHECK_BUILTIN([__sync_fetch_and_or],
    [[long f(long* x) { return __sync_fetch_and_or(x, 2L); }]])

KVDB_CHECK_BUILTIN([__sync_or_and_fetch],
    [[long f(long* x) { return __sync_or_and_fetch(x, 2L); }]])

KVDB_CHECK_BUILTIN([__sync_fetch_and_or_8],
    [[#include <stdint.h>
    int64_t f(int64_t* x) { return __sync_fetch_and_or(x, (int64_t) 2); }]])

KVDB_CHECK_BUILTIN([__sync_or_and_fetch_8],
    [[#include <stdint.h>
    int64_t f(int64_t* x) { return __sync_or_and_fetch(x, (int64_t) 2); }]])

KVDB_CHECK_BUILTIN([__sync_bool_compare_and_swap],
    [[bool f(long* x, long y, long z) { return __sync_bool_compare_and_swap(x, y, z); }]])

KVDB_CHECK_BUILTIN([__sync_bool_compare_and_swap_8],
    [[#include <stdint.h>
    bool f(int64_t* x, int64_t y, int64_t z) { return __sync_bool_compare_and_swap(x, y, z); }]])

KVDB_CHECK_BUILTIN([__sync_val_compare_and_swap],
    [[long f(long* x, long y, long z) { return __sync_val_compare_and_swap(x, y, z); }]])

KVDB_CHECK_BUILTIN([__sync_val_compare_and_swap_8],
    [[#include <stdint.h>
    int64_t f(int64_t* x, int64_t y, int64_t z) { return __sync_val_compare_and_swap(x, y, z); }]])

KVDB_CHECK_BUILTIN([__sync_lock_test_and_set],
    [[long f(long* x) { return __sync_lock_test_and_set(x, 1); }]])

KVDB_CHECK_BUILTIN([__sync_lock_test_and_set_val],
    [[long f(long* x, long y) { return __sync_lock_test_and_set(x, y); }]])

KVDB_CHECK_BUILTIN([__sync_lock_release_set],
    [[void f(long* x) { __sync_lock_release(x); }]])


dnl
dnl C++17 features
dnl

SAVE_CXX="$CXX"
if test -n "$GCC"; then CXX="$CXX -Werror"; fi

AC_MSG_CHECKING([whether the C++ compiler supports C++17])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#if __cplusplus >= 201703L
int f() { return 0; }
#endif]], [[return f();]])],
    [ac_cv_cxx17=yes], [ac_cv_cxx17=no])
AC_MSG_RESULT([$ac_cv_cxx17])
if test "$ac_cv_cxx17" != yes; then
    SAVE_CXX="${SAVE_CXX} -std=gnu++20"
    CXX="${CXX} -std=gnu++20"
    AC_MSG_CHECKING([whether the C++ compiler with -std=gnu++20 supports C++20])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#if __cplusplus >= 202002L
int f() { return 0; }
#endif]], [[return f();]])],
        [ac_cv_cxx17=yes], [ac_cv_cxx17=no])
    AC_MSG_RESULT([$ac_cv_cxx17])
fi

AC_CACHE_CHECK([whether the C++ compiler understands rvalue references], [ac_cv_cxx_rvalue_references], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[int f(int &) { return 1; } int f(int &&) { return 0; }]], [[return f(int());]])],
	[ac_cv_cxx_rvalue_references=yes], [ac_cv_cxx_rvalue_references=no])])
if test "$ac_cv_cxx_rvalue_references" = yes; then
    AC_DEFINE([TAMER_HAVE_CXX_RVALUE_REFERENCES], [1], [Define if the C++ compiler understands rvalue references.])
fi

AC_MSG_CHECKING([for std::move])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <utility>], [[long x = 0; long &&y = std::move(x); (void) y;]])], [ac_cv_std_move=yes], [ac_cv_std_move=no])
AC_MSG_RESULT([$ac_cv_std_move])

AC_MSG_CHECKING([for variadic templates])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <utility>
#include <cstddef>
template <typename... TS> struct foo { static constexpr size_t count = sizeof...(TS); };], [[long x = foo<>::count + foo<int, int, int, int, int>::count; (void) x;]])], [ac_cv_variadic_templates=yes], [ac_cv_variadic_templates=no])
AC_MSG_RESULT([$ac_cv_variadic_templates])

if test "$ac_cv_std_move" != yes -o "$ac_cv_variadic_templates" != yes -o "$ac_cv_cxx17" != yes; then
    AC_MSG_ERROR([

The C++ compiler does not appear to understand C++20.
To fix this problem, try supplying a "CXX" argument to ./configure,
such as "./configure CXX='c++ -std=gnu++20'".
If you are using clang on Mac, ensure the '-stdlib=libc++' option.
])
fi

AC_CACHE_CHECK([whether the C++ compiler understands constexpr], [ac_cv_cxx_constexpr], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[constexpr int f(int x) { return x + 1; }]], [[]])],
	[ac_cv_cxx_constexpr=yes], [ac_cv_cxx_constexpr=no])])
if test "$ac_cv_cxx_constexpr" = yes; then
    AC_DEFINE([HAVE_CXX_CONSTEXPR], [1], [Define if the C++ compiler understands constexpr.])
fi

AC_CACHE_CHECK([whether the C++ compiler understands noexcept], [ac_cv_cxx_noexcept], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[int f(int x) noexcept { return x + 1; }]], [[]])],
	[ac_cv_cxx_noexcept=yes], [ac_cv_cxx_noexcept=no])])
if test "$ac_cv_cxx_noexcept" = yes; then
    AC_DEFINE([HAVE_CXX_NOEXCEPT], [1], [Define if the C++ compiler understands noexcept.])
fi

AC_CACHE_CHECK([whether the C++ compiler understands static_assert], [ac_cv_cxx_static_assert], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[const int f = 2;]], [[static_assert(f == 2, "f should be 2");]])],
	[ac_cv_cxx_static_assert=yes], [ac_cv_cxx_static_assert=no])])
if test "$ac_cv_cxx_static_assert" = yes; then
    AC_DEFINE([HAVE_CXX_STATIC_ASSERT], [1], [Define if the C++ compiler understands static_assert.])
fi

AC_CACHE_CHECK([whether the C++ compiler understands template alias], [ac_cv_cxx_template_alias], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[template <typename T> struct X { typedef T type; }; template <typename T> using Y = X<T>; int f(int x) { return x; }]], [[return f(Y<int>::type());]])],
	[ac_cv_cxx_template_alias=yes], [ac_cv_cxx_template_alias=no])])
if test "$ac_cv_cxx_template_alias" = yes; then
    AC_DEFINE([HAVE_CXX_TEMPLATE_ALIAS], [1], [Define if the C++ compiler understands template alias.])
fi

AC_CACHE_CHECK([whether the C++ compiler understands user-defined literals], [ac_cv_cxx_user_literals], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <stddef.h>
size_t operator"" _LEN(const char *, size_t len) { return len; } int f(size_t x) { return x > 2; }]], [[return f("ABC"_LEN);]])],
	[ac_cv_cxx_user_literals=yes], [ac_cv_cxx_user_literals=no])])
if test "$ac_cv_cxx_user_literals" = yes; then
    AC_DEFINE([HAVE_CXX_USER_LITERALS], [1], [Define if the C++ compiler understands user-defined literals.])
fi

AC_CHECK_HEADERS([type_traits])

AC_CACHE_CHECK([for std::hash], [ac_cv_have_std_hash], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <functional>
#include <stddef.h>],
        [[std::hash<int> h; size_t x = h(1); return x == 0;]])],
        [ac_cv_have_std_hash=yes], [ac_cv_have_std_hash=no])])
if test $ac_cv_have_std_hash = yes; then
    AC_DEFINE([HAVE_STD_HASH], [1], [Define if you have std::hash.])
fi

AC_CACHE_CHECK([for __has_trivial_copy], [ac_cv_have___has_trivial_copy], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [[long x = 1; if (__has_trivial_copy(long)) x = 0;]])], [ac_cv_have___has_trivial_copy=yes], [ac_cv_have___has_trivial_copy=no])])
if test $ac_cv_have___has_trivial_copy = yes; then
    AC_DEFINE([HAVE___HAS_TRIVIAL_COPY], [1], [Define if you have the __has_trivial_copy compiler intrinsic.])
fi

AC_CACHE_CHECK([for std::is_trivially_copyable], [ac_cv_have_std_is_trivially_copyable], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <type_traits>], [[return std::is_trivially_copyable<int>::value;]])], [ac_cv_have_std_is_trivially_copyable=yes], [ac_cv_have_std_is_trivially_copyable=no])])
if test $ac_cv_have_std_is_trivially_copyable = yes; then
    AC_DEFINE([HAVE_STD_IS_TRIVIALLY_COPYABLE], [1], [Define if you have the std::is_trivially_copyable template.])
fi

AC_CACHE_CHECK([for std::is_rvalue_reference], [ac_cv_have_std_is_rvalue_reference], [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <type_traits>], [[return std::is_rvalue_reference<int>::value;]])], [ac_cv_have_std_is_rvalue_reference=yes], [ac_cv_have_std_is_rvalue_reference=no])])
if test $ac_cv_have_std_is_rvalue_reference = yes; then
    AC_DEFINE([HAVE_STD_IS_RVALUE_REFERENCE], [1], [Define if you have the std::is_rvalue_reference template.])
fi

CXX="$SAVE_CXX"


dnl Memory allocator

AC_CHECK_LIB([flow], [malloc], [have_flow=true], [have_flow=])
AC_CHECK_LIB([jemalloc], [mallctl], [have_jemalloc=true], [have_jemalloc=])
AC_CHECK_LIB([tcmalloc_minimal], [tc_malloc], [have_tcmalloc_minimal=true], [have_tcmalloc_minimal=])
AC_CHECK_LIB([hoard], [_Z16getMainHoardHeapv], [have_hoard=true], [have_hoard=])

AC_ARG_WITH([malloc],
    [AS_HELP_STRING([--with-malloc=TYPE],
                    [memory allocator (malloc|jemalloc|tcmalloc|hoard|flow)])],
    [ac_mtd_malloc=$withval], [ac_mtd_malloc=yes])

if test \( "$ac_mtd_malloc" = tcmalloc -a -z "$have_tcmalloc_minimal" \) \
	-o \( "$ac_mtd_malloc" = jemalloc -a -z "$have_jemalloc" \) \
	-o \( "$ac_mtd_malloc" = flow -a -z "$have_flow" \) \
        -o \( "$ac_mtd_malloc" = hoard -a -z "$have_hoard" \) ; then
    AC_MSG_ERROR([$ac_mtd_malloc not found])
elif test "$ac_mtd_malloc" = tcmalloc -o "$ac_mtd_malloc" = jemalloc -o "$ac_mtd_malloc" = flow -o "$ac_mtd_malloc" = hoard; then
    :
elif test "$ac_mtd_malloc" = yes -o "$ac_mtd_malloc" = default; then
    AC_MSG_CHECKING([for malloc library])
    if test -n "$have_flow"; then ac_mtd_malloc=flow;
    elif test -n "$have_jemalloc"; then ac_mtd_malloc=jemalloc;
    elif test -n "$have_tcmalloc_minimal"; then ac_mtd_malloc=tcmalloc;
    else ac_mtd_malloc=malloc; fi
    AC_MSG_RESULT([$ac_mtd_malloc])
elif test "$ac_mtd_malloc" = no -o "$ac_mtd_malloc" = malloc -o -z "$ac_mtd_malloc"; then
    ac_mtd_malloc=malloc
else
    AC_MSG_ERROR([Unknown malloc type $ac_mtd_malloc])
fi

if test "$ac_mtd_malloc" = tcmalloc; then
    MALLOC_LIBS="-ltcmalloc_minimal"
    AC_DEFINE([HAVE_TCMALLOC], [1], [Define if you are using libtcmalloc for malloc.])
elif test "$ac_mtd_malloc" = jemalloc; then
    MALLOC_LIBS="-ljemalloc"
    AC_DEFINE([HAVE_JEMALLOC], [1], [Define if you are using libjemalloc for malloc.])
elif test "$ac_mtd_malloc" = flow; then
    MALLOC_LIBS="-lflow"
    AC_DEFINE([HAVE_FLOW_MALLOC], [1], [Define if you are using libflow for malloc.])
elif test "$ac_mtd_malloc" = hoard; then
    MALLOC_LIBS="-lhoard"
    AC_DEFINE([HAVE_HOARD_MALLOC], [1], [Define if you are using libhoard for malloc.])
else
    MALLOC_LIBS=
fi
AC_SUBST(MALLOC_LIBS)


dnl Types

AC_DEFUN([KVDB_CHECK_SAME_TYPE], [
    pushdef([KVDB_CST_VAR], [AS_TR_SH([ac_cv_have_same_type_$1_is_$2])])
    AC_CACHE_CHECK([whether $1 and $2 are the same type], KVDB_CST_VAR,
	[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([$3
int f($1) {return 0;} int f($2) {return 0;}], [])],
	    [KVDB_CST_VAR=no], [KVDB_CST_VAR=yes])])
    if test $KVDB_CST_VAR = yes; then
	AC_DEFINE(AS_TR_CPP([HAVE_$1_IS_$2]), [1], [Define if $1 and $2 are the same type.])
    fi
    popdef([KVDB_CST_VAR])
])

KVDB_CHECK_SAME_TYPE([off_t], [long], [#include <stdio.h>])
KVDB_CHECK_SAME_TYPE([off_t], [long long], [#include <stdio.h>])
KVDB_CHECK_SAME_TYPE([int64_t], [long], [#include <stdint.h>])
KVDB_CHECK_SAME_TYPE([int64_t], [long long], [#include <stdint.h>])
KVDB_CHECK_SAME_TYPE([size_t], [unsigned], [#include <stdio.h>])
KVDB_CHECK_SAME_TYPE([size_t], [unsigned long], [#include <stdio.h>])
KVDB_CHECK_SAME_TYPE([size_t], [unsigned long long], [#include <stdio.h>])

AC_CHECK_TYPES([long long])
AC_CHECK_SIZEOF([short])
AC_CHECK_SIZEOF([int])
AC_CHECK_SIZEOF([long])
AC_CHECK_SIZEOF([long long])
AC_CHECK_SIZEOF([void *])

AC_CHECK_DECLS([getline])
AC_CHECK_FUNCS([vsnprintf])

AC_CHECK_HEADERS([time.h])
AC_CHECK_DECLS([clock_gettime], [], [], [#if HAVE_TIME_H
# include <time.h>
#endif])
AC_SEARCH_LIBS([clock_gettime], [rt])
AC_CHECK_FUNCS([clock_gettime])

AC_MSG_CHECKING([whether MADV_HUGEPAGE is supported])
AC_PREPROC_IFELSE([AC_LANG_PROGRAM([[#include <sys/mman.h>
#ifndef MADV_HUGEPAGE
#error "no"
#endif]], [])],
                  [have_madv_hugepage=yes], [have_madv_hugepage=no])
AC_MSG_RESULT([$have_madv_hugepage])
if test $have_madv_hugepage = yes; then
    AC_DEFINE([HAVE_MADV_HUGEPAGE], [1], [Define if MADV_HUGEPAGE is supported.])
fi

AC_MSG_CHECKING([whether MAP_HUGETLB is supported])
AC_PREPROC_IFELSE([AC_LANG_PROGRAM([[#include <sys/mman.h>
#ifndef MAP_HUGETLB
#error "no"
#endif]], [])],
                  [have_map_hugetlb=yes], [have_map_hugetlb=no])
AC_MSG_RESULT([$have_map_hugetlb])
if test $have_map_hugetlb = yes; then
    AC_DEFINE([HAVE_MAP_HUGETLB], [1], [Define if MAP_HUGETLB is supported.])
fi

AC_ARG_ENABLE([superpage],
    [AS_HELP_STRING([--disable-superpage],
	    [disable superpage support])],
    [], [enable_superpage=maybe])
if test "$enable_superpage $have_madv_hugepage $have_map_hugetlb" = "yes no no"; then
    AC_MSG_ERROR([
Error: superpages are not supported on this machine.
Try again without --enable-superpage.
])
elif test "$enable_superpage $have_madv_hugepage $have_map_hugetlb" != "maybe no no" -a "$enable_superpage" != no; then
    AC_DEFINE_UNQUOTED([HAVE_SUPERPAGE], [1], [Define if superpage support is enabled.])
fi

AC_ARG_ENABLE([memdebug],
    [AS_HELP_STRING([--enable-memdebug],
	    [enable memory debugging])])
if test "$enable_memdebug" = yes; then
    AC_DEFINE_UNQUOTED([HAVE_MEMDEBUG], [1], [Define if memory debugging support is enabled.])
fi

AC_ARG_ENABLE([assert],
    [],
    [AC_MSG_WARN([Use --disable-assertions instead of --disable-assert.])])
AC_ARG_ENABLE([assertions],
    [AS_HELP_STRING([--disable-assertions],
	    [disable debugging assertions])])
if test "$enable_assertions" != no -o "(" -z "$enable_assertions" -a "$enable_assert" != no ")"; then
    AC_DEFINE_UNQUOTED([HAVE_ASSERTIONS_ENABLED], [1], [Define to enable debugging assertions.])
fi

AC_ARG_ENABLE([preconditions],
    [AS_HELP_STRING([--disable-preconditions],
            [disable precondition assertions])])
if test "$enable_preconditions" = no; then
    AC_DEFINE_UNQUOTED([HAVE_PRECONDITIONS_ENABLED], [0], [Define to enable precondition assertions.])
elif test -n "$enable_preconditions"; then
    AC_DEFINE_UNQUOTED([HAVE_PRECONDITIONS_ENABLED], [1], [Define to enable precondition assertions.])
fi

AC_ARG_ENABLE([invariants],
    [AS_HELP_STRING([--disable-invariants],
            [disable invariant assertions])])
if test "$enable_invariants" = no; then
    AC_DEFINE_UNQUOTED([HAVE_INVARIANTS_ENABLED], [0], [Define to enable invariant assertions.])
elif test -n "$enable_preconditions"; then
    AC_DEFINE_UNQUOTED([HAVE_INVARIANTS_ENABLED], [1], [Define to enable invariant assertions.])
fi

AC_DEFINE_UNQUOTED([CACHE_LINE_SIZE], [64], [Assumed size of a cache line.])

AC_ARG_ENABLE([tamer-debug],
    [AS_HELP_STRING([--enable-tamer-debug],
	    [Enable extra tamer debugging])],
    [], [enable_tamer_debug=no])
TAMERFLAGS=
if test "$enable_tamer_debug" != no; then
    TAMERFLAGS=-g
fi
AC_SUBST([TAMERFLAGS])

AH_TOP([#ifndef MASSTREE_CONFIG_H_INCLUDED
#define MASSTREE_CONFIG_H_INCLUDED 1])

AH_BOTTOM([#if !HAVE_ALL_ASSERTIONS_ENABLED && !HAVE_ASSERTIONS_ENABLED
# define NDEBUG 1
#endif

/** @brief Assert macro that always runs. */
extern void fail_always_assert(const char* file, int line, const char* assertion, const char* message = 0) __attribute__((noreturn));
#define always_assert(x, ...) do { if (!(x)) fail_always_assert(__FILE__, __LINE__, #x, ## __VA_ARGS__); } while (0)
#define mandatory_assert always_assert

/** @brief Assert macro for invariants.

    masstree_invariant(x) is executed if --enable-invariants or
    --enable-assertions. */
extern void fail_masstree_invariant(const char* file, int line, const char* assertion, const char* message = 0) __attribute__((noreturn));
#if HAVE_ALL_ASSERTIONS_ENABLED || (!defined(HAVE_INVARIANTS_ENABLED) && HAVE_ASSERTIONS_ENABLED) || HAVE_INVARIANTS_ENABLED
#define masstree_invariant(x, ...) do { if (!(x)) fail_masstree_invariant(__FILE__, __LINE__, #x, ## __VA_ARGS__); } while (0)
#else
#define masstree_invariant(x, ...) do { } while (0)
#endif

/** @brief Assert macro for preconditions.

    masstree_precondition(x) is executed if --enable-preconditions or
    --enable-assertions. */
extern void fail_masstree_precondition(const char* file, int line, const char* assertion, const char* message = 0) __attribute__((noreturn));
#if HAVE_ALL_ASSERTIONS_ENABLED || (!defined(HAVE_PRECONDITIONS_ENABLED) && HAVE_ASSERTIONS_ENABLED) || HAVE_PRECONDITIONS_ENABLED
#define masstree_precondition(x, ...) do { if (!(x)) fail_masstree_precondition(__FILE__, __LINE__, #x, ## __VA_ARGS__); } while (0)
#else
#define masstree_precondition(x, ...) do { } while (0)
#endif

#ifndef invariant
#define invariant masstree_invariant
#endif
#ifndef precondition
#define precondition masstree_precondition
#endif

#endif])

AC_DEFINE_UNQUOTED([HAVE_UNALIGNED_ACCESS], [1], [Define if unaligned accesses are OK.])

AC_OUTPUT
