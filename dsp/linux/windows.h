/*
 * Semibreve — Win32 type shim for the vendored FxSound DSP.
 *
 * The DSP itself is portable scalar C: no SSE intrinsics, no inline asm,
 * no threads, no locks, and no allocation on the process path. What it
 * does have is thirty years of <windows.h> in its includes, pulled in by
 * codedefs.h and pt_defs.h purely for typedefs. Across all 94 built
 * sources the actual Win32 *call* surface is six functions, every one of
 * which is either dead code or off the audio path.
 *
 * So this is a type shim, not an emulation layer. It declares the handful
 * of Windows spellings the sources mention and nothing else. There is no
 * implementation here and there must never be one — if a future upstream
 * merge starts genuinely calling Win32, that is a porting decision to make
 * deliberately, not something to paper over by growing this file.
 *
 * Deliberately absent: HRESULT/COM, CRITICAL_SECTION, and the waveOut and
 * registry families. Anything reaching for those has strayed off the DSP
 * path and should fail loudly at compile time.
 */
#ifndef SEMIBREVE_WIN32_SHIM_H_
#define SEMIBREVE_WIN32_SHIM_H_

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
/* <windows.h> transitively supplies the character-classification functions on
 * MSVC; pstr.cpp and pstrURL.cpp rely on that and include neither directly. */
#include <ctype.h>
#include <wctype.h>

/* UTF-8 <-> wide conversion, on the preset path. See the header for why it is
 * hand-rolled rather than locale-dependent. */
#include "win32_strings.h"

/* MSVC's sized-integer spelling. A macro rather than a typedef because the
 * sources write "unsigned __int64", and only a keyword can follow "unsigned". */
#define __int64 long long

/* MSVC wide-string conversions used by dfxpEq/dfxpSession for preset parsing. */
#define _wtoi(s)  ((int)wcstol((s), NULL, 10))
#define _wtol(s)  (wcstol((s), NULL, 10))
#define _wtof(s)  (wcstod((s), NULL))

/* Integer spellings. Sized to the Win32 LLP64 definitions rather than the
 * host's, since the DSP casts between these and its own types. */
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef uint32_t            DWORD;
typedef int32_t             LONG;
typedef uint32_t            ULONG;
typedef int                 BOOL;
typedef unsigned int        UINT;

/* Opaque handles. The DSP stores these but never dereferences them; the
 * only producer, dfxSharedUtil's LoadLibrary call, is stubbed on Linux. */
typedef void               *HANDLE;
typedef void               *HWND;
typedef void               *HINSTANCE;
typedef void               *HMODULE;
typedef void               *LPVOID;

/* Message-parameter types. Referenced only in spectrum.h function
 * signatures for a Win32 message pump the Linux build never runs. */
typedef uintptr_t           WPARAM;
typedef intptr_t            LPARAM;
typedef intptr_t            LRESULT;

/* String spellings. */
typedef char               *LPSTR;
typedef const char         *LPCSTR;
/* Huge-pointer string, a 16-bit-era spelling still used in ptutil/PWAV. */
typedef char               *HPSTR;
typedef wchar_t            *LPWSTR;
typedef const wchar_t      *LPCWSTR;

/* TCHAR family. UNICODE is not defined for this build, so these follow the
 * narrow spelling, matching how upstream compiles its non-UNICODE targets. */
typedef char                TCHAR;
typedef char               *LPTSTR;
typedef const char         *LPCTSTR;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Appears only in file-creation prototypes the Linux build never calls. */
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; }
	SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

/* 100ns-tick timestamp. dfxpUniversal declares one to hold a file time it
 * never reads on this build; the layout only has to match in size. */
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

/* Calling-convention markers, meaningless on the SysV ABI. */
#define WINAPI
#define CALLBACK
#define APIENTRY

/*
 * The three function-level shims the build genuinely needs.
 *
 * LoadLibrary/GetProcAddress/FreeLibrary appear in exactly one place:
 * dfxSharedUtil, which uses a DLL's shared data segment as cross-process
 * shared memory to hand spectrum values and elapsed-time to the GUI process.
 * Semibreve's UI gets those over D-Bus instead, so these return failure and
 * the module runs with a NULL data pointer — a state upstream already
 * handles, since every dfxSharedUtil accessor null-checks before use.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

/* MSVC's bounds-checked wcstok. Its contract matches glibc's three-argument
 * wcstok exactly — same arguments, same meaning — so this is a rename. */
#define wcstok_s(s, d, c)  wcstok((s), (d), (c))

/* pstr.cpp reads this after a failed call for diagnostics it then discards.
 * errno is the closest equivalent and costs nothing to supply. */
#include <errno.h>
static inline DWORD GetLastError(void) { return (DWORD)errno; }

typedef int errno_t;

/*
 * Error-text formatting. Both of these exist only to build human-readable
 * strings for CSlout trace output, so strerror carries the real information
 * across and the Windows-specific machinery around it does not need to.
 *
 * _wcserror returns a pointer to a static buffer, matching the CRT — callers
 * pass the result straight into a format string and never free or retain it.
 */
static inline wchar_t *_wcserror(int errnum)
{
	static wchar_t wbuf[256];
	const char *s = strerror(errnum);
	size_t i = 0;
	if (s == NULL)
		s = "unknown error";
	/* strerror output is ASCII on glibc, so a byte-wise widen is exact. */
	for (; s[i] != '\0' && i < (sizeof(wbuf) / sizeof(wbuf[0])) - 1; i++)
		wbuf[i] = (wchar_t)(unsigned char)s[i];
	wbuf[i] = L'\0';
	return wbuf;
}

#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100

/* Only ever called as FormatMessage(FROM_SYSTEM, NULL, err, 0, buf, len, NULL)
 * to turn a GetLastError value into text. Returns the character count, or 0
 * on failure, as the real one does. */
static inline DWORD FormatMessage(DWORD flags, const void *source, DWORD message_id,
                                  DWORD language_id, wchar_t *buffer, DWORD size,
                                  void *arguments)
{
	const wchar_t *s;
	DWORD i = 0;

	(void)flags; (void)source; (void)language_id; (void)arguments;
	if (buffer == NULL || size == 0)
		return 0;
	s = _wcserror((int)message_id);
	while (s[i] != L'\0' && i < size - 1) {
		buffer[i] = s[i];
		i++;
	}
	buffer[i] = L'\0';
	return i;
}

/*
 * MSVC stat spellings, used by FileGeneral for size and mtime lookups.
 * _stat64i32 is plain struct stat here — Linux has been 64-bit-clean on both
 * fields for long enough that MSVC's split into sized variants has no
 * counterpart to preserve.
 */
#include <sys/stat.h>
#include <unistd.h>
#define _stat64i32 stat
#define _stati64   stat

static inline int _wstat_impl(const wchar_t *path, struct stat *st)
{
	char buf[4096];
	if (path == NULL || WideCharToMultiByte(CP_UTF8, 0, path, -1, buf,
	                                        (int)sizeof(buf), 0, 0) == 0)
		return -1;
	return stat(buf, st);
}
#define _wstat(p, s)     _wstat_impl((p), (s))
#define _wstati64(p, s)  _wstat_impl((p), (s))

/* Milliseconds, matching the Win32 signature. Only reached by FileGeneral's
 * retry loop when a file is locked by another writer. */
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }

static inline int _wunlink(const wchar_t *path)
{
	char buf[4096];
	if (path == NULL || WideCharToMultiByte(CP_UTF8, 0, path, -1, buf,
	                                        (int)sizeof(buf), 0, 0) == 0)
		return -1;
	return unlink(buf);
}

/* MSVC path-component limits, used to size local buffers in FileGeneral. */
#define _MAX_PATH   260
#define _MAX_DRIVE    3
#define _MAX_DIR    256
#define _MAX_FNAME  256
#define _MAX_EXT    256

/* COM success code, compared against by pstr.cpp's GUID helper. */
#define S_OK 0L

/* Case-insensitive wide compare. glibc spells it wcscasecmp. */
#define _wcsicmp  wcscasecmp
#define _wcsnicmp wcsncasecmp

/* Broken-down local time, and a pointer to a FILETIME. Referenced by
 * pstrArray's timestamp helpers, which the DSP does not call. */
typedef struct {
	WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;
typedef FILETIME *LPFILETIME;

/* Orders two FILETIMEs: -1, 0, or 1. Same comparison the real one performs,
 * on the same 64-bit tick value split across the two DWORDs. */
static inline long CompareFileTime(const FILETIME *a, const FILETIME *b)
{
	unsigned long long x, y;
	if (a == NULL || b == NULL)
		return 0;
	x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
	y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static inline BOOL CreateDirectoryW(const wchar_t *path, LPSECURITY_ATTRIBUTES sa)
{
	char buf[4096];
	(void)sa;
	if (path == NULL || WideCharToMultiByte(CP_UTF8, 0, path, -1, buf,
	                                        (int)sizeof(buf), 0, 0) == 0)
		return 0;
	return mkdir(buf, 0777) == 0;
}

/* Bounds-checked printf variants. Their contract matches the ISO functions
 * argument for argument, so these are renames. */
#define swprintf_s  swprintf
#define wcscpy_s(d, n, s)  ((void)(n), wcscpy((d), (s)))
#define wcscat_s(d, n, s)  ((void)(n), wcscat((d), (s)))

/*
 * MSVC path splitter. Any of the four outputs may be NULL, meaning "not
 * wanted". The drive component is always empty on Linux, which is what
 * FileGeneral's callers do with it anyway — they use fname and ext.
 */
static inline void _wsplitpath(const wchar_t *path, wchar_t *drive, wchar_t *dir,
                               wchar_t *fname, wchar_t *ext)
{
	const wchar_t *slash, *dot, *base;

	if (drive) drive[0] = L'\0';
	if (dir)   dir[0]   = L'\0';
	if (fname) fname[0] = L'\0';
	if (ext)   ext[0]   = L'\0';
	if (path == NULL)
		return;

	slash = wcsrchr(path, L'/');
	base  = slash ? slash + 1 : path;

	if (dir && slash) {
		size_t n = (size_t)(slash - path) + 1;
		wmemcpy(dir, path, n);
		dir[n] = L'\0';
	}
	/* A leading dot is part of the name, not an extension separator. */
	dot = wcsrchr(base, L'.');
	if (dot == base)
		dot = NULL;
	if (fname) {
		size_t n = dot ? (size_t)(dot - base) : wcslen(base);
		wmemcpy(fname, base, n);
		fname[n] = L'\0';
	}
	if (ext && dot)
		wcscpy(ext, dot);
}

/* Win32's wsprintf is an unbounded sprintf. The wide call site in pstr.cpp
 * resolves through the swprintf compatibility overload, which reproduces that
 * unbounded behaviour deliberately — see pt_swprintf.h. */
#define wsprintf swprintf

/*
 * GUID generation. pstrGenerateGUID_Wide is never called from the DSP; it is
 * only here because pstr.cpp is compiled whole for one string helper. The
 * type has to exist and the function has to fail cleanly, nothing more —
 * inventing a random GUID would be implementing a feature the port does not
 * use and cannot test.
 */
typedef struct { DWORD Data1; WORD Data2, Data3; unsigned char Data4[8]; } GUID;
#ifdef __cplusplus
/* C++ only: the real StringFromGUID2 takes REFGUID, which is a reference in
 * C++ and a pointer in C. Only pstr.cpp calls it, and that is C++ — declaring
 * the reference form unconditionally is a syntax error in the fifteen C
 * files that reach this header. */
static inline long CoCreateGuid(GUID *g) { if (g) { GUID z = {0,0,0,{0}}; *g = z; } return -1; }
static inline int  StringFromGUID2(const GUID &g, wchar_t *out, int n)
{ (void)g; if (out && n > 0) out[0] = L'\0'; return 0; }
#endif

static inline HMODULE LoadLibrary(const wchar_t *p) { (void)p; return NULL; }
static inline void   *GetProcAddress(HMODULE m, const char *n) { (void)m; (void)n; return NULL; }
static inline int     FreeLibrary(HMODULE m) { (void)m; return 1; }

/*
 * _wfopen_s is the only file-open call in the entire DSP, in Valscfg.cpp —
 * which is the preset reader, so this sits directly on the path that loads
 * every .fac file. Wide paths convert to the locale encoding; UTF-8 on any
 * modern Fedora, so non-ASCII preset paths survive.
 */
static inline int _wfopen_s(FILE **fp, const wchar_t *path, const wchar_t *mode)
{
	char cp[4096], cm[16];
	if (fp == NULL)
		return 22;                      /* EINVAL, matching the CRT */
	*fp = NULL;
	if (wcstombs(cp, path, sizeof(cp)) == (size_t)-1)
		return 22;
	if (wcstombs(cm, mode, sizeof(cm)) == (size_t)-1)
		return 22;
	*fp = fopen(cp, cm);
	return (*fp == NULL) ? 2 : 0;       /* ENOENT, matching the CRT */
}

#ifdef __cplusplus
}
#endif

#endif /* SEMIBREVE_WIN32_SHIM_H_ */
