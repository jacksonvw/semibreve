/*
 * Semibreve — stand-in for <mmsystem.h>.
 *
 * U_pwav.h includes this for the WAVEFORMATEX/waveOut declarations used by
 * ptutil/PWAV, a WAV file utility that is not on the DSP path. Only the
 * types are provided; the waveOut functions are deliberately absent so any
 * real use fails at compile time.
 */
#ifndef SEMIBREVE_STUB_MMSYSTEM_H_
#define SEMIBREVE_STUB_MMSYSTEM_H_
#include <windows.h>
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef void *HWAVEOUT;
typedef void *HWAVEIN;

/*
 * Multimedia file I/O handle and chunk descriptor. PwavConvert.cpp is in the
 * build for one function, pwavFloatTo24Bit, which the DFX effects chain calls
 * through comProcessWaveBuffer. The rest of that file reads and writes WAV
 * files through the mmio* API; only the types are needed for it to compile,
 * and the functions are deliberately absent so any real use fails loudly.
 */
typedef void *HMMIO;
typedef struct {
	DWORD ckid, cksize, fccType, dwDataOffset, dwFlags;
} MMCKINFO;
typedef unsigned int MMRESULT;
typedef struct wavehdr_tag {
	LPSTR  lpData;
	DWORD  dwBufferLength, dwBytesRecorded;
	DWORD *dwUser;
	DWORD  dwFlags, dwLoops;
	struct wavehdr_tag *lpNext;
	DWORD *reserved;
} WAVEHDR, *LPWAVEHDR;
#endif
