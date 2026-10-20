/*
 * Semibreve — wide format-string translation for the vendored FxSound DSP.
 *
 * MSVC and glibc disagree about what %s means inside a *wide* format string,
 * and they disagree silently.
 *
 *              MSVC                     glibc / C99
 *   %s         wchar_t*                 char*
 *   %ls        wchar_t*                 wchar_t*
 *   %hs        char*                    (not standard; narrow)
 *   %S         char*                    wchar_t*
 *   %c         wchar_t                  char
 *   %lc        wchar_t                  wchar_t
 *
 * The DSP is written to the MSVC convention — 78 swprintf sites and 34
 * fwprintf sites pass wchar_t* to a plain %s. Under glibc each of those
 * reinterprets the wide pointer as a byte string, so a UTF-32 "Jazz"
 * (4A 00 00 00 61 00 ...) formats as "J" and stops at the first zero byte.
 *
 * This is the most dangerous incompatibility in the port, precisely because
 * nothing reports it. It does not crash, does not warn, and does not return
 * an error — it just silently truncates every string to its first character.
 * It surfaced as valsRead() failing to parse a preset, and the same bug on
 * the fwprintf side would have written corrupted names into every saved
 * preset file.
 *
 * Rewriting 112 format strings across the vendored sources would be a large
 * diff and an easy place to miss one. Translating the format at the call
 * boundary instead keeps upstream's text untouched and fixes every site at
 * once, including any that arrive with a future merge. These calls build
 * trace messages, registry paths and preset fields — never audio — so the
 * per-call cost is irrelevant.
 */
#ifndef SEMIBREVE_PT_WFORMAT_H_
#define SEMIBREVE_PT_WFORMAT_H_

#ifdef __cplusplus

#include <wchar.h>
#include <stddef.h>

#define PT_WFMT_MAX 1024

/*
 * Rewrites one MSVC wide format string into its glibc equivalent, copying
 * everything else through untouched. Returns `out` on success, or `fmt`
 * unchanged if the result would not fit — degrading to upstream's behaviour
 * rather than truncating the format.
 */
static inline const wchar_t *pt_wfmt(const wchar_t *fmt, wchar_t *out, size_t cap)
{
	size_t i = 0, o = 0;

	if (fmt == NULL)
		return fmt;

	while (fmt[i] != L'\0') {
		if (fmt[i] != L'%') {
			if (o + 1 >= cap) return fmt;
			out[o++] = fmt[i++];
			continue;
		}
		/* Literal %% passes through as a pair. */
		if (fmt[i + 1] == L'%') {
			if (o + 2 >= cap) return fmt;
			out[o++] = fmt[i++];
			out[o++] = fmt[i++];
			continue;
		}

		/* Copy the conversion spec, noting any length modifier, and stop at
		 * the conversion character. */
		{
			size_t spec_start = i;
			int is_narrow = 0, is_wide = 0;
			size_t j = i + 1;

			while (fmt[j] != L'\0' && wcschr(L"-+ #0123456789.*", fmt[j]) != NULL)
				j++;
			while (fmt[j] == L'h' || fmt[j] == L'l' || fmt[j] == L'w' ||
			       fmt[j] == L'L' || fmt[j] == L'j' || fmt[j] == L'z' ||
			       fmt[j] == L't' || fmt[j] == L'I' || fmt[j] == L'6' ||
			       fmt[j] == L'4' || fmt[j] == L'3' || fmt[j] == L'2') {
				if (fmt[j] == L'h') is_narrow = 1;
				if (fmt[j] == L'l' || fmt[j] == L'w') is_wide = 1;
				j++;
			}
			if (fmt[j] == L'\0')
				return fmt;                     /* malformed; leave it alone */

			wchar_t conv = fmt[j];
			if (conv == L's' || conv == L'c' || conv == L'S' || conv == L'C') {
				/* MSVC: bare %s/%c are wide; %hs/%hc and %S/%C are narrow. */
				int wide = 1;
				if (is_narrow || conv == L'S' || conv == L'C')
					wide = 0;
				if (is_wide)
					wide = 1;

				wchar_t base = (conv == L'c' || conv == L'C') ? L'c' : L's';

				/* Re-emit flags and width, dropping any length modifier. */
				if (o + (j - spec_start) + 2 >= cap) return fmt;
				out[o++] = L'%';
				{
					size_t k = spec_start + 1;
					while (fmt[k] != L'\0' &&
					       wcschr(L"-+ #0123456789.*", fmt[k]) != NULL)
						out[o++] = fmt[k++];
				}
				if (wide)
					out[o++] = L'l';
				out[o++] = base;
				i = j + 1;
			} else {
				/* Everything else — %d, %f, %g, %x — is spelled the same. */
				size_t n = j - spec_start + 1;
				if (o + n >= cap) return fmt;
				while (spec_start <= j)
					out[o++] = fmt[spec_start++];
				i = j + 1;
			}
		}
	}
	if (o >= cap)
		return fmt;
	out[o] = L'\0';
	return out;
}

#endif /* __cplusplus */

#endif /* SEMIBREVE_PT_WFORMAT_H_ */
