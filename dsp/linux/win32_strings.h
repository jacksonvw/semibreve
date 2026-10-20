/*
 * Semibreve — MultiByteToWideChar / WideCharToMultiByte for the vendored DSP.
 *
 * Used by audiopassthru's pstrWide.cpp, which the DSP links against for its
 * UTF-8 <-> wide conversions. That code sits on the preset path, so this is
 * not a stub: it has to be correct.
 *
 * Hand-rolled rather than delegating to mbstowcs/wcstombs. Those depend on
 * LC_CTYPE, and a process that never calls setlocale(LC_ALL, "") runs in the
 * "C" locale, where every byte above 0x7F fails to convert. That failure mode
 * is silent, data-dependent, and would only show up on a preset whose path or
 * name contains a non-ASCII character. Encoding UTF-8 directly removes the
 * dependency altogether.
 *
 * CP_ACP and CP_UTF8 are both treated as UTF-8. On Windows CP_ACP is the
 * legacy system codepage, but the Linux target's narrow encoding is UTF-8
 * everywhere, so the distinction does not survive the port.
 *
 * wchar_t is 32-bit on Linux against 16-bit on Windows, so wide strings here
 * hold UTF-32 code points rather than UTF-16 code units. That is invisible to
 * the DSP, which only ever passes these buffers back to the same routines,
 * but it does mean a code point above U+FFFF occupies one wchar_t here and
 * two on Windows. Nothing in the DSP indexes into wide strings by unit count.
 *
 * Windows semantics reproduced: a source length of -1 means NUL-terminated
 * and the returned count includes the terminator; a destination capacity of 0
 * means "measure only" and returns the required count without writing. Those
 * are the only two forms pstrWide.cpp uses. Anything else returns 0, matching
 * the API's failure convention rather than guessing.
 */
#ifndef SEMIBREVE_WIN32_STRINGS_H_
#define SEMIBREVE_WIN32_STRINGS_H_

#include <stddef.h>
#include <wchar.h>

#define CP_ACP           0
#define CP_UTF8      65001
#define MB_PRECOMPOSED   1

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes needed to encode one code point as UTF-8. Surrogates and out-of-range
 * values are mapped to U+FFFD, which is three bytes. */
static inline int sb_utf8_len(unsigned int cp)
{
	if (cp < 0x80)      return 1;
	if (cp < 0x800)     return 2;
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 3; /* U+FFFD */
	if (cp < 0x10000)   return 3;
	return 4;
}

static inline int sb_utf8_put(unsigned int cp, char *out)
{
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		cp = 0xFFFD;
	if (cp < 0x80) { out[0] = (char)cp; return 1; }
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Decodes one code point, advancing *pp. Malformed input yields U+FFFD and
 * consumes a single byte, so a bad sequence cannot stall or overrun. */
static inline unsigned int sb_utf8_get(const unsigned char **pp)
{
	const unsigned char *p = *pp;
	unsigned int c = p[0];
	int extra, i;
	unsigned int cp;

	if (c < 0x80)             { *pp = p + 1; return c; }
	else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
	else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
	else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
	else                      { *pp = p + 1; return 0xFFFD; }

	for (i = 1; i <= extra; i++) {
		if ((p[i] & 0xC0) != 0x80) { *pp = p + 1; return 0xFFFD; }
		cp = (cp << 6) | (unsigned int)(p[i] & 0x3F);
	}
	*pp = p + extra + 1;
	return cp;
}

static inline int MultiByteToWideChar(unsigned int codepage, unsigned long flags,
                                      const char *src, int srclen,
                                      wchar_t *dst, int dstlen)
{
	const unsigned char *p;
	int n = 0;

	(void)codepage; (void)flags;
	if (src == NULL || srclen != -1)
		return 0;

	p = (const unsigned char *)src;
	while (*p) {
		unsigned int cp = sb_utf8_get(&p);
		if (dstlen > 0) {
			if (n >= dstlen)
				return 0;              /* ERROR_INSUFFICIENT_BUFFER */
			dst[n] = (wchar_t)cp;
		}
		n++;
	}
	if (dstlen > 0) {
		if (n >= dstlen)
			return 0;
		dst[n] = L'\0';
	}
	return n + 1;                              /* includes the terminator */
}

static inline int WideCharToMultiByte(unsigned int codepage, unsigned long flags,
                                      const wchar_t *src, int srclen,
                                      char *dst, int dstlen,
                                      const char *defchar, int *used_default)
{
	int n = 0;
	const wchar_t *w;

	(void)codepage; (void)flags; (void)defchar;
	if (used_default)
		*used_default = 0;
	if (src == NULL || srclen != -1)
		return 0;

	for (w = src; *w; w++) {
		unsigned int cp = (unsigned int)*w;
		if (dstlen > 0) {
			int need = sb_utf8_len(cp);
			if (n + need >= dstlen)
				return 0;
			n += sb_utf8_put(cp, dst + n);
		} else {
			n += sb_utf8_len(cp);
		}
	}
	if (dstlen > 0)
		dst[n] = '\0';
	return n + 1;                              /* includes the terminator */
}

#ifdef __cplusplus
}
#endif

#endif /* SEMIBREVE_WIN32_STRINGS_H_ */
