/*
 * Semibreve — narrow line read from a wide-oriented stream.
 *
 * C gives every FILE a byte or wide orientation, fixed by the first I/O call
 * on it, and mixing the two afterwards is undefined. MSVC does not enforce
 * this; glibc does.
 *
 * valsRead() — the preset parser — reads its structural lines with fgetws but
 * deliberately reads the comment line, which holds the preset name, with
 * fgets, because that line is UTF-8 and upstream converts it explicitly.
 * Under glibc the earlier fgetws has already made the stream wide-oriented,
 * so that fgets returns NULL without consuming anything.
 *
 * The failure is quiet and its consequences are not local. The comment line
 * is left in the stream, so the very next fgetws reads the preset name where
 * a number belongs, and every field from there on is off by one line. In
 * testing this produced 60 application integers instead of 7 and an EQ band
 * count of 0, which then failed GraphicEqNew and aborted the load — a preset
 * that parses perfectly on Windows failing on Linux with no diagnostic
 * anywhere near the actual cause.
 *
 * This reads the line the way the stream's orientation requires and hands
 * back the UTF-8 bytes the caller expects. The re-encoding is the
 * locale-independent conversion from win32_strings.h.
 *
 * One residual locale dependency remains and is not ours to remove: glibc's
 * fgetws decodes the file's bytes through LC_CTYPE, so a preset name
 * containing non-ASCII needs the process to be in a UTF-8 locale. Call
 * setlocale(LC_ALL, "") at startup. Every stock FxSound preset name is ASCII,
 * so this only affects user-authored ones.
 */
#ifndef SEMIBREVE_PT_STREAM_H_
#define SEMIBREVE_PT_STREAM_H_

#include <stdio.h>
#include <wchar.h>

#include "win32_strings.h"

#define PT_STREAM_LINE_MAX 4096

#ifdef __cplusplus
extern "C" {
#endif

static inline char *pt_fgets_utf8(char *buf, int n, FILE *stream)
{
	wchar_t wline[PT_STREAM_LINE_MAX];

	if (buf == NULL || n <= 0)
		return NULL;
	buf[0] = '\0';

	if (fgetws(wline, PT_STREAM_LINE_MAX, stream) == NULL)
		return NULL;
	if (WideCharToMultiByte(CP_UTF8, 0, wline, -1, buf, n, 0, 0) == 0) {
		buf[0] = '\0';
		return NULL;
	}
	return buf;
}

#ifdef __cplusplus
}
#endif

#endif /* SEMIBREVE_PT_STREAM_H_ */
