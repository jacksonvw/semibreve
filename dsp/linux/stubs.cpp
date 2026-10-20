/*
 * Semibreve — stubs for the two symbol families with no Linux counterpart.
 *
 * Upstream builds DfxDsp as a static library, so its linker only ever pulls
 * the object files something actually references. That lets undefined symbols
 * sit in the archive indefinitely. A shared library has the same latitude at
 * link time but not at run time, where a call into a missing symbol is a
 * crash rather than a link error. These definitions close that gap.
 *
 * 1. The registry — an in-memory store, not a stub
 *
 *    regCreateKey_Wide / regReadKey_Wide are implemented in audiopassthru
 *    against the Win32 registry. The first attempt here made reads report
 *    "absent" and writes succeed silently, on the reasoning that the registry
 *    was only used for session persistence and Semibreve keeps its
 *    configuration in TOML instead.
 *
 *    That was wrong, and quietly so. The registry is not only a persistence
 *    layer for this DSP — it is also an internal channel. eqUpdateFromRegistry
 *    reads each EQ band back out and pushes any difference into the
 *    processing chain, so a write-only registry means the DSP compares live
 *    values against a permanent zero and can flatten the EQ it just loaded.
 *
 *    So the store is real: an in-process map, faithful to the read-back
 *    contract, with no disk anywhere. The DSP's own state machine behaves
 *    exactly as it does on Windows, while nothing survives the process — our
 *    configuration still lives in TOML and is pushed in at startup, so there
 *    is still only one source of truth across runs.
 *
 *    Off the audio path by construction: these are reached from preset loads,
 *    parameter sets, and processTimer(), never from processAudio.
 *
 * 2. The DSP hardware card
 *
 *    comHrdEepromReadUlong, comHrdEepromWriteUlong and comHrdwrGetChar belong
 *    to the driver for Power Technology's 1990s outboard DSP card — the one
 *    whose handle struct still tracks a dongle and a serial number. They are
 *    declared in the headers and referenced from ptutil/COM, but defined
 *    nowhere in the repository at all, which is the clearest possible
 *    statement that no supported build reaches them.
 *
 *    comProcessWaveBuffer was briefly stubbed here alongside them, on the
 *    same reasoning, and that was wrong: it lives in ptutil/COM/Comwave.cpp
 *    and it is how dfxpModifyRealtypeSamples runs the entire DFX effects
 *    chain. Sharing a prefix with the card driver is all it has in common
 *    with these three.
 *
 *    They cannot simply be dropped, because dfxpComm.cpp still calls comInit
 *    during startup, passing a settings struct upstream itself named
 *    `obsolete`. So they are defined here, and they fail. NOT_OKAY is what
 *    the card layer already returns when no card is present, so upstream's
 *    own no-hardware path carries us the rest of the way.
 *
 *    If one of these is ever reached in anger it means a genuine hardware
 *    path has become live, which would be a porting decision to take
 *    deliberately rather than something to discover through a wrong answer.
 *
 * 3. fileGetModifiedDate_Wide — a real implementation, not a stub
 *
 *    Its upstream body opens a Win32 file handle and calls GetFileTime.
 *    Supporting that would mean shimming CreateFileW, CloseHandle and the
 *    whole handle family for one timestamp, so it is reimplemented here on
 *    stat() instead. Its only caller is a sort-by-date helper in pstrArray
 *    that the DSP never invokes, but a wrong timestamp is the kind of thing
 *    that surfaces much later and confusingly, so it returns the true value.
 */

#include "codedefs.h"
#include "pt_defs.h"
#include "slout.h"

#include <map>
#include <mutex>
#include <string>

/*
 * The upstream headers are included rather than the prototypes retyped, so
 * the compiler verifies each signature against the declaration the callers
 * actually see. Transcribing them by hand produced four silent mismatches:
 * definitions that compiled and linked while the real references stayed
 * undefined, because a shared library does not complain about either.
 */
#include "reg.h"
/* The card headers go inside extern "C", exactly as Comread.cpp includes
 * them, so the declarations here carry the same linkage as the references.
 * hrdwr.h comes first because it defines COMHRDWR_DECL, which hutil.h uses
 * without including it. */
extern "C" {
#include "hrdwr.h"
#include "hutil.h"
}
#include "c_dsps.h"

/*
 * Not extern "C". These are declared without a linkage specification, so
 * every reference from the DSP is C++-mangled and the definitions must be
 * too.
 */

/* ---- Registry ---------------------------------------------------------- */

namespace {

/* Keyed by hive plus path, mirroring how the Win32 calls are addressed. */
std::map<std::wstring, std::wstring> &reg_store()
{
	static std::map<std::wstring, std::wstring> store;
	return store;
}

std::mutex &reg_lock()
{
	static std::mutex m;
	return m;
}

std::wstring reg_key(int key_class, const wchar_t *name)
{
	return std::to_wstring(key_class) + L"\\" + name;
}

} /* namespace */

int PT_DECLSPEC regCreateKey_Wide(int i_key_class, wchar_t *wcp_keyname,
                                  wchar_t *wcp_data)
{
	if (wcp_keyname == NULL)
		return NOT_OKAY_NO_BREAK;

	std::lock_guard<std::mutex> guard(reg_lock());
	reg_store()[reg_key(i_key_class, wcp_keyname)] =
		(wcp_data != NULL) ? wcp_data : L"";
	return OKAY;
}

/*
 * ul_data_size is the caller's buffer capacity. Upstream forwards it to
 * RegQueryValueExW, which reads it as a byte count, but every caller in this
 * library passes the wchar_t length of a wchar_t array — so it is treated as
 * a wchar_t capacity here. That is the interpretation that matches the actual
 * buffers; taking it as bytes would halve them for no reason.
 */
int PT_DECLSPEC regReadKey_Wide(int i_key_class, wchar_t *wcp_keyname,
                                int *ip_key_exists_flag, wchar_t *wcp_data,
                                unsigned long ul_data_size)
{
	if (ip_key_exists_flag != NULL)
		*ip_key_exists_flag = IS_FALSE;
	if (wcp_keyname == NULL)
		return NOT_OKAY_NO_BREAK;
	if (wcp_data != NULL && ul_data_size > 0)
		wcp_data[0] = L'\0';

	std::lock_guard<std::mutex> guard(reg_lock());
	std::map<std::wstring, std::wstring> &store = reg_store();
	std::map<std::wstring, std::wstring>::const_iterator it =
		store.find(reg_key(i_key_class, wcp_keyname));

	/* Absent is a normal answer, not an error — upstream returns OKAY with
	 * the flag clear and lets the caller fall back to its default. */
	if (it == store.end())
		return OKAY;

	if (wcp_data != NULL && ul_data_size > 0) {
		size_t n = it->second.size();
		if (n > (size_t)ul_data_size - 1)
			n = (size_t)ul_data_size - 1;
		wmemcpy(wcp_data, it->second.c_str(), n);
		wcp_data[n] = L'\0';
	}
	if (ip_key_exists_flag != NULL)
		*ip_key_exists_flag = IS_TRUE;
	return OKAY;
}

/* ---- Outboard DSP card ------------------------------------------------- */

/*
 * These three take C linkage, unlike the registry pair above. Comread.cpp
 * wraps `#include "hrdwr.h"` and `#include "hutil.h"` in extern "C", so its
 * references are unmangled, while nothing wraps reg.h. The split is upstream's
 * and there is no single answer to copy — each family has to match the way its
 * own callers declare it.
 *
 * comProcessWaveBuffer stays outside this block: it is declared through
 * c_dsps.h in some translation units and referenced C++-mangled.
 */
extern "C" {

int COMHRDWR_DECL comHrdEepromReadUlong(unsigned short board_address,
                                        unsigned short address,
                                        unsigned long *ulpdata)
{
	(void)board_address; (void)address;
	if (ulpdata != NULL)
		*ulpdata = 0;
	return NOT_OKAY_NO_BREAK;
}

int COMHRDWR_DECL comHrdEepromWriteUlong(unsigned short board_address,
                                         unsigned short address,
                                         unsigned long uldata)
{
	(void)board_address; (void)address; (void)uldata;
	return NOT_OKAY_NO_BREAK;
}

int COMHRDWR_DECL comHrdwrGetChar(int i_address, long *lp_val)
{
	(void)i_address;
	if (lp_val != NULL)
		*lp_val = 0;
	return NOT_OKAY_NO_BREAK;
}

} /* extern "C" */

/* ---- File modification time -------------------------------------------- */

int PT_DECLSPEC fileGetModifiedDate_Wide(wchar_t *wcp_fullpath,
                                         FILETIME *ftp_modified_date,
                                         CSlout *hp_slout)
{
	/* FILETIME counts 100ns ticks from 1601-01-01; the Unix epoch sits this
	 * many ticks later. */
	const unsigned long long EPOCH_DELTA = 116444736000000000ULL;
	char path[4096];
	struct stat st;
	unsigned long long ticks;

	(void)hp_slout;
	if (wcp_fullpath == NULL || ftp_modified_date == NULL)
		return NOT_OKAY_NO_BREAK;
	if (WideCharToMultiByte(CP_UTF8, 0, wcp_fullpath, -1, path,
	                        (int)sizeof(path), 0, 0) == 0)
		return NOT_OKAY_NO_BREAK;
	if (stat(path, &st) != 0)
		return NOT_OKAY_NO_BREAK;

	ticks = (unsigned long long)st.st_mtime * 10000000ULL + EPOCH_DELTA;
	ftp_modified_date->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
	ftp_modified_date->dwHighDateTime = (DWORD)(ticks >> 32);
	return OKAY;
}

