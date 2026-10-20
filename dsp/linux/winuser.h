/*
 * Semibreve — stand-in for <winuser.h>.
 *
 * Comsftwr.c includes it alongside <windows.h> for MessageBox, which the
 * Linux build never reaches (that code path is the unreferenced demo
 * limiter). Folds into the type shim.
 */
#ifndef SEMIBREVE_STUB_WINUSER_H_
#define SEMIBREVE_STUB_WINUSER_H_
#include <windows.h>
#endif
