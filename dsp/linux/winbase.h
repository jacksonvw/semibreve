/*
 * Semibreve — empty stand-in for the MSVC header <winbase.h>.
 *
 * Pulled in by the audiopassthru string and file utilities for declarations
 * they do not use on this build. Present so the #includes resolve without
 * editing upstream sources.
 */
#ifndef SEMIBREVE_STUB_WINBASE_H_
#define SEMIBREVE_STUB_WINBASE_H_
#include <windows.h>
#endif
