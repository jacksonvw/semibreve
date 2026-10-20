/*
 * Semibreve — swprintf compatibility for the vendored FxSound DSP.
 *
 * The sources mix two incompatible spellings of swprintf:
 *
 *   swprintf(buf, L"%s", x)        MSVC non-conforming, 141 call sites
 *   swprintf(buf, n, L"%s", x)     ISO C99, used by the newer FxSound code
 *
 * Upstream compiles the first form by defining _CRT_NON_CONFORMING_SWPRINTFS.
 * glibc only has the ISO form, so the legacy calls bind the format string to
 * the size_t count parameter. That is why this cannot be waved through with
 * -fpermissive: it would compile, reinterpret a pointer as a length, and
 * write past the buffer.
 *
 * Rewriting 141 call sites would be a large diff against code we have
 * committed to keeping close to upstream, and every rewrite is a chance to
 * type the wrong size. Instead the count is recovered from the type.
 *
 * An overload rather than a macro
 * -------------------------------
 * The obvious implementation is `#define swprintf pt_swprintf`. It does not
 * survive: libstdc++'s <cwchar> does `#undef swprintf` before re-exporting
 * the function into namespace std, so any translation unit that reaches a
 * standard header after this one silently loses the substitution and fails
 * against glibc's prototype. DfxDspEq.cpp does exactly that, via <string>.
 *
 * Declaring a real overload instead is immune to include order and to
 * #undef. Resolution against glibc's variadic declaration is unambiguous in
 * both directions: a format literal cannot bind to size_t, so the ISO calls
 * only match glibc's, and a size_t cannot bind to const wchar_t*, so the
 * legacy calls only match this one. The ISO form needs no overload here at
 * all — it already resolves to the C library.
 *
 * C++ only. No .c file in the build uses the two-argument form (verified
 * across all sources), so this stays out of the C compiler's way.
 */
#ifndef SEMIBREVE_PT_SWPRINTF_H_
#define SEMIBREVE_PT_SWPRINTF_H_

#ifdef __cplusplus

#include <wchar.h>
#include <stddef.h>
#include <stdio.h>
#include <type_traits>

/* Rewrites %s/%c to the wide spellings glibc expects. Applied to every wide
 * format below — see pt_wformat.h for why this is not optional. */
#include "pt_wformat.h"

/*
 * Pointers to the C library functions, captured here — before the overloads
 * below are declared — so each initialiser can only see the C declaration.
 *
 * Calling ::swprintf from inside the overloads does not work: these
 * overloads live in the global namespace too, so the qualified name finds
 * them as well, and the template is the better match. Every call then
 * re-entered itself until the stack ran out. Binding the address up front
 * freezes the resolution at a point where only one candidate exists.
 */
namespace pt_detail {
	static int (* const c_swprintf)(wchar_t *, size_t, const wchar_t *, ...) = ::swprintf;
	static int (* const c_fwprintf)(FILE *, const wchar_t *, ...)            = ::fwprintf;
	static int (* const c_swscanf)(const wchar_t *, const wchar_t *, ...)    = ::swscanf;
}

/*
 * Scratch size for the pointer case below. Comfortably above anything the
 * formats reaching it can produce — they are %d, %.2f, and %s of paths that
 * upstream caps at PT_MAX_PATH_STRLEN = 1024.
 */
#define PT_SWPRINTF_SCRATCH 4096

/*
 * MSVC two-argument form.
 *
 * Deliberately one function rather than an array overload plus a pointer
 * overload: a by-value pointer parameter also wins array arguments by decay,
 * so the two are ambiguous at every array call site. A forwarding reference
 * preserves the array type, and the branch resolves at compile time.
 *
 *   Array destination  — capacity is the real extent, the same bound MSVC's
 *                        version enforces. This is the common case.
 *
 *   Pointer destination — about twenty sites: out-parameters like
 *                        dfxpEqGetBandBoostCut's wcp_boost_cut, plus heap
 *                        buffers in VALS and PRELST. A pointer carries no
 *                        capacity, and MSVC's answer is that there is none —
 *                        its swprintf writes unbounded, like wcscpy, and the
 *                        caller guarantees the buffer. Formatting into
 *                        scratch and copying out reproduces that exactly.
 *                        Choosing a bound instead would silently truncate
 *                        output upstream writes in full, and truncation
 *                        inside preset parsing is precisely what the
 *                        Milestone 0 round-trip test exists to catch.
 *                        Overflow behaviour is therefore identical to
 *                        upstream's: no worse, and no better. A format that
 *                        did exceed the scratch fails loudly with a negative
 *                        return rather than corrupting the stack.
 */
template <typename T, typename... Args>
inline int swprintf(T &&buf, const wchar_t *fmt, Args... args)
{
	typedef typename std::remove_reference<T>::type B;

	wchar_t fbuf[PT_WFMT_MAX];
	const wchar_t *f = pt_wfmt(fmt, fbuf, PT_WFMT_MAX);

	if constexpr (std::is_array<B>::value) {
		return pt_detail::c_swprintf(buf, std::extent<B>::value, f, args...);
	} else {
		wchar_t scratch[PT_SWPRINTF_SCRATCH];
		int n = pt_detail::c_swprintf(scratch, PT_SWPRINTF_SCRATCH, f, args...);
		if (n < 0)
			return n;
		wmemcpy(buf, scratch, (size_t)n + 1);
		return n;
	}
}

/*
 * The ISO three-argument form needs the same translation — DfxDsp's newer
 * code uses it with %s and wide arguments just as the legacy code does.
 * Unambiguous against the template above: a size_t cannot bind to the
 * const wchar_t* format parameter.
 */
template <typename... Args>
inline int swprintf(wchar_t *buf, size_t count, const wchar_t *fmt, Args... args)
{
	wchar_t fbuf[PT_WFMT_MAX];
	return pt_detail::c_swprintf(buf, count, pt_wfmt(fmt, fbuf, PT_WFMT_MAX), args...);
}

/*
 * Preset *writing* goes through fwprintf, so the same divergence would put
 * one-character names and paths into every saved .fac file. These template
 * overloads beat the C variadic declarations in overload resolution, because
 * passing through an ellipsis ranks worse than an exact match.
 */
template <typename... Args>
inline int fwprintf(FILE *stream, const wchar_t *fmt, Args... args)
{
	wchar_t fbuf[PT_WFMT_MAX];
	return pt_detail::c_fwprintf(stream, pt_wfmt(fmt, fbuf, PT_WFMT_MAX), args...);
}

/* Preset *reading* goes through swscanf, where %s means wchar_t* to MSVC and
 * char* to glibc — the same trap, pointed the other way. */
template <typename... Args>
inline int swscanf(const wchar_t *src, const wchar_t *fmt, Args... args)
{
	wchar_t fbuf[PT_WFMT_MAX];
	return pt_detail::c_swscanf(src, pt_wfmt(fmt, fbuf, PT_WFMT_MAX), args...);
}

#endif /* __cplusplus */

#endif /* SEMIBREVE_PT_SWPRINTF_H_ */
