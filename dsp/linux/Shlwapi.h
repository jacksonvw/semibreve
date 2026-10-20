/*
 * Semibreve — stand-in for <Shlwapi.h>.
 *
 * FileGeneral.cpp uses exactly one function from it, PathFileExistsW, to
 * implement fileExist_Wide. Mapped onto access(F_OK) over the UTF-8 form of
 * the path.
 */
#ifndef SEMIBREVE_STUB_SHLWAPI_H_
#define SEMIBREVE_STUB_SHLWAPI_H_

#include <windows.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline BOOL PathFileExistsW(const wchar_t *path)
{
	char buf[4096];
	if (path == NULL)
		return 0;
	if (WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, (int)sizeof(buf), 0, 0) == 0)
		return 0;
	return access(buf, F_OK) == 0;
}

#ifdef __cplusplus
}
#endif

#endif
