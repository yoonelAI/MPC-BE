/*
 * BDNavigator.h - Blu-ray navigation session (libbluray via bdnav.dll) for MPC-BE BDMenu
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include <vector>
#include <list>
#include <deque>
#include <map>
#include <atomic>
#include "BDNavLib.h"

// posted to the main window when the menu plane changed (coalesced)
#define WM_BDNAV_NOTIFY (WM_APP + 0x0B0D)

struct SubPicDesc;

// In navigation mode the video renderer gets a synchronous subpicture queue (no pre-rendering):
// the menu is drawn for exactly the frame that is presented -> no gaps after jumps, no lag.
extern bool g_bBDNavSubpicNoQueue;   // set by CMainFrame before the graph is built
extern bool g_bBDNavNoQueueApplied;  // set when a renderer was created with that setting
class CBDNavReader;

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

	struct Marker {
		uint64_t title90k;   // position in the title (playlist), 90 kHz
		uint64_t dur90k;     // playlist duration, 90 kHz
		int      playlist;
		uint32_t seq;        // marker sequence number
		uint32_t flags;      // kMark* (title kind when the data was read)
	};
	static const uint32_t kMarkInteractive = 0x01;   // interactive title: no seek bar
	static const uint32_t kMarkBDJ         = 0x02;
	static const uint32_t kMarkTopMenu     = 0x04;

	// source filter (graph / reader thread)
	bool StartPlayback();
	// 192-byte BD-TS packets; <0 = end. bFlush / markers apply to the position BEFORE the returned data.
	int  ReadTS(BYTE* out, int outcap, bool& bEvent, bool& bFlush, std::vector<Marker>& markers);
	void SetReader(CBDNavReader* pReader);
	void AddAnchor(REFERENCE_TIME rtStream, const Marker& m);

	// seek bar (main thread)
	REFERENCE_TIME GetStreamTime();
	bool GetTitlePos(REFERENCE_TIME& pos, REFERENCE_TIME& dur);
	bool SeekTitle(REFERENCE_TIME pos);

	// chapters of the current playlist (title time); returns true if changed since 'gen'
	bool GetChapters(std::vector<REFERENCE_TIME>& chapters, uint32_t& gen);
	// audio stream selected by the disc (PID), consumed by the main thread; -1 = none
	int  TakeAudioPid() { return m_audioPid.exchange(-1); }
	// streams selected by the disc for the clip being played (PIDs), -1 = none / unknown
	int  GetDiscAudioPid();
	bool TakeSubtitleChange() { return m_bPgDirty.exchange(false); }
	int  GetDiscSubtitlePid(bool& bEnabled);
	void ClearReader(CBDNavReader* pReader);
	bool IsSegmentPlayedOut();
	int  SegmentRemainingMs();
	// navigation failed right at the start -> main thread reopens with the original playback
	static const WPARAM kNotifyFallback = 1;
	// the stream format changes (other video codec/resolution or new audio streams): the current
	// DirectShow graph plays out, then the main thread rebuilds it while the navigation keeps going
	static const WPARAM kNotifyRebuild = 2;
	static const int    kReadSegmentEnd = -2;
	static const WPARAM kNotifyDrained = 3;   // the last graph's data was read completely
	void OnSegmentDrained() { if (m_hNotify) ::PostMessageW(m_hNotify, WM_BDNAV_NOTIFY, kNotifyDrained, 0); }
	bool IsRebuildPending() const { return m_bRebuildPending; }
	void BeginSegment();   // CBDNavReader::Load, before reading
	// writes BDMV\CLIPINF\<clip>.clpi of the clip being played into the temp folder and
	// returns the matching virtual ...\BDMV\STREAM\<clip>.m2ts path (lets the MPEG splitter read clip info)
	CString PrepareClipInfoPath();

	// BD-J
	bool IsBDJDisc() const { return m_bBDJDisc; }
	bool IsBDJTitle() const { return m_bBDJTitle; }
	DWORD GetStartWaitMs() const { return m_bBDJDisc ? 25000 : 3000; }   // first data for the graph (BD-J: JVM + Xlet start)
	// the menu changed but no presented frame has shown it yet (still picture / idle: no new video frames)
	bool IsRenderPending();

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
	void UserAction() { m_dwUserActionTick = ::GetTickCount(); m_bUserJumpPending = true; CancelSeekPending(); }
	void CancelSeekPending() { CAutoLock lock(&m_csAnchor); m_bSeekPending = false; }

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

	// IG plane copy (last non-empty content) + converted copy for the subpicture
	// (premultiplied, inverted alpha: ready to be copied row by row)
	std::vector<uint32_t> m_planeConv;
	std::vector<uint32_t> m_plane;
	int        m_planeW = 0, m_planeH = 0;
	RECT       m_planeBox = {};
	bool       m_planeVisible = false;
	bool       m_bPlaneSynced = false;   // m_plane/m_planeConv match the dll plane (partial updates allowed)
	bool       m_bBDJDisc = false;
	std::atomic<bool> m_bBDJTitle{ false };
	int        m_titleFlags = 0;
	int        m_keyInterest = -1;
	unsigned long long m_flushTotal = 0;   // m_total when the last user-jump flush was requested
	bool       m_bNaturalEnd = false;      // END_OF_TITLE before the next jump: the disc moved on by itself
	uint32_t   m_lastEvent = 0;            // suppress repeated END_OF_TITLE in the log
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
	bool       m_bUserJumpPending = false;
	// graph segments (format changes)
	bool       m_bSegmentEnd = false;            // under m_csNav
	std::atomic<bool> m_bRebuildPending{ false };
	DWORD      m_dwLastFlushTick = 0;
	bool       m_bGraphSig = false;
	int        m_graphPG = 0;                       // PG streams of the clip the graph was built for
	bool       CheckFormatChange(CString& extra);   // m_csNav held; true = rebuild needed
	int        m_pgNumber = 0;                      // PG stream number selected by the disc (1..N, 0 = none)
	bool       m_pgEnabled = false;
	std::atomic<bool> m_bPgDirty{ false };
	CString    m_graphSigV, m_graphSigA;         // format the current graph was built for
	std::vector<BYTE> m_carry;                   // data of the new clip read together with the boundary event
	bool ClipSig(int playlist, int item, CString& v, CString& a);
	bool       m_bVerbose = false;
	// diagnostics: bdnav_dump.txt in the data folder -> the TS data of each graph segment is written to dump\seg_NN.m2ts
	FILE*      m_dumpFile = nullptr;
	int        m_dumpSeg = 0;
	unsigned long long m_dumpBytes = 0;
	void       DumpData(const BYTE* p, int n);
	void       DumpNewSegment();
	DWORD      m_dwPlayTick = 0;
	bool       m_bFallbackPosted = false;
	std::atomic<int> m_audioPid{ -1 };
	int        m_audioNumber = 0;
	std::vector<REFERENCE_TIME> m_chapters;   // under m_csAnchor: chapters of the playlist that is playing now
	uint32_t   m_chaptersGen = 0;
	int        m_chaptersPlaylist = -1;       // playlist m_chapters belong to
	std::map<int, std::vector<REFERENCE_TIME>> m_chapterCache;   // playlist -> chapters (read ahead)
	DWORD      m_dwJumpArmTick = 0;           // user title jump: also flush at the first new playlist (m_csNav)
	void UpdateChapters();                    // m_csNav held

	// crash protection: a disc that crashed the player is played the original way next time
	CString    m_strLockFile;
	static bool IsBlocked(const CString& root);
	static void CheckCrashLock();
	std::vector<Marker> m_pendingMarkers;   // under m_csNav
	void QueueMarker();

	CCritSec   m_csReader;
	CBDNavReader* m_pReader = nullptr;

	struct Anchor {
		REFERENCE_TIME rt;      // stream time
		REFERENCE_TIME title;   // title time at rt
		REFERENCE_TIME dur;
		int            playlist;
		uint32_t       seq;
		uint32_t       flags;
	};
	uint32_t   m_markerSeq = 0;            // under m_csNav
	// user seek in progress: show the target position until its data plays
	bool           m_bSeekPending = false;  // under m_csAnchor
	uint32_t       m_seekSeq = 0;
	REFERENCE_TIME m_seekPos = 0;
	DWORD          m_seekTick = 0;
	CCritSec   m_csAnchor;
	std::deque<Anchor> m_anchors;

	// UI thread waiting for m_csNav: the reader thread yields (no starvation of mouse/keys)
	std::atomic<int> m_uiWaiting{ 0 };
	struct CUILock {
		CBDNavigator& n;
		CUILock(CBDNavigator& nav) : n(nav) { n.m_uiWaiting++; n.m_csNav.Lock(); }
		~CUILock() { n.m_csNav.Unlock(); n.m_uiWaiting--; }
	};

	// latency trace
	DWORD      m_dwInputTick = 0;
	DWORD      m_dwPlaneTick = 0;
	uint32_t   m_lastRenderedVersion = 0;
	int        m_traceBudget = 300;
};
