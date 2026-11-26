/*
 * BDNavLib.h - runtime loader for bdnav.dll (MPC-BE BDMenu)
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include "bdnav_api.h"

#define BDNAV_FUNCS(X)              \
	X(bdnav_api_version)            \
	X(bdnav_lib_version)            \
	X(bdnav_get_log)                \
	X(bdnav_set_debug)              \
	X(bdnav_open)                   \
	X(bdnav_close)                  \
	X(bdnav_get_disc_info)          \
	X(bdnav_set_player)             \
	X(bdnav_set_notify)             \
	X(bdnav_play)                   \
	X(bdnav_read)                   \
	X(bdnav_get_event)              \
	X(bdnav_user_input)             \
	X(bdnav_mouse_select)           \
	X(bdnav_menu_call)              \
	X(bdnav_set_scr)                \
	X(bdnav_tell_time)              \
	X(bdnav_seek_time)              \
	X(bdnav_seek_chapter)           \
	X(bdnav_get_current_chapter)    \
	X(bdnav_get_playlist_info)      \
	X(bdnav_get_clip_info)          \
	X(bdnav_get_chapters)           \
	X(bdnav_get_stream_pid)         \
	X(bdnav_read_file)              \
	X(bdnav_lock_plane)             \
	X(bdnav_unlock_plane)            \
	X(bdnav_open_ex)                \
	X(bdnav_read_ex)                \
	X(bdnav_get_title_kind)         \
	X(bdnav_bdj_started)             \
	X(bdnav_select_stream)

class CBDNavLib
{
public:
	static CBDNavLib& Get();

	bool Load();                       // idempotent; false = not available (fallback to original playback)
	bool IsLoaded() const { return m_hModule != nullptr; }
	const CString& GetError() const { return m_strError; }

#define BDNAV_DECL_FN(n) decltype(&::n) n = nullptr;
	BDNAV_FUNCS(BDNAV_DECL_FN)
#undef BDNAV_DECL_FN

private:
	CBDNavLib() = default;
	~CBDNavLib();
	void Unload();

	HMODULE m_hModule = nullptr;
	bool    m_bTried  = false;
	CString m_strError;
};

CString BDNavUTF8ToW(const char* s);
CStringA BDNavWToUTF8(const CString& s);
CString BDNavGetDiscRoot(const CString& path);   // .iso -> itself; ...\BDMV\index.bdmv -> disc root folder
void BDNavLog(const CString& line);              // appends to bdnav.log next to mpc-be64.exe
void BDNavLogRaw(const char* utf8, size_t len);   // appends raw UTF-8 text (libbluray log)
CString BDNavGetExeDir();                         // folder of mpc-be64.exe
CString BDNavGetDataDir();                        // exe folder if writable, else %LOCALAPPDATA%\MPC-BE-BDMenu
bool BDNavIsVerbose();                            // bdnav_debug.txt exists in the data folder

// diagnostics only (opens the disc with libbluray, logs disc info, closes it)
CString BDNavProbe(const CString& path);
