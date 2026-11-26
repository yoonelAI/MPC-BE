/*
 * BDNavigator.cpp - Blu-ray navigation session (libbluray via bdnav.dll) for MPC-BE BDMenu
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDNavigator.h"
#include "SubPic/ISubPic.h"
#include "BDNavReader.h"

static const uint64_t kLongTitle90k   = 10ull * 60 * 90000;   // >= 10 min: treat as main feature
static const DWORD    kAutoMenuDelay  = 2500;                 // ms of playback before auto menu
static const unsigned long long kAutoMenuBytes = 8ull * 1024 * 1024;
static const DWORD    kUserJumpWindow = 15000;                // ms after user input: jumps flush the buffer

bool g_bBDNavSubpicNoQueue  = false;
bool g_bBDNavNoQueueApplied = false;

CBDNavigator& CBDNavigator::Get()
{
	static CBDNavigator nav;
	return nav;
}

CBDNavigator::~CBDNavigator()
{
	// process exit: the dll may already be unloading, don't call into it
	m_nav = nullptr;
	if (!m_strLockFile.IsEmpty()) {
		::DeleteFileW(m_strLockFile);   // normal exit is not a crash
	}
}

static CString LockFilePath()  { return BDNavGetDataDir() + L"\\bdnav_session.lock"; }
static CString BlockFilePath() { return BDNavGetDataDir() + L"\\bdnav_blocklist.txt"; }

static CString ReadSmallFile(const CString& path)
{
	CString s;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path, L"rb") == 0 && f) {
		char buf[16384] = {};
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		s = BDNavUTF8ToW(buf[0] == '\xEF' ? buf + 3 : buf);
	}
	return s;
}

void CBDNavigator::CheckCrashLock()
{
	// a lock file left behind means the player did not exit normally while a disc was in navigation mode
	const CString lock = LockFilePath();
	if (!::PathFileExistsW(lock)) {
		return;
	}
	CString root = ReadSmallFile(lock);
	root.Trim();
	::DeleteFileW(lock);
	if (root.IsEmpty() || IsBlocked(root)) {
		return;
	}
	FILE* f = nullptr;
	if (_wfopen_s(&f, BlockFilePath(), L"ab") == 0 && f) {
		const CStringA line = BDNavWToUTF8(root) + "\r\n";
		fwrite(line.GetString(), 1, line.GetLength(), f);
		fclose(f);
	}
	BDNavLog(L"previous session did not end normally -> menus disabled for: " + root);
}

bool CBDNavigator::IsBlocked(const CString& root)
{
	const CString list = ReadSmallFile(BlockFilePath());
	int pos = 0;
	CString line = list.Tokenize(L"\r\n", pos);
	while (!line.IsEmpty()) {
		line.Trim();
		if (!line.IsEmpty() && line.CompareNoCase(root) == 0) {
			return true;
		}
		line = list.Tokenize(L"\r\n", pos);
	}
	return false;
}

static CString SanitizeFileName(CString s)
{
	static const WCHAR bad[] = L"\\/:*?\"<>|";
	for (int i = 0; i < s.GetLength(); i++) {
		if (s[i] < 32 || wcschr(bad, s[i])) {
			s.SetAt(i, L'_');
		}
	}
	s.Trim();
	s.TrimRight(L'.');
	if (s.GetLength() > 120) {
		s.Truncate(120);
	}
	return s;
}

static const WCHAR* EventName(uint32_t e)
{
	switch (e) {
	case BDNAV_EV_ERROR: return L"ERROR";
	case BDNAV_EV_READ_ERROR: return L"READ_ERROR";
	case BDNAV_EV_ENCRYPTED: return L"ENCRYPTED";
	case BDNAV_EV_ANGLE: return L"ANGLE";
	case BDNAV_EV_TITLE: return L"TITLE";
	case BDNAV_EV_PLAYLIST: return L"PLAYLIST";
	case BDNAV_EV_PLAYITEM: return L"PLAYITEM";
	case BDNAV_EV_CHAPTER: return L"CHAPTER";
	case BDNAV_EV_PLAYMARK: return L"PLAYMARK";
	case BDNAV_EV_END_OF_TITLE: return L"END_OF_TITLE";
	case BDNAV_EV_AUDIO_STREAM: return L"AUDIO_STREAM";
	case BDNAV_EV_IG_STREAM: return L"IG_STREAM";
	case BDNAV_EV_PG_TEXTST_STREAM: return L"PG_TEXTST_STREAM";
	case BDNAV_EV_PIP_PG_TEXTST_STREAM: return L"PIP_PG_TEXTST_STREAM";
	case BDNAV_EV_SECONDARY_AUDIO_STREAM: return L"SECONDARY_AUDIO_STREAM";
	case BDNAV_EV_SECONDARY_VIDEO_STREAM: return L"SECONDARY_VIDEO_STREAM";
	case BDNAV_EV_PG_TEXTST: return L"PG_TEXTST";
	case BDNAV_EV_PIP_PG_TEXTST: return L"PIP_PG_TEXTST";
	case BDNAV_EV_SECONDARY_AUDIO: return L"SECONDARY_AUDIO";
	case BDNAV_EV_SECONDARY_VIDEO: return L"SECONDARY_VIDEO";
	case BDNAV_EV_SECONDARY_VIDEO_SIZE: return L"SECONDARY_VIDEO_SIZE";
	case BDNAV_EV_PLAYLIST_STOP: return L"PLAYLIST_STOP";
	case BDNAV_EV_DISCONTINUITY: return L"DISCONTINUITY";
	case BDNAV_EV_SEEK: return L"SEEK";
	case BDNAV_EV_STILL: return L"STILL";
	case BDNAV_EV_STILL_TIME: return L"STILL_TIME";
	case BDNAV_EV_SOUND_EFFECT: return L"SOUND_EFFECT";
	case BDNAV_EV_IDLE: return L"IDLE";
	case BDNAV_EV_POPUP: return L"POPUP";
	case BDNAV_EV_MENU: return L"MENU";
	case BDNAV_EV_STEREOSCOPIC_STATUS: return L"STEREOSCOPIC_STATUS";
	case BDNAV_EV_KEY_INTEREST_TABLE: return L"KEY_INTEREST_TABLE";
	case BDNAV_EV_UO_MASK_CHANGED: return L"UO_MASK_CHANGED";
	default: return L"?";
	}
}

bool CBDNavigator::Open(const CString& path, HWND hNotify, CString& err)
{
	Close();

	auto& lib = CBDNavLib::Get();
	if (!lib.Load()) {
		err = lib.GetError();
		return false;
	}

	m_bVerbose = BDNavIsVerbose();
	lib.bdnav_set_debug(m_bVerbose ? 2 : 0);

	const CString root = BDNavGetDiscRoot(path);
	CheckCrashLock();
	if (IsBlocked(root)) {
		err = L"disabled for this disc after a crash (delete bdnav_blocklist.txt to retry)";
		return false;
	}
	char buf[2048] = {};
	// BD-J: bundled JRE + libbluray jar next to the exe (bdj\jre, bdj\libbluray-j2se-*.jar)
	BDNAV_CONFIG cfg = {};
	cfg.size = sizeof(cfg);
	const CString bdjDir = BDNavGetExeDir() + L"\\bdj";
	const CString jre = bdjDir + L"\\jre";
	const CString jar = bdjDir + L"\\libbluray-j2se-1.5.1.jar";
	const bool bHaveJava = ::PathFileExistsW(jre + L"\\bin\\server\\jvm.dll") && ::PathFileExistsW(jar);
	const CStringA jre8 = BDNavWToUTF8(jre), jar8 = BDNavWToUTF8(jar), store8 = BDNavWToUTF8(BDNavGetDataDir() + L"\\bdj_storage");
	if (bHaveJava) {
		cfg.java_home = jre8;
		cfg.bdj_jar = jar8;
		cfg.storage_dir = store8;
	}
	cfg.menu_lang = "chi";
	cfg.pg_lang = "chi";
	BDNAV* nav = lib.bdnav_open_ex(BDNavWToUTF8(root), &cfg, buf, sizeof(buf));
	if (!nav) {
		err = L"libbluray cannot open the disc: " + BDNavUTF8ToW(buf);
		return false;
	}

	BDNAV_DISC_INFO di = {};
	lib.bdnav_get_disc_info(nav, &di);

	CString reason;
	if (!di.bluray_detected) {
		reason = L"not a Blu-ray disc";
	} else if (di.aacs_detected && !di.aacs_handled) {
		reason = L"AACS encrypted";
	} else if (di.bdplus_detected && !di.bdplus_handled) {
		reason = L"BD+ protected";
	} else if (di.no_menu_support || (!di.first_play_supported && !di.top_menu_supported)) {
		reason = L"no HDMV navigation";
	} else if ((di.num_bdj_titles > 0 || di.bdj_detected) && !bHaveJava) {
		reason = L"BD-J disc, bundled Java missing (bdj\\jre, bdj\\libbluray-j2se-1.5.1.jar)";
	} else if ((di.num_bdj_titles > 0 || di.bdj_detected) && !di.bdj_handled) {
		reason = L"BD-J disc, Java not usable";
	}
	if (!reason.IsEmpty()) {
		lib.bdnav_close(nav);
		err = reason;
		return false;
	}

	{
		CAutoLock lock(&m_csNav);
		m_nav = nav;
		m_hNotify = hNotify;
		m_strRoot = root;
		m_strLabel = BDNavUTF8ToW(di.disc_name);
		if (m_strLabel.IsEmpty()) {
			m_strLabel = BDNavUTF8ToW(di.udf_volume_id);
		}
		if (m_strLabel.IsEmpty()) {
			CString t(root);
			t.TrimRight(L'\\');
			m_strLabel = t.Mid(t.ReverseFind(L'\\') + 1);
			if (m_strLabel.GetLength() > 4 && m_strLabel.Right(4).CompareNoCase(L".iso") == 0) {
				m_strLabel.Truncate(m_strLabel.GetLength() - 4);
			}
		}
		m_bPlaying = false;
		m_total = 0;
		m_title = -1;
		m_playlist = -1;
		m_playitem = 0;
		m_playlistDur = 0;
		m_titleTick = 0;
		m_titleBytes = 0;
		m_popupAvail = 0;
		m_bMenuSeen = false;
		m_bAutoMenuDone = false;
		m_lastMouseX = m_lastMouseY = -1;
		m_logBudget = 3000;
		m_libLogBudget = 4 * 1024 * 1024;
		m_dwLibLogTick = ::GetTickCount();
		m_dwUserActionTick = 0;
		m_bAlignWarned = false;
		m_bUserJumpPending = false;
		m_pendingMarkers.clear();
		m_bFallbackPosted = false;
		m_audioNumber = 0;
		m_bSegmentEnd = false;
		m_bGraphSig = false;
		m_carry.clear();
		m_pgNumber = 0;
		m_pgEnabled = false;
		m_bBDJDisc = di.num_bdj_titles > 0 || di.bdj_detected;
		m_bBDJTitle = false;
		m_titleFlags = 0;
		m_keyInterest = -1;
	}
	m_bPgDirty = false;
	m_bRebuildPending = false;
	m_audioPid = -1;
	{
		CAutoLock lock(&m_csAnchor);
		m_anchors.clear();
		m_bSeekPending = false;
		m_chapters.clear();
		m_chaptersGen++;
	}
	{
		CAutoLock lock(&m_csPlane);
		m_plane.clear();
		m_planeW = m_planeH = 0;
		m_planeVisible = false;
		m_bPlaneSynced = false;
		m_planeVersion = 0;
	}
	m_bNotifyPending = false;
	m_bFlushRequest = false;
	m_bOverlayOn = false;

	const int region = DetectRegion();
	lib.bdnav_set_player(nav, region, "chi", nullptr, "chi");
	lib.bdnav_set_notify(nav, &CBDNavigator::OnNotifyStatic, this);

	if (!WriteMarker(err)) {
		Close();
		return false;
	}

	CString msg;
	msg.Format(L"Navigation mode: \"%s\" titles=%d HDMV=%d BD-J=%d%s region=%d | %s",
		m_strLabel.GetString(), di.num_titles, di.num_hdmv_titles, di.num_bdj_titles,
		m_bBDJDisc ? L" (bundled Java 17)" : L"", region, root.GetString());
	BDNavLog(msg);
	FlushLibLog();

	m_strLockFile = LockFilePath();
	FILE* f = nullptr;
	if (_wfopen_s(&f, m_strLockFile, L"wb") == 0 && f) {
		const CStringA u8 = BDNavWToUTF8(root);
		fwrite(u8.GetString(), 1, u8.GetLength(), f);
		fclose(f);
	}
	return true;
}

int CBDNavigator::DetectRegion()
{
	// AnyDVD writes "region=N" into disc.inf; default region A
	auto& lib = CBDNavLib::Get();
	char buf[4096] = {};
	int n = lib.bdnav_read_file(m_nav, "disc.inf", (uint8_t*)buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = 0;
		const char* p = strstr(buf, "region=");
		if (p) {
			const int r = atoi(p + 7);
			switch (r) {
			case 1: return 1;   // A
			case 2: return 2;   // B
			case 3:
			case 4: return 4;   // C
			default: break;
			}
		}
	}
	return 1;
}

bool CBDNavigator::WriteMarker(CString& err)
{
	WCHAR tmp[MAX_PATH] = {};
	::GetTempPathW(MAX_PATH, tmp);
	CString dir = CString(tmp) + L"MPC-BE-BDMenu";
	::CreateDirectoryW(dir, nullptr);

	CString name = SanitizeFileName(m_strLabel);
	if (name.IsEmpty()) {
		name = L"Blu-ray";
	}
	m_strTempDir = dir + L"\\" + name;
	::CreateDirectoryW(m_strTempDir, nullptr);
	m_strMarker = dir + L"\\" + name + L".bdnav";

	FILE* f = nullptr;
	if (_wfopen_s(&f, m_strMarker, L"wb") != 0 || !f) {
		err = L"cannot write " + m_strMarker;
		return false;
	}
	const CStringA content = CStringA("BDNAV-MPC1\r\n") + BDNavWToUTF8(m_strRoot) + "\r\n";
	fwrite(content.GetString(), 1, content.GetLength(), f);
	fclose(f);
	return true;
}

CString CBDNavigator::PrepareClipInfoPath()
{
	CAutoLock lock(&m_csNav);
	if (!m_nav || m_playlist < 0 || m_strTempDir.IsEmpty()) {
		return L"";
	}
	auto& lib = CBDNavLib::Get();

	char clip[16] = {};
	char desc[1024] = {};
	if (!lib.bdnav_get_clip_info(m_nav, (uint32_t)m_playlist, (uint32_t)m_playitem, clip, sizeof(clip), desc, sizeof(desc)) || !clip[0]) {
		BDNavLog(L"clip info: not available");
		return L"";
	}
	const CString clipW = BDNavUTF8ToW(clip);
	CString m;
	m.Format(L"graph built for clip %s (%05d.mpls item %d): %s", clipW.GetString(), m_playlist, m_playitem, BDNavUTF8ToW(desc).GetString());
	BDNavLog(m);
	m_bGraphSig = ClipSig(m_playlist, m_playitem, m_graphSigV, m_graphSigA);
	{
		const char* pg = strstr(desc, " PG:");
		m_graphPG = pg ? atoi(pg + 4) : 0;
	}

	std::vector<uint8_t> data(16 * 1024 * 1024);
	const CStringA rel = CStringA("BDMV/CLIPINF/") + clip + ".clpi";
	const int n = lib.bdnav_read_file(m_nav, rel, data.data(), (int)data.size());
	if (n <= 0) {
		BDNavLog(L"clip info: cannot read " + CString(rel));
		return L"";
	}

	const CString bdmv = m_strTempDir + L"\\BDMV";
	::CreateDirectoryW(bdmv, nullptr);
	::CreateDirectoryW(bdmv + L"\\CLIPINF", nullptr);
	::CreateDirectoryW(bdmv + L"\\STREAM", nullptr);
	const CString clpi = bdmv + L"\\CLIPINF\\" + clipW + L".clpi";
	FILE* f = nullptr;
	if (_wfopen_s(&f, clpi, L"wb") != 0 || !f) {
		BDNavLog(L"clip info: cannot write " + clpi);
		return L"";
	}
	fwrite(data.data(), 1, (size_t)n, f);
	fclose(f);
	m_tempFiles.push_back(clpi);

	return bdmv + L"\\STREAM\\" + clipW + L".m2ts";
}

void CBDNavigator::Close()
{
	BDNAV* nav = nullptr;
	{
		CAutoLock lock(&m_csNav);
		nav = m_nav;
		m_nav = nullptr;
		m_bPlaying = false;
	}
	if (nav) {
		auto& lib = CBDNavLib::Get();
		lib.bdnav_set_notify(nav, nullptr, nullptr);
		lib.bdnav_close(nav);
		{
			CAutoLock lock(&m_csNav);
			if (m_dumpFile) {
				fclose(m_dumpFile);
				m_dumpFile = nullptr;
			}
			m_dumpSeg = 0;
			m_dumpBytes = 0;
		}
		FlushLibLog();
		BDNavLog(L"Navigation closed");
	}
	{
		CAutoLock lock(&m_csPlane);
		m_plane.clear();
		m_plane.shrink_to_fit();
		m_planeW = m_planeH = 0;
		m_planeVisible = false;
		m_bPlaneSynced = false;
	}
	m_bOverlayOn = false;
	if (!m_strMarker.IsEmpty()) {
		::DeleteFileW(m_strMarker);
		m_strMarker.Empty();
	}
	if (!m_strLockFile.IsEmpty()) {
		::DeleteFileW(m_strLockFile);
		m_strLockFile.Empty();
	}
	for (const auto& fn : m_tempFiles) {
		::DeleteFileW(fn);
	}
	m_tempFiles.clear();
	if (!m_strTempDir.IsEmpty()) {
		::RemoveDirectoryW(m_strTempDir + L"\\BDMV\\CLIPINF");
		::RemoveDirectoryW(m_strTempDir + L"\\BDMV\\STREAM");
		::RemoveDirectoryW(m_strTempDir + L"\\BDMV");
		::RemoveDirectoryW(m_strTempDir);
		m_strTempDir.Empty();
	}
	m_bNotifyPending = false;
}

bool CBDNavigator::IsOpen()
{
	CAutoLock lock(&m_csNav);
	return m_nav != nullptr;
}

void CBDNavigator::FlushLibLog()
{
	CBDNavLib& lib = CBDNavLib::Get();
	if (!lib.IsLoaded() || !m_libLogBudget) {
		return;
	}
	m_dwLibLogTick = ::GetTickCount();
	std::vector<char> buf(65536);
	const int n = lib.bdnav_get_log(buf.data(), (int)buf.size(), 1);
	if (n > 0) {
		size_t len = std::min<size_t>((size_t)n, m_libLogBudget);
		m_libLogBudget -= len;
		BDNavLogRaw(buf.data(), len);
		if (!m_libLogBudget) {
			BDNavLog(L"(libbluray log limit reached)");
		}
	}
}

bool CBDNavigator::StartPlayback()
{
	CAutoLock lock(&m_csNav);
	if (!m_nav) {
		return false;
	}
	if (m_bPlaying) {
		return true;
	}
	if (!CBDNavLib::Get().bdnav_play(m_nav)) {
		BDNavLog(L"bd_play() failed");
		FlushLibLog();
		return false;
	}
	m_bPlaying = true;
	m_dwPlayTick = ::GetTickCount();
	BDNavLog(L"bd_play() OK, First Play started");
	return true;
}

int CBDNavigator::ReadTS(BYTE* out, int outcap, bool& bEvent, bool& bFlush, std::vector<Marker>& markers)
{
	bEvent = false;
	bFlush = false;
	for (int i = 0; m_uiWaiting > 0 && i < 200; i++) {
		Sleep(1);
	}
	CAutoLock lock(&m_csNav);
	if (!m_nav || !m_bPlaying) {
		return -1;
	}

	if (m_bSegmentEnd) {
		bFlush = m_bFlushRequest.exchange(false);
		return kReadSegmentEnd;
	}

	const int want = (outcap / 6144) * 6144;
	if (want <= 0) {
		return 0;
	}

	if (!m_carry.empty() && (int)m_carry.size() <= outcap) {
		// first data of this graph segment (read together with the boundary event)
		const int n = (int)m_carry.size();
		memcpy(out, m_carry.data(), n);
		m_carry.clear();
		DumpData(out, n);
		bFlush = m_bFlushRequest.exchange(false);
		if (!m_pendingMarkers.empty()) {
			markers.insert(markers.end(), m_pendingMarkers.begin(), m_pendingMarkers.end());
			m_pendingMarkers.clear();
		}
		m_total += (unsigned)n;
		return n;
	}

	BDNAV_EVENT ev = {};
	uint32_t rflags = 0;
	const int r = CBDNavLib::Get().bdnav_read_ex(m_nav, out, want, &ev, &rflags);
	if ((rflags & BDNAV_READ_POS_JUMP) && ev.event == BDNAV_EV_NONE && m_bBDJDisc) {
		// BD-J moved the playback position without telling us: new time anchor for the progress bar
		QueueMarker();
		if (m_bVerbose && m_logBudget > 0) {
			m_logBudget--;
			BDNavLog(L"read position jumped without an event (BD-J) -> new time anchor");
		}
	}
	if (ev.event != BDNAV_EV_NONE) {
		// idle (BD-J menu without a playlist) / still picture: no data, the reader thread waits a bit
		bEvent = ev.event != BDNAV_EV_IDLE && ev.event != BDNAV_EV_STILL_TIME && ev.event != BDNAV_EV_END_OF_TITLE;
		HandleEvent(ev);
		if (ev.event == BDNAV_EV_ENCRYPTED) {
			return -1;
		}
		if (m_bSegmentEnd) {
			// the data returned with the boundary event belongs to the new clip
			if (r > 0) {
				m_carry.assign(out, out + r);
			}
			bFlush = m_bFlushRequest.exchange(false);
			m_bRebuildPending = true;
			// a user jump (menu button, top menu) into another format: the old data is obsolete -> rebuild now
			// (by bytes, not time: the disc reads far ahead, e.g. Odyssey PLAY -> 21 s of warnings read in 0.2 s)
			const bool bNow = bFlush || (m_dwLastFlushTick && ::GetTickCount() - m_dwLastFlushTick < 3000
				&& m_total - m_flushTotal < 1024 * 1024);
			if (m_hNotify) {
				::PostMessageW(m_hNotify, WM_BDNAV_NOTIFY, kNotifyRebuild, bNow ? 1 : 0);
			}
			return kReadSegmentEnd;
		}
	}
	if (r < 0) {
		BDNavLog(L"bd_read() error, end of navigation");
		FlushLibLog();
		if (!m_bMenuSeen && !m_bFallbackPosted && ::GetTickCount() - m_dwPlayTick < 20000 && m_hNotify) {
			m_bFallbackPosted = true;
			BDNavLog(L"navigation failed at the start -> original playback");
			::PostMessageW(m_hNotify, WM_BDNAV_NOTIFY, kNotifyFallback, 0);
		}
		return -1;
	}
	bFlush = m_bFlushRequest.exchange(false);
	if (!m_pendingMarkers.empty()) {
		markers.insert(markers.end(), m_pendingMarkers.begin(), m_pendingMarkers.end());
		m_pendingMarkers.clear();
	}

	if ((r % 192) && !m_bAlignWarned) {
		m_bAlignWarned = true;
		CString m;
		m.Format(L"warning: bd_read returned %d bytes (not a multiple of 192)", r);
		BDNavLog(m);
	}
	m_total += (unsigned)r;
	DumpData(out, r);

	CheckAutoMenu();

	if (::GetTickCount() - m_dwLibLogTick > 1000) {
		FlushLibLog();
	}
	return r;
}

void CBDNavigator::HandleEvent(const BDNAV_EVENT& ev)
{
	auto& lib = CBDNavLib::Get();
	CString extra;
	bool bJump = false;
	bool bLog = m_bVerbose;

	switch (ev.event) {
	case BDNAV_EV_TITLE: {
		m_title = (int)ev.param;
		m_titleTick = ::GetTickCount();
		m_titleBytes = m_total;
		bJump = true;
		const int kind = lib.bdnav_get_title_kind(m_nav, ev.param);
		m_titleFlags = kind > 0 ? kind : 0;
		m_bBDJTitle = kind >= 0 && (kind & BDNAV_TITLE_BDJ);
		extra.Format(L" (%s%s)", m_bBDJTitle ? L"BD-J" : L"HDMV", (m_titleFlags & BDNAV_TITLE_INTERACTIVE) ? L", interactive" : L"");
		break;
	}
	case BDNAV_EV_KEY_INTEREST_TABLE:
		if ((int)ev.param != m_keyInterest) {
			m_keyInterest = (int)ev.param;
			bLog = true;
		}
		break;
	case BDNAV_EV_PLAYLIST: {
		m_playlist = (int)ev.param;
		m_playitem = 0;
		BDNAV_TITLE_INFO ti = {};
		m_playlistDur = lib.bdnav_get_playlist_info(m_nav, ev.param, &ti) ? ti.duration : 0;
		extra.Format(L" %05u.mpls (%.1f s, %u chapters)", ev.param, m_playlistDur / 90000.0, ti.chapter_count);
		bJump = true;
		UpdateChapters();
		if (CheckFormatChange(extra)) {
			bLog = true;
		}
		m_bPgDirty = true;   // stream numbers map to other PIDs in the new playlist
		break;
	}
	case BDNAV_EV_PG_TEXTST:
		m_pgEnabled = ev.param != 0;
		m_bPgDirty = true;
		PostNotify();
		bLog = true;
		break;
	case BDNAV_EV_PG_TEXTST_STREAM:
		m_pgNumber = (ev.param >= 1 && ev.param < 0xFFF) ? (int)ev.param : 0;
		m_bPgDirty = true;
		PostNotify();
		bLog = true;
		break;
	case BDNAV_EV_AUDIO_STREAM:
		if (ev.param >= 1 && ev.param < 0xFF && m_playlist >= 0) {
			m_audioNumber = (int)ev.param;
			const int pid = lib.bdnav_get_stream_pid(m_nav, (uint32_t)m_playlist, (uint32_t)std::max(m_playitem, 0), 0, m_audioNumber);
			if (pid > 0) {
				m_audioPid = pid;
				extra.Format(L" pid 0x%04x", pid);
				PostNotify();
			}
		}
		break;
	case BDNAV_EV_PLAYITEM: {
		m_playitem = (int)ev.param;
		char clip[16] = {};
		char desc[512] = {};
		if (m_playlist >= 0 && lib.bdnav_get_clip_info(m_nav, (uint32_t)m_playlist, ev.param, clip, sizeof(clip), desc, sizeof(desc))) {
			extra.Format(L" clip %s: %s", BDNavUTF8ToW(clip).GetString(), BDNavUTF8ToW(desc).GetString());
		}
		CheckFormatChange(extra);
		bLog = true;   // clip boundaries are rare and useful in the log
		break;
	}
	case BDNAV_EV_SEEK:
		bJump = true;
		break;
	case BDNAV_EV_POPUP:
		m_popupAvail = (int)ev.param;
		break;
	default:
		break;
	}

	if (ev.event == BDNAV_EV_PLAYLIST || ev.event == BDNAV_EV_SEEK) {
		QueueMarker();
	}

	if (bJump && m_bUserJumpPending && ::GetTickCount() - m_dwUserActionTick < kUserJumpWindow) {
		m_bUserJumpPending = false;
		m_bFlushRequest = true;   // jump requested by the user: drop data buffered for the old position
		m_dwLastFlushTick = ::GetTickCount();
		m_flushTotal = m_total;
		extra += L" [flush]";
	}

	if (ev.event == BDNAV_EV_IDLE || ev.event == BDNAV_EV_STILL_TIME || ev.event == BDNAV_EV_UO_MASK_CHANGED) {
		bLog = false;   // repeated many times per second
	}
	const bool bRepeat = (ev.event == BDNAV_EV_END_OF_TITLE && m_lastEvent == BDNAV_EV_END_OF_TITLE);
	m_lastEvent = ev.event;
	switch (ev.event) {
	case BDNAV_EV_TITLE: case BDNAV_EV_PLAYLIST: case BDNAV_EV_MENU: case BDNAV_EV_STILL:
	case BDNAV_EV_END_OF_TITLE: case BDNAV_EV_ERROR: case BDNAV_EV_READ_ERROR: case BDNAV_EV_ENCRYPTED:
		bLog = true;
		break;
	default:
		break;
	}
	if (bLog && !bRepeat && m_logBudget > 0) {
		m_logBudget--;
		CString m;
		m.Format(L"event %s %u%s (read %.1f MB)", EventName(ev.event), ev.param, extra.GetString(), m_total / 1048576.0);
		BDNavLog(m);
	}
}

bool CBDNavigator::ClipSig(int playlist, int item, CString& v, CString& a)
{
	// m_csNav held. desc: "V: 1011=HEVC/f8 1015=HEVC/f6 A: 1100=LPCM/f1/r1/jpn 1101=AC3/... PG:1 IG:0"
	v.Empty();
	a.Empty();
	char clip[16] = {};
	char desc[1024] = {};
	if (playlist < 0 || !CBDNavLib::Get().bdnav_get_clip_info(m_nav, (uint32_t)playlist, (uint32_t)std::max(item, 0), clip, sizeof(clip), desc, sizeof(desc))) {
		return false;
	}
	const CString d = BDNavUTF8ToW(desc);
	const int iA  = d.Find(L" A:");
	const int iPG = d.Find(L" PG:");
	if (iA < 0 || iPG < iA) {
		return false;
	}
	v = d.Mid(2, iA - 2).Trim();
	int pos = 0;
	const CString aud = d.Mid(iA + 3, iPG - iA - 3);
	for (CString t = aud.Tokenize(L" ", pos); !t.IsEmpty(); t = aud.Tokenize(L" ", pos)) {
		const int slash = t.Find(L'/');
		a += (a.IsEmpty() ? L"" : L" ") + (slash > 0 ? t.Left(slash) : t);   // "1100=LPCM"
	}
	return true;
}

bool CBDNavigator::CheckFormatChange(CString& extra)
{
	// m_csNav held
	CString v, a;
	if (m_bSegmentEnd || !m_bGraphSig || !ClipSig(m_playlist, m_playitem, v, a)) {
		return false;
	}
	bool bRebuild = (v != m_graphSigV);
	int pos = 0;
	for (CString t = a.Tokenize(L" ", pos); !t.IsEmpty(); t = a.Tokenize(L" ", pos)) {
		if ((L" " + m_graphSigA + L" ").Find(L" " + t + L" ") < 0) {
			bRebuild = true;   // a new audio stream (the current graph has no pin for it)
		}
	}
	// a clip with PG subtitles while the graph has none: rebuild so that they can be shown
	char clip[16] = {};
	char desc[1024] = {};
	if (!bRebuild && CBDNavLib::Get().bdnav_get_clip_info(m_nav, (uint32_t)m_playlist, (uint32_t)std::max(m_playitem, 0), clip, sizeof(clip), desc, sizeof(desc))) {
		const char* pg = strstr(desc, " PG:");
		if (pg && atoi(pg + 4) > 0 && m_graphPG == 0) {
			bRebuild = true;
		}
	}
	if (!bRebuild) {
		return false;
	}
	m_bSegmentEnd = true;
	m_graphSigV = v;
	m_graphSigA = a;
	{
		const char* pg = strstr(desc, " PG:");
		m_graphPG = pg ? atoi(pg + 4) : 0;   // desc may be empty if bRebuild was decided before reading it
		if (!desc[0]) {
			char d2[1024] = {};
			if (CBDNavLib::Get().bdnav_get_clip_info(m_nav, (uint32_t)m_playlist, (uint32_t)std::max(m_playitem, 0), clip, sizeof(clip), d2, sizeof(d2))) {
				const char* pg2 = strstr(d2, " PG:");
				m_graphPG = pg2 ? atoi(pg2 + 4) : 0;
			}
		}
	}
	QueueMarker();     // time anchor for the first data of the new graph
	extra += L" [format change -> rebuild graph]";
	return true;
}

int CBDNavigator::GetDiscAudioPid()
{
	CAutoLock lock(&m_csNav);
	if (!m_nav || m_audioNumber < 1 || m_playlist < 0) {
		return -1;
	}
	return CBDNavLib::Get().bdnav_get_stream_pid(m_nav, (uint32_t)m_playlist, (uint32_t)std::max(m_playitem, 0), 0, m_audioNumber);
}

int CBDNavigator::GetDiscSubtitlePid(bool& bEnabled)
{
	CAutoLock lock(&m_csNav);
	bEnabled = m_pgEnabled;
	if (!m_nav || m_pgNumber < 1 || m_playlist < 0) {
		return -1;
	}
	return CBDNavLib::Get().bdnav_get_stream_pid(m_nav, (uint32_t)m_playlist, (uint32_t)std::max(m_playitem, 0), 1, m_pgNumber);
}

bool CBDNavigator::IsSegmentPlayedOut()
{
	CAutoLock lock(&m_csReader);
	return !m_pReader || m_pReader->IsPlayedOut();
}

int CBDNavigator::SegmentRemainingMs()
{
	CAutoLock lock(&m_csReader);
	return m_pReader ? m_pReader->RemainingMs() : -1;
}

void CBDNavigator::ClearReader(CBDNavReader* pReader)
{
	// the old graph's reader may be destroyed after the new one registered itself
	CAutoLock lock(&m_csReader);
	if (m_pReader == pReader) {
		m_pReader = nullptr;
	}
}

void CBDNavigator::DumpNewSegment()
{
	// m_csNav held
	if (m_dumpFile) {
		fclose(m_dumpFile);
		m_dumpFile = nullptr;
	}
	if (!::PathFileExistsW(BDNavGetDataDir() + L"\\bdnav_dump.txt") || m_dumpBytes > 4000ull * 1024 * 1024) {
		return;
	}
	const CString dir = BDNavGetDataDir() + L"\\dump";
	::CreateDirectoryW(dir, nullptr);
	CString path;
	path.Format(L"%s\\seg_%02d.m2ts", dir.GetString(), ++m_dumpSeg);
	m_dumpFile = _wfopen(path, L"wb");
	CString m;
	m.Format(L"dump: %s%s", path.GetString(), m_dumpFile ? L"" : L" (open failed)");
	BDNavLog(m);
}

void CBDNavigator::DumpData(const BYTE* p, int n)
{
	if (m_dumpFile && n > 0) {
		fwrite(p, 1, n, m_dumpFile);
		m_dumpBytes += (unsigned)n;
	}
}

void CBDNavigator::BeginSegment()
{
	{
		CAutoLock lock(&m_csNav);
		m_bSegmentEnd = false;
		DumpNewSegment();
	}
	m_bRebuildPending = false;
	CAutoLock lock(&m_csAnchor);
	m_anchors.clear();
	m_bSeekPending = false;
}

void CBDNavigator::UpdateChapters()
{
	// m_csNav held
	std::vector<uint64_t> starts(1000);
	const int n = CBDNavLib::Get().bdnav_get_chapters(m_nav, (uint32_t)m_playlist, starts.data(), (int)starts.size());
	CAutoLock lock(&m_csAnchor);
	m_chapters.clear();
	for (int i = 0; i < n && i < (int)starts.size(); i++) {
		m_chapters.push_back((REFERENCE_TIME)(starts[i] * 1000 / 9));
	}
	m_chaptersGen++;
	PostNotify();
}

bool CBDNavigator::GetChapters(std::vector<REFERENCE_TIME>& chapters, uint32_t& gen)
{
	CAutoLock lock(&m_csAnchor);
	if (gen == m_chaptersGen) {
		return false;
	}
	gen = m_chaptersGen;
	chapters = m_chapters;
	return true;
}

void CBDNavigator::CheckAutoMenu()
{
	// User decision 2026-11-24: if the disc goes straight into the main feature
	// (no menu shown), call up the menu automatically.
	if (m_bAutoMenuDone || m_bMenuSeen) {
		return;
	}
	if (m_title < 1 || m_title == 0xFFFF || m_playlistDur < kLongTitle90k) {
		return;
	}
	if (::GetTickCount() - m_titleTick < kAutoMenuDelay || m_total - m_titleBytes < kAutoMenuBytes) {
		return;
	}

	m_bAutoMenuDone = true;
	auto& lib = CBDNavLib::Get();
	if (m_popupAvail || m_bBDJTitle) {
		lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_POPUP);
		BDNavLog(L"auto menu: main title started without menu -> pop-up menu");
	} else {
		const int r = lib.bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"auto menu: main title started without menu -> top menu"
		           : L"auto menu: top menu call refused by disc");
	}
}

void CBDNavigator::QueueMarker()
{
	// m_csNav held
	Marker m = {};
	m.title90k = CBDNavLib::Get().bdnav_tell_time(m_nav);
	m.dur90k   = m_playlistDur;
	m.playlist = m_playlist;
	m.seq      = ++m_markerSeq;
	m_pendingMarkers.push_back(m);
}

void CBDNavigator::SetReader(CBDNavReader* pReader)
{
	CAutoLock lock(&m_csReader);
	m_pReader = pReader;
}

REFERENCE_TIME CBDNavigator::GetStreamTime()
{
	CAutoLock lock(&m_csReader);
	return m_pReader ? m_pReader->GetStreamTimeNow() : 0;
}

void CBDNavigator::AddAnchor(REFERENCE_TIME rtStream, const Marker& m)
{
	Anchor a;
	a.rt       = rtStream;
	a.title    = (REFERENCE_TIME)(m.title90k * 1000 / 9);
	a.dur      = (REFERENCE_TIME)(m.dur90k * 1000 / 9);
	a.playlist = m.playlist;
	a.seq      = m.seq;
	{
		CAutoLock lock(&m_csAnchor);
		// anchors at/after this stream time belong to data that was flushed
		while (!m_anchors.empty() && m_anchors.back().rt >= a.rt) {
			m_anchors.pop_back();
		}
		m_anchors.push_back(a);
		while (m_anchors.size() > 16) {
			m_anchors.pop_front();
		}
	}
	if (m_bVerbose && m_traceBudget > 0) {
		m_traceBudget--;
		CString msg;
		msg.Format(L"time anchor: stream %.3f s = %05d.mpls %.3f / %.1f s", rtStream / 1e7, m.playlist, a.title / 1e7, a.dur / 1e7);
		BDNavLog(msg);
	}
}

bool CBDNavigator::GetTitlePos(REFERENCE_TIME& pos, REFERENCE_TIME& dur)
{
	pos = dur = 0;
	if (m_title == 0 || (m_titleFlags & BDNAV_TITLE_INTERACTIVE)) {
		return true;   // top menu / interactive title: no seek bar
	}
	if (m_bBDJTitle && m_playlistDur < kLongTitle90k) {
		bool menu;
		{
			CAutoLock lock(&m_csPlane);
			menu = m_planeVisible;
		}
		if (menu) {
			return true;   // BD-J menu over a short looping clip (e.g. language selection): no seek bar
		}
	}
	const REFERENCE_TIME now = GetStreamTime();
	CAutoLock lock(&m_csAnchor);

	if (m_bSeekPending) {
		bool done = ::GetTickCount() - m_seekTick > 8000;
		for (const auto& a : m_anchors) {
			if (a.seq >= m_seekSeq && a.rt <= now) {
				done = true;   // the seek target is playing now
			}
		}
		if (!done) {
			pos = m_seekPos;
			dur = m_anchors.empty() ? 0 : m_anchors.back().dur;
			if (dur > 0 && pos > dur) {
				pos = dur;
			}
			return dur > 0;
		}
		m_bSeekPending = false;
	}

	const Anchor* best = nullptr;
	for (const auto& a : m_anchors) {
		if (a.rt <= now) {
			best = &a;
		}
	}
	if (!best) {
		return false;
	}
	dur = best->dur;
	pos = best->title + std::max<REFERENCE_TIME>(now - best->rt, 0);
	if (dur > 0 && pos > dur) {
		pos = dur;
	}
	return true;
}

bool CBDNavigator::SeekTitle(REFERENCE_TIME pos)
{
	CUILock lock(*this);
	if (!m_nav || !m_bPlaying) {
		return false;
	}
	if (pos < 0) {
		pos = 0;
	}
	UserAction();
	m_bUserJumpPending = false;   // the flush is requested right here
	const uint64_t tick = (uint64_t)pos * 9 / 1000;
	const int64_t r = CBDNavLib::Get().bdnav_seek_time(m_nav, tick);
	CString msg;
	msg.Format(L"user: seek to %.3f s -> %lld", pos / 1e7, r);
	BDNavLog(msg);
	if (r < 0) {
		FlushLibLog();
		return false;
	}
	m_bFlushRequest = true;
	QueueMarker();
	{
		CAutoLock lockA(&m_csAnchor);
		m_bSeekPending = true;
		m_seekSeq  = m_markerSeq;
		m_seekPos  = pos;
		m_seekTick = ::GetTickCount();
	}
	FlushLibLog();
	return true;
}

void CBDNavigator::OnNotifyStatic(void* ctx, int plane, int reason)
{
	static_cast<CBDNavigator*>(ctx)->OnNotify(plane, reason);
}

// called from inside bdnav_* calls (m_csNav is held by the calling thread)
void CBDNavigator::OnNotify(int plane, int reason)
{
	if (plane != BDNAV_PLANE_IG || !m_nav) {
		return;
	}

	auto& lib = CBDNavLib::Get();
	BDNAV_PLANE p = {};
	bool visible = false;
	if (lib.bdnav_lock_plane(m_nav, BDNAV_PLANE_IG, &p)) {
		{
			CAutoLock lock(&m_csPlane);
			if (p.w > 0 && p.h > 0) {
				m_planeW = p.w;
				m_planeH = p.h;
			}
			if (reason == BDNAV_OV_FLUSH && p.argb && p.visible && p.w > 0 && p.h > 0) {
				const size_t npx = (size_t)p.w * p.h;
				// partial update (only the area the disc changed) if our copy is in sync with the dll plane
				const bool partial = m_bPlaneSynced && m_plane.size() == npx && m_planeConv.size() == npx;
				RECT dr = { 0, 0, p.w, p.h };
				if (partial) {
					dr = (p.dx1 >= p.dx0) ? RECT{ p.dx0, p.dy0, p.dx1 + 1, p.dy1 + 1 } : RECT{ 0, 0, 0, 0 };
				}
				m_plane.resize(npx);
				m_planeConv.resize(npx);
				if (dr.right > dr.left) {
					const size_t bytes = (size_t)(dr.right - dr.left) * 4;
					for (LONG y = dr.top; y < dr.bottom; y++) {
						memcpy(&m_plane[(size_t)y * p.w + dr.left], p.argb + (size_t)y * p.stride + dr.left, bytes);
					}
				}
				const RECT oldBox = m_planeBox;
				const RECT newBox = { p.x0, p.y0, p.x1 + 1, p.y1 + 1 };
				// m_planeConv is valid inside m_planeBox: convert the changed area, or the whole box if it grew
				const bool inside = partial && newBox.left >= oldBox.left && newBox.top >= oldBox.top
					&& newBox.right <= oldBox.right && newBox.bottom <= oldBox.bottom;
				RECT cv = newBox;
				if (inside) {
					cv.left = std::max(dr.left, newBox.left);
					cv.top = std::max(dr.top, newBox.top);
					cv.right = std::min(dr.right, newBox.right);
					cv.bottom = std::min(dr.bottom, newBox.bottom);
				}
				m_planeBox = newBox;
				m_planeVisible = true;
				m_bPlaneSynced = true;

				// convert once: premultiplied colour, inverted alpha (MPC subpicture format)
				for (int y = cv.top; y < cv.bottom; y++) {
					const uint32_t* s = &m_plane[(size_t)y * p.w];
					uint32_t* d = &m_planeConv[(size_t)y * p.w];
					for (int x = cv.left; x < cv.right; x++) {
						const uint32_t c = s[x];
						const uint32_t a = c >> 24;
						if (a == 0) {
							d[x] = 0xFF000000;
						} else if (a == 255) {
							d[x] = c & 0x00FFFFFF;
						} else {
							const uint32_t m = a * 257 + 1;   // x*a/255 ~= (x*m) >> 16
							const uint32_t r = (((c >> 16) & 0xFF) * m) >> 16;
							const uint32_t g = (((c >> 8) & 0xFF) * m) >> 16;
							const uint32_t b = ((c & 0xFF) * m) >> 16;
							d[x] = ((255 - a) << 24) | (r << 16) | (g << 8) | b;
						}
					}
				}
			} else {
				// empty flush / hide / close: keep the last content (the main thread hides it after a short delay)
				m_planeVisible = false;
				m_bPlaneSynced = false;
				if (reason == BDNAV_OV_CLOSE) {
					m_plane.clear();
				}
			}
			m_planeVersion++;
			visible = m_planeVisible;
			m_dwPlaneTick = ::GetTickCount();
		}
		if (m_bVerbose && m_traceBudget > 0 && m_dwInputTick && m_dwPlaneTick - m_dwInputTick < 2000) {
			m_traceBudget--;
			CString m;
			m.Format(L"trace: plane v%u reason=%d visible=%d, %u ms after input", m_planeVersion, reason, visible ? 1 : 0, m_dwPlaneTick - m_dwInputTick);
			BDNavLog(m);
		}
		lib.bdnav_unlock_plane(m_nav, BDNAV_PLANE_IG);
	}

	if (visible && !m_bMenuSeen) {
		m_bMenuSeen = true;
		BDNavLog(L"menu visible");
	}
	PostNotify();
}

void CBDNavigator::PostNotify()
{
	if (m_hNotify && !m_bNotifyPending.exchange(true)) {
		::PostMessageW(m_hNotify, WM_BDNAV_NOTIFY, 0, 0);
	}
}

bool CBDNavigator::IsRenderPending()
{
	CAutoLock lock(&m_csPlane);
	return m_bOverlayOn && m_planeVisible && m_planeVersion != m_lastRenderedVersion;
}

bool CBDNavigator::IsMenuVisible()
{
	CAutoLock lock(&m_csPlane);
	return m_planeVisible;
}

bool CBDNavigator::Mouse(int x, int y, int vw, int vh, bool bClick)
{
	if (vw <= 0 || vh <= 0) {
		return false;
	}
	int pw = 1920, ph = 1080;
	{
		CAutoLock lock(&m_csPlane);
		if (m_planeW > 0 && m_planeH > 0) {
			pw = m_planeW;
			ph = m_planeH;
		}
	}
	const int px = MulDiv(x, pw, vw);
	const int py = MulDiv(y, ph, vh);
	if (px < 0 || py < 0 || px >= pw || py >= ph) {
		return false;
	}

	CUILock lock(*this);
	if (!m_nav) {
		return false;
	}
	auto& lib = CBDNavLib::Get();
	if (!bClick && px == m_lastMouseX && py == m_lastMouseY) {
		return false;
	}
	m_lastMouseX = px;
	m_lastMouseY = py;

	m_dwInputTick = ::GetTickCount();
	const int r = lib.bdnav_mouse_select(m_nav, -1, px, py);
	if (m_bBDJTitle) {
		// BD-J menus are made for the remote control and rarely handle the mouse:
		// a click activates the button under the cursor if the disc reports one, otherwise it is ENTER
		if (!bClick) {
			return false;
		}
		UserAction();
		const uint32_t key = r > 0 ? BDNAV_KEY_MOUSE_ACTIVATE : BDNAV_KEY_ENTER;
		const int r2 = lib.bdnav_user_input(m_nav, -1, key);
		CString m;
		m.Format(L"mouse click (%d,%d) BD-J select=%d -> %s %d", px, py, r, r > 0 ? L"activate" : L"ENTER", r2);
		BDNavLog(m);
		FlushLibLog();
		return true;
	}
	if (bClick) {
		int r2 = -100;
		if (r > 0) {
			UserAction();
			r2 = lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_MOUSE_ACTIVATE);
		}
		CString m;
		m.Format(L"mouse click (%d,%d) select=%d activate=%d", px, py, r, r2);
		BDNavLog(m);
		FlushLibLog();
	}
	return r > 0;
}

void CBDNavigator::Key(uint32_t key)
{
	CUILock lock(*this);
	if (m_nav) {
		UserAction();
		m_dwInputTick = ::GetTickCount();
		const int r = CBDNavLib::Get().bdnav_user_input(m_nav, -1, key);
		CString m;
		m.Format(L"user key %u -> %d", key, r);
		BDNavLog(m);
		FlushLibLog();
	}
}

void CBDNavigator::MenuCall()
{
	CUILock lock(*this);
	if (m_nav) {
		UserAction();
		const int r = CBDNavLib::Get().bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"user: top menu" : L"user: top menu refused by disc");
		FlushLibLog();
	}
}

void CBDNavigator::Popup()
{
	CUILock lock(*this);
	if (!m_nav) {
		return;
	}
	UserAction();
	auto& lib = CBDNavLib::Get();
	if (m_popupAvail || m_bBDJTitle) {   // BD-J: the Xlet decides (no POPUP event)
		const int r = lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_POPUP);
		CString m;
		m.Format(L"user: pop-up menu -> %d", r);
		BDNavLog(m);
	} else {
		const int r = lib.bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"user: pop-up n/a -> top menu" : L"user: top menu refused by disc");
	}
	FlushLibLog();
}

bool CBDNavigator::GetPlaneSize(SIZE& sz)
{
	CAutoLock lock(&m_csPlane);
	sz.cx = m_planeW > 0 ? m_planeW : 1920;
	sz.cy = m_planeH > 0 ? m_planeH : 1080;
	return true;
}

bool CBDNavigator::RenderMenu(SubPicDesc& spd, RECT& bbox)
{
	SetRectEmpty(&bbox);

	if (!m_bOverlayOn) {
		return false;
	}

	CAutoLock lock(&m_csPlane);
	if (m_plane.empty() || !spd.bits || spd.bpp != 32 || spd.w <= 0 || spd.h <= 0 || m_planeW <= 0 || m_planeH <= 0) {
		return false;
	}

	const int sw = m_planeW, sh = m_planeH;
	if ((size_t)sw * sh != m_plane.size()) {
		return false;
	}
	const RECT sb = m_planeBox;
	// destination rectangle (nearest-neighbour scaling if sizes differ)
	RECT db;
	db.left   = MulDiv(sb.left,   spd.w, sw);
	db.top    = MulDiv(sb.top,    spd.h, sh);
	db.right  = MulDiv(sb.right,  spd.w, sw);
	db.bottom = MulDiv(sb.bottom, spd.h, sh);
	db.left   = std::max<LONG>(db.left, 0);
	db.top    = std::max<LONG>(db.top, 0);
	db.right  = std::min<LONG>(db.right, spd.w);
	db.bottom = std::min<LONG>(db.bottom, spd.h);
	if (db.right <= db.left || db.bottom <= db.top) {
		return false;
	}

	const bool same = (sw == spd.w && sh == spd.h);
	if (same && m_planeConv.size() == m_plane.size()) {
		const size_t bytes = (size_t)(db.right - db.left) * 4;
		for (LONG y = db.top; y < db.bottom; y++) {
			memcpy(spd.bits + (size_t)spd.pitch * y + (size_t)db.left * 4, &m_planeConv[(size_t)y * sw + db.left], bytes);
		}
	} else
	for (LONG y = db.top; y < db.bottom; y++) {
		const int syy = same ? y : std::min(MulDiv(y, sh, spd.h), sh - 1);
		const uint32_t* src = &m_plane[(size_t)syy * sw];
		uint32_t* dst = (uint32_t*)(spd.bits + (size_t)spd.pitch * y);
		for (LONG x = db.left; x < db.right; x++) {
			const int sxx = same ? x : std::min(MulDiv(x, sw, spd.w), sw - 1);
			const uint32_t c = src[sxx];
			const uint32_t a = c >> 24;
			if (a == 0) {
				dst[x] = 0xFF000000;   // transparent (MPC subpic: inverted alpha)
			} else {
				const uint32_t r = ((c >> 16) & 0xFF) * a / 255;
				const uint32_t g = ((c >> 8) & 0xFF) * a / 255;
				const uint32_t b = (c & 0xFF) * a / 255;
				dst[x] = ((255 - a) << 24) | (r << 16) | (g << 8) | b;   // premultiplied, inverted alpha
			}
		}
	}

	bbox = db;
	if (m_planeVersion != m_lastRenderedVersion) {
		m_lastRenderedVersion = m_planeVersion;
		if (m_bVerbose && m_traceBudget > 0 && m_dwInputTick) {
			m_traceBudget--;
			CString m;
			m.Format(L"trace: rendered v%u, %u ms after plane change", m_planeVersion, ::GetTickCount() - m_dwPlaneTick);
			BDNavLog(m);
		}
	}
	return true;
}
