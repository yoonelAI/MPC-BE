/*
 * BDNavigator.cpp - Blu-ray navigation session (libbluray via bdnav.dll) for MPC-BE BDMenu
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDNavigator.h"
#include "SubPic/ISubPic.h"

static const uint64_t kLongTitle90k   = 10ull * 60 * 90000;   // >= 10 min: treat as main feature
static const DWORD    kAutoMenuDelay  = 2500;                 // ms of playback before auto menu
static const unsigned long long kAutoMenuBytes = 8ull * 1024 * 1024;

CBDNavigator& CBDNavigator::Get()
{
	static CBDNavigator nav;
	return nav;
}

CBDNavigator::~CBDNavigator()
{
	// process exit: the dll may already be unloading, don't call into it
	m_nav = nullptr;
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

bool CBDNavigator::Open(const CString& path, HWND hNotify, CString& err)
{
	Close();

	auto& lib = CBDNavLib::Get();
	if (!lib.Load()) {
		err = lib.GetError();
		return false;
	}

	const CString root = BDNavGetDiscRoot(path);
	char buf[2048] = {};
	BDNAV* nav = lib.bdnav_open(BDNavWToUTF8(root), buf, sizeof(buf));
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
	} else if (di.num_bdj_titles > 0 || di.bdj_detected) {
		reason = L"BD-J disc (not supported yet)";
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
		m_playlistDur = 0;
		m_titleTick = 0;
		m_titleBytes = 0;
		m_popupAvail = 0;
		m_bMenuSeen = false;
		m_bAutoMenuDone = false;
		m_lastMouseX = m_lastMouseY = -1;
		m_logBudget = 400;
		m_raw.resize(192 * 32 * 6);
	}
	{
		CAutoLock lock(&m_csPlane);
		m_plane.clear();
		m_planeW = m_planeH = 0;
		m_planeVisible = false;
		m_planeVersion = 0;
	}
	m_bNotifyPending = false;

	const int region = DetectRegion();
	lib.bdnav_set_player(nav, region, "chi", nullptr, "chi");
	lib.bdnav_set_notify(nav, &CBDNavigator::OnNotifyStatic, this);

	if (!WriteMarker(err)) {
		Close();
		return false;
	}

	CString msg;
	msg.Format(L"Navigation mode: \"%s\" titles=%d HDMV=%d region=%d | %s",
		m_strLabel.GetString(), di.num_titles, di.num_hdmv_titles, region, root.GetString());
	BDNavLog(msg);
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
		BDNavLog(L"Navigation closed");
	}
	{
		CAutoLock lock(&m_csPlane);
		m_plane.clear();
		m_plane.shrink_to_fit();
		m_planeW = m_planeH = 0;
		m_planeVisible = false;
	}
	if (!m_strMarker.IsEmpty()) {
		::DeleteFileW(m_strMarker);
		m_strMarker.Empty();
	}
	m_bNotifyPending = false;
}

bool CBDNavigator::IsOpen()
{
	CAutoLock lock(&m_csNav);
	return m_nav != nullptr;
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
		return false;
	}
	m_bPlaying = true;
	BDNavLog(L"bd_play() OK, First Play started");
	return true;
}

int CBDNavigator::ReadTS(BYTE* out, int outcap, bool& bEvent)
{
	bEvent = false;
	CAutoLock lock(&m_csNav);
	if (!m_nav || !m_bPlaying) {
		return -1;
	}

	int want = (outcap / 188) * 192;
	if (want > (int)m_raw.size()) {
		want = (int)m_raw.size();
	}
	want -= want % 192;
	if (want <= 0) {
		return 0;
	}

	BDNAV_EVENT ev = {};
	const int r = CBDNavLib::Get().bdnav_read(m_nav, m_raw.data(), want, &ev);
	if (ev.event != BDNAV_EV_NONE) {
		bEvent = true;
		HandleEvent(ev);
		if (ev.event == BDNAV_EV_ENCRYPTED) {
			return -1;
		}
	}
	if (r < 0) {
		BDNavLog(L"bd_read() error, end of navigation");
		return -1;
	}

	int o = 0;
	for (int i = 0; i + 192 <= r; i += 192) {
		memcpy(out + o, m_raw.data() + i + 4, 188);
		o += 188;
	}
	m_total += (unsigned)r;

	CheckAutoMenu();
	return o;
}

void CBDNavigator::HandleEvent(const BDNAV_EVENT& ev)
{
	auto& lib = CBDNavLib::Get();
	CString msg;

	switch (ev.event) {
	case BDNAV_EV_TITLE:
		m_title = (int)ev.param;
		m_titleTick = ::GetTickCount();
		m_titleBytes = m_total;
		msg.Format(L"event TITLE %d", m_title == 0xFFFF ? -1 : m_title);
		break;
	case BDNAV_EV_PLAYLIST: {
		m_playlist = (int)ev.param;
		BDNAV_TITLE_INFO ti = {};
		m_playlistDur = lib.bdnav_get_playlist_info(m_nav, ev.param, &ti) ? ti.duration : 0;
		msg.Format(L"event PLAYLIST %05u.mpls (%.1f s, %u chapters)", ev.param, m_playlistDur / 90000.0, ti.chapter_count);
		break;
	}
	case BDNAV_EV_POPUP:
		m_popupAvail = (int)ev.param;
		msg.Format(L"event POPUP available=%u", ev.param);
		break;
	case BDNAV_EV_MENU:
		msg.Format(L"event MENU active=%u", ev.param);
		break;
	case BDNAV_EV_STILL:
		msg.Format(L"event STILL %u", ev.param);
		break;
	case BDNAV_EV_END_OF_TITLE:
		msg = L"event END_OF_TITLE";
		break;
	case BDNAV_EV_ERROR:
	case BDNAV_EV_READ_ERROR:
		msg.Format(L"event %s %u", ev.event == BDNAV_EV_ERROR ? L"ERROR" : L"READ_ERROR", ev.param);
		break;
	case BDNAV_EV_ENCRYPTED:
		msg.Format(L"event ENCRYPTED %u", ev.param);
		break;
	default:
		break;
	}

	if (!msg.IsEmpty() && m_logBudget > 0) {
		m_logBudget--;
		CString m;
		m.Format(L"%s (read %.1f MB)", msg.GetString(), m_total / 1048576.0);
		BDNavLog(m);
	}
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
	if (m_popupAvail) {
		lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_POPUP);
		BDNavLog(L"auto menu: main title started without menu -> pop-up menu");
	} else {
		const int r = lib.bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"auto menu: main title started without menu -> top menu"
		           : L"auto menu: top menu call refused by disc");
	}
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
	if (lib.bdnav_lock_plane(m_nav, BDNAV_PLANE_IG, &p)) {
		{
			CAutoLock lock(&m_csPlane);
			if (reason == BDNAV_OV_CLOSE || !p.argb || !p.visible || p.w <= 0 || p.h <= 0) {
				m_planeVisible = false;
				if (p.w > 0 && p.h > 0) {
					m_planeW = p.w;
					m_planeH = p.h;
				}
			} else {
				m_planeW = p.w;
				m_planeH = p.h;
				m_plane.resize((size_t)p.w * p.h);
				for (int y = 0; y < p.h; y++) {
					memcpy(&m_plane[(size_t)y * p.w], p.argb + (size_t)y * p.stride, (size_t)p.w * 4);
				}
				m_planeBox = { p.x0, p.y0, p.x1 + 1, p.y1 + 1 };
				m_planeVisible = true;
			}
			m_planeVersion++;
		}
		lib.bdnav_unlock_plane(m_nav, BDNAV_PLANE_IG);
	}

	if (m_planeVisible && !m_bMenuSeen) {
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

	CAutoLock lock(&m_csNav);
	if (!m_nav) {
		return false;
	}
	auto& lib = CBDNavLib::Get();
	if (!bClick && px == m_lastMouseX && py == m_lastMouseY) {
		return false;
	}
	m_lastMouseX = px;
	m_lastMouseY = py;

	const int r = lib.bdnav_mouse_select(m_nav, -1, px, py);
	if (bClick && r > 0) {
		lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_MOUSE_ACTIVATE);
		CString m;
		m.Format(L"mouse click (%d,%d) -> activate", px, py);
		BDNavLog(m);
	}
	return r > 0;
}

void CBDNavigator::Key(uint32_t key)
{
	CAutoLock lock(&m_csNav);
	if (m_nav) {
		CBDNavLib::Get().bdnav_user_input(m_nav, -1, key);
	}
}

void CBDNavigator::MenuCall()
{
	CAutoLock lock(&m_csNav);
	if (m_nav) {
		const int r = CBDNavLib::Get().bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"user: top menu" : L"user: top menu refused by disc");
	}
}

void CBDNavigator::Popup()
{
	CAutoLock lock(&m_csNav);
	if (!m_nav) {
		return;
	}
	auto& lib = CBDNavLib::Get();
	if (m_popupAvail) {
		lib.bdnav_user_input(m_nav, -1, BDNAV_KEY_POPUP);
		BDNavLog(L"user: pop-up menu");
	} else {
		const int r = lib.bdnav_menu_call(m_nav, -1);
		BDNavLog(r ? L"user: pop-up n/a -> top menu" : L"user: top menu refused by disc");
	}
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

	CAutoLock lock(&m_csPlane);
	if (!m_planeVisible || m_plane.empty() || !spd.bits || spd.bpp != 32 || spd.w <= 0 || spd.h <= 0) {
		return false;
	}

	const int sw = m_planeW, sh = m_planeH;
	const RECT sb = m_planeBox;
	// destination rectangle (nearest-neighbour scaling if sizes differ)
	RECT db;
	db.left   = MulDiv(sb.left,   spd.w, sw);
	db.top    = MulDiv(sb.top,    spd.h, sh);
	db.right  = MulDiv(sb.right,  spd.w, sw);
	db.bottom = MulDiv(sb.bottom, spd.h, sh);
	db.right  = std::min<LONG>(db.right, spd.w);
	db.bottom = std::min<LONG>(db.bottom, spd.h);
	if (db.right <= db.left || db.bottom <= db.top) {
		return false;
	}

	const bool same = (sw == spd.w && sh == spd.h);
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
	return true;
}
