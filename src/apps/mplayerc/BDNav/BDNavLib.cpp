/*
 * BDNavLib.cpp - runtime loader for bdnav.dll (MPC-BE BDMenu)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDNavLib.h"

static CString GetExeDir()
{
	WCHAR path[MAX_PATH] = {};
	::GetModuleFileNameW(nullptr, path, MAX_PATH);
	::PathRemoveFileSpecW(path);
	return CString(path);
}

CString BDNavGetExeDir()
{
	return GetExeDir();
}

CBDNavLib& CBDNavLib::Get()
{
	static CBDNavLib lib;
	return lib;
}

CBDNavLib::~CBDNavLib()
{
	Unload();
}

void CBDNavLib::Unload()
{
#define BDNAV_NULL_FN(n) n = nullptr;
	BDNAV_FUNCS(BDNAV_NULL_FN)
#undef BDNAV_NULL_FN
	if (m_hModule) {
		::FreeLibrary(m_hModule);
		m_hModule = nullptr;
	}
}

bool CBDNavLib::Load()
{
	if (m_hModule) {
		return true;
	}
	if (m_bTried) {
		return false;
	}
	m_bTried = true;

	const CString dll = GetExeDir() + L"\\bdnav.dll";
	m_hModule = ::LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m_hModule) {
		m_strError.Format(L"LoadLibrary(%s) failed, error %u", dll.GetString(), ::GetLastError());
		return false;
	}

#define BDNAV_LOAD_FN(n)                                                          \
	n = reinterpret_cast<decltype(n)>(::GetProcAddress(m_hModule, #n));           \
	if (!n) {                                                                     \
		m_strError = CString(L"bdnav.dll: missing export ") + CString(#n);        \
		Unload();                                                                 \
		return false;                                                             \
	}
	BDNAV_FUNCS(BDNAV_LOAD_FN)
#undef BDNAV_LOAD_FN

	const int ver = bdnav_api_version();
	if (ver != BDNAV_API_VERSION) {
		m_strError.Format(L"bdnav.dll API version mismatch (dll %d, exe %d)", ver, BDNAV_API_VERSION);
		Unload();
		return false;
	}

	return true;
}

CString BDNavUTF8ToW(const char* s)
{
	CString w;
	if (s && *s) {
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		if (n > 0) {
			::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.GetBuffer(n), n);
			w.ReleaseBuffer();
		}
	}
	return w;
}

CStringA BDNavWToUTF8(const CString& s)
{
	CStringA a;
	if (!s.IsEmpty()) {
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			::WideCharToMultiByte(CP_UTF8, 0, s, -1, a.GetBuffer(n), n, nullptr, nullptr);
			a.ReleaseBuffer();
		}
	}
	return a;
}

CString BDNavGetDiscRoot(const CString& path)
{
	CString root(path);
	root.TrimRight(L'\\');
	if (root.GetLength() >= 4 && root.Right(4).CompareNoCase(L".iso") == 0) {
		return root;
	}

	if (::PathIsDirectoryW(root)) {
		if (root.GetLength() >= 5 && root.Right(5).CompareNoCase(L"\\BDMV") == 0) {
			root.Truncate(root.GetLength() - 5);
		}
		if (root.GetLength() == 2 && root[1] == L':') {
			root += L'\\';
		}
		return root;
	}

	int pos = root.ReverseFind(L'\\');
	if (pos > 0) {
		root.Truncate(pos); // ...\BDMV
	}
	if (root.GetLength() >= 5 && root.Right(5).CompareNoCase(L"\\BDMV") == 0) {
		root.Truncate(root.GetLength() - 5);
	}
	if (root.GetLength() == 2 && root[1] == L':') {
		root += L'\\';
	}
	return root;
}

static CCritSec g_csLog;

CString BDNavGetDataDir()
{
	static CString dir;
	if (!dir.IsEmpty()) {
		return dir;
	}
	// exe folder (portable) if writable, otherwise %LOCALAPPDATA%\MPC-BE-BDMenu (installed in Program Files)
	const CString exeDir = GetExeDir();
	const CString test = exeDir + L"\\bdnav_write.tmp";
	HANDLE h = ::CreateFileW(test, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		::CloseHandle(h);
		dir = exeDir;
	} else {
		WCHAR path[MAX_PATH] = {};
		if (::GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) {
			dir = CString(path) + L"\\MPC-BE-BDMenu";
		} else {
			WCHAR tmp[MAX_PATH] = {};
			::GetTempPathW(MAX_PATH, tmp);
			dir = CString(tmp) + L"MPC-BE-BDMenu";
		}
		::CreateDirectoryW(dir, nullptr);
	}
	return dir;
}

bool BDNavIsVerbose()
{
	static int verbose = -1;
	if (verbose < 0) {
		verbose = (::PathFileExistsW(BDNavGetDataDir() + L"\\bdnav_debug.txt") || ::PathFileExistsW(GetExeDir() + L"\\bdnav_debug.txt")) ? 1 : 0;
	}
	return verbose == 1;
}

static CString GetLogFile()
{
	static bool rotated = false;
	const CString logfile = BDNavGetDataDir() + L"\\bdnav.log";
	if (!rotated) {
		rotated = true;
		WIN32_FILE_ATTRIBUTE_DATA fad = {};
		if (::GetFileAttributesExW(logfile, GetFileExInfoStandard, &fad)
				&& (((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) > 2 * 1024 * 1024) {
			const CString old = BDNavGetDataDir() + L"\\bdnav.old.log";
			::DeleteFileW(old);
			::MoveFileW(logfile, old);
		}
	}
	return logfile;
}

void BDNavLogRaw(const char* utf8, size_t len)
{
	if (!utf8 || !len) {
		return;
	}
	CAutoLock lock(&g_csLog);
	const CString logfile = GetLogFile();
	FILE* f = nullptr;
	if (_wfopen_s(&f, logfile, L"ab") == 0 && f) {
		fwrite(utf8, 1, len, f);
		fclose(f);
	}
}

void BDNavLog(const CString& line)
{
	CAutoLock lock(&g_csLog);
	const CString logfile = GetLogFile();
	FILE* f = nullptr;
	if (_wfopen_s(&f, logfile, L"ab") == 0 && f) {
		_fseeki64(f, 0, SEEK_END);
		if (_ftelli64(f) == 0) {
			fwrite("\xEF\xBB\xBF", 1, 3, f);
		}
		CString full;
		full.Format(L"[%s] %s\r\n", CTime::GetCurrentTime().Format(L"%Y-%m-%d %H:%M:%S").GetString(), line.GetString());
		const CStringA u8 = BDNavWToUTF8(full);
		fwrite(u8.GetString(), 1, u8.GetLength(), f);
		fclose(f);
	}
}

CString BDNavProbe(const CString& path)
{
	auto& lib = CBDNavLib::Get();
	CString msg;

	if (!lib.Load()) {
		msg.Format(L"BDNav: not loaded (%s)", lib.GetError().GetString());
		BDNavLog(msg);
		return msg;
	}

	const CString root = BDNavGetDiscRoot(path);
	const CStringA root8 = BDNavWToUTF8(root);
	const CString libver = BDNavUTF8ToW(lib.bdnav_lib_version());

	char err[2048] = {};
	const DWORD t0 = ::GetTickCount();
	BDNAV* nav = lib.bdnav_open(root8, err, sizeof(err));
	const DWORD dt = ::GetTickCount() - t0;

	if (!nav) {
		msg.Format(L"BDNav: %s | open FAILED (%u ms): %s", libver.GetString(), dt, root.GetString());
		BDNavLog(msg);
		BDNavLog(L"libbluray log: " + BDNavUTF8ToW(err));
		return msg;
	}

	BDNAV_DISC_INFO di = {};
	lib.bdnav_get_disc_info(nav, &di);
	lib.bdnav_close(nav);

	msg.Format(L"BDNav: %s | open OK (%u ms) | BD=%d FirstPlay=%d TopMenu=%d titles=%d HDMV=%d BDJ=%d AACS=%d/%d BD+=%d | \"%s\"",
		libver.GetString(), dt,
		di.bluray_detected, di.first_play_supported, di.top_menu_supported,
		di.num_titles, di.num_hdmv_titles, di.num_bdj_titles,
		di.aacs_detected, di.aacs_handled, di.bdplus_detected,
		BDNavUTF8ToW(di.disc_name).GetString());
	BDNavLog(msg + L" | " + root);

	char log[2048] = {};
	if (lib.bdnav_get_log(log, sizeof(log), 1) > 0) {
		BDNavLog(L"libbluray log: " + BDNavUTF8ToW(log));
	}
	return msg;
}
