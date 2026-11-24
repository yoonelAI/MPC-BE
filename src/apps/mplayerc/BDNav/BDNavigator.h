/*
 * BDNavigator.h - Blu-ray navigation session (libbluray via bdnav.dll) for MPC-BE BDMenu
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include <vector>
#include <list>
#include <atomic>
#include "BDNavLib.h"

// posted to the main window when the menu plane changed (coalesced)
#define WM_BDNAV_NOTIFY (WM_APP + 0x0B0D)

struct SubPicDesc;

class CBDNavigator
{
public:
	static CBDNavigator& Get();

	// main thread
	bool Open(const CString& path, HWND hNotify, CString& err);
	void Close();
	bool IsOpen();
	const CString& GetMarkerPath() const { return m_strMarker; }
	const CString& GetDiscRoot() const   { return m_strRoot; }
	const CString& GetLabel() const      { return m_strLabel; }
	void NotifyHandled() { m_bNotifyPending = false; }

	// source filter (graph / reader thread)
	bool StartPlayback();
	int  ReadTS(BYTE* out, int outcap, bool& bEvent);   // 192-byte BD-TS packets; <0 = end
	bool TakeFlushRequest() { return m_bFlushRequest.exchange(false); }
	// writes BDMV\CLIPINF\<clip>.clpi of the clip being played into the temp folder and
	// returns the matching virtual ...\BDMV\STREAM\<clip>.m2ts path (lets the MPEG splitter read clip info)
	CString PrepareClipInfoPath();

	// user input (main thread). Coordinates are relative to the video rectangle.
	bool IsMenuVisible();                 // libbluray IG plane has visible content
	void SetOverlayOn(bool on) { m_bOverlayOn = on; }
	bool IsOverlayOn() const { return m_bOverlayOn; }
	bool Mouse(int x, int y, int vw, int vh, bool bClick);
	void Key(uint32_t key);
	void MenuCall();   // top menu
	void Popup();      // pop-up menu (falls back to top menu if no pop-up)
	void FlushLibLog();

	// subpicture rendering (subpic queue thread)
	bool GetPlaneSize(SIZE& sz);
	bool RenderMenu(SubPicDesc& spd, RECT& bbox);

private:
	CBDNavigator() = default;
	~CBDNavigator();

	static void OnNotifyStatic(void* ctx, int plane, int reason);
	void OnNotify(int plane, int reason);
	void HandleEvent(const BDNAV_EVENT& ev);
	void CheckAutoMenu();
	void PostNotify();
	bool WriteMarker(CString& err);
	int  DetectRegion();
	void UserAction() { m_dwUserActionTick = ::GetTickCount(); }

	CCritSec   m_csNav;       // guards m_nav and all bdnav_* calls
	CCritSec   m_csPlane;     // guards the plane copy
	BDNAV*     m_nav = nullptr;
	HWND       m_hNotify = nullptr;
	std::atomic<bool> m_bNotifyPending{ false };
	std::atomic<bool> m_bFlushRequest{ false };
	std::atomic<bool> m_bOverlayOn{ false };

	CString    m_strRoot;
	CString    m_strLabel;
	CString    m_strMarker;
	CString    m_strTempDir;
	std::list<CString> m_tempFiles;
	bool       m_bPlaying = false;

	// IG plane copy (last non-empty content)
	std::vector<uint32_t> m_plane;
	int        m_planeW = 0, m_planeH = 0;
	RECT       m_planeBox = {};
	bool       m_planeVisible = false;
	uint32_t   m_planeVersion = 0;

	// navigation state (under m_csNav)
	unsigned long long m_total = 0;
	int        m_title = -1;
	int        m_playlist = -1;
	int        m_playitem = 0;
	uint64_t   m_playlistDur = 0;   // 90 kHz
	DWORD      m_titleTick = 0;
	unsigned long long m_titleBytes = 0;
	int        m_popupAvail = 0;
	bool       m_bMenuSeen = false;
	bool       m_bAutoMenuDone = false;
	int        m_lastMouseX = -1, m_lastMouseY = -1;
	int        m_logBudget = 3000;
	size_t     m_libLogBudget = 4 * 1024 * 1024;
	DWORD      m_dwLibLogTick = 0;
	DWORD      m_dwUserActionTick = 0;
	bool       m_bAlignWarned = false;
};
