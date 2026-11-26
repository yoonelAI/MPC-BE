/*
 * bdnavtest.c - P2-1 test of bdnav.dll API v6 (BD-J via bundled JRE, ARGB planes, dirty rectangles)
 *
 * usage: bdnavtest <disc|iso> <outdir> <java_home> <jar> [seconds]
 *   - opens the disc with bdnav_open_ex (JRE, jar, storage in <outdir>\storage)
 *   - real-time paced reading with bdnav_read_ex, scripted keys
 *   - keeps a host copy of the IG plane updated ONLY through the reported dirty rectangles
 *     (same rule as CBDNavigator) and compares it with the full plane after every flush
 *   - saves the host copy as BMP (what the player would show)
 * License: GPLv3 (same as MPC-BE)
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include "bdnav_api.h"

static BDNAV* g_nav;
static DWORD g_t0;
static double now_s(void) { return (GetTickCount() - g_t0) / 1000.0; }

/* host copy */
static uint32_t* g_copy;
static int g_w, g_h, g_synced;
static long g_flushes, g_partial, g_full, g_mismatch_frames, g_empty_dirty, g_argb_flushes;
static long long g_copied_px;
static int g_visible, g_changed;
static const char* g_ev_names[] = {
	"NONE","ERROR","READ_ERROR","ENCRYPTED","ANGLE","TITLE","PLAYLIST","PLAYITEM","CHAPTER","PLAYMARK",
	"END_OF_TITLE","AUDIO_STREAM","IG_STREAM","PG_TEXTST_STREAM","PIP_PG_TEXTST_STREAM","SEC_AUDIO_STREAM",
	"SEC_VIDEO_STREAM","PG_TEXTST","PIP_PG_TEXTST","SEC_AUDIO","SEC_VIDEO","SEC_VIDEO_SIZE","PLAYLIST_STOP",
	"DISCONTINUITY","SEEK","STILL","STILL_TIME","SOUND_EFFECT","IDLE","POPUP","MENU","STEREO","KEY_INTEREST","UO_MASK" };

static void notify_cb(void* ctx, int plane, int reason)
{
	(void)ctx;
	if (plane != BDNAV_PLANE_IG) return;
	BDNAV_PLANE p;
	memset(&p, 0, sizeof(p));
	if (!bdnav_lock_plane(g_nav, BDNAV_PLANE_IG, &p)) return;
	if (reason == BDNAV_OV_FLUSH && p.argb && p.visible && p.w > 0 && p.h > 0) {
		g_flushes++;
		if (p.argb_source) g_argb_flushes++;
		const size_t n = (size_t)p.w * p.h;
		int partial = g_synced && g_w == p.w && g_h == p.h && g_copy;
		if (!g_copy || g_w != p.w || g_h != p.h) {
			free(g_copy);
			g_copy = (uint32_t*)calloc(n, 4);
			g_w = p.w; g_h = p.h;
		}
		int x0 = 0, y0 = 0, x1 = p.w - 1, y1 = p.h - 1;
		if (partial) {
			x0 = p.dx0; y0 = p.dy0; x1 = p.dx1; y1 = p.dy1;
			if (x1 < x0) g_empty_dirty++;
			g_partial++;
		} else g_full++;
		for (int y = y0; y <= y1 && x1 >= x0; y++)
			memcpy(g_copy + (size_t)y * p.w + x0, p.argb + (size_t)y * p.stride + x0, (size_t)(x1 - x0 + 1) * 4);
		if (x1 >= x0) g_copied_px += (long long)(x1 - x0 + 1) * (y1 - y0 + 1);
		/* verify against the full plane */
		long bad = 0;
		for (int y = 0; y < p.h; y++)
			if (memcmp(g_copy + (size_t)y * p.w, p.argb + (size_t)y * p.stride, (size_t)p.w * 4)) bad++;
		if (bad) {
			g_mismatch_frames++;
			if (g_mismatch_frames <= 10)
				printf("[MISMATCH %7.2f] flush #%ld: %ld rows differ (dirty %d,%d-%d,%d partial=%d)\n", now_s(), g_flushes, bad, p.dx0, p.dy0, p.dx1, p.dy1, partial);
		}
		if (g_flushes <= 8 || g_flushes % 40 == 0)
			printf("[flush %7.2f] #%ld %s bbox=(%d,%d)-(%d,%d) dirty=(%d,%d)-(%d,%d) %s\n", now_s(), g_flushes, p.argb_source ? "ARGB" : "HDMV",
			       p.x0, p.y0, p.x1, p.y1, p.dx0, p.dy0, p.dx1, p.dy1, partial ? "partial" : "full");
		g_synced = 1;
		g_visible = 1;
		g_changed = 1;
	} else {
		g_synced = 0;
		if (reason == BDNAV_OV_FLUSH || reason == BDNAV_OV_HIDE) { g_visible = 0; g_changed = 1; }
	}
	bdnav_unlock_plane(g_nav, BDNAV_PLANE_IG);
}

static wchar_t g_out[MAX_PATH];
static int g_saved;

static void save_shot(const char* tag)
{
	if (!g_copy || g_saved >= 60) return;
	const int W = 960, H = 540;
	int stride = (W * 3 + 3) & ~3;
	uint8_t* img = (uint8_t*)malloc((size_t)stride * H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t c = g_visible ? g_copy[(size_t)(y * g_h / H) * g_w + (x * g_w / W)] : 0;
			unsigned a = c >> 24, bg = 0x30;
			uint8_t* o = img + (size_t)(H - 1 - y) * stride + x * 3;
			o[0] = (uint8_t)(((c & 0xff) * a + bg * (255 - a)) / 255);
			o[1] = (uint8_t)((((c >> 8) & 0xff) * a + bg * (255 - a)) / 255);
			o[2] = (uint8_t)((((c >> 16) & 0xff) * a + bg * (255 - a)) / 255);
		}
	wchar_t path[MAX_PATH];
	swprintf(path, MAX_PATH, L"%ls\\shot_%02d_%05.1fs_%hs.bmp", g_out, g_saved, now_s(), tag);
	FILE* f = _wfopen(path, L"wb");
	if (f) {
		uint8_t hdr[54] = { 0 };
		uint32_t ds = (uint32_t)stride * H;
		hdr[0] = 'B'; hdr[1] = 'M'; *(uint32_t*)(hdr + 2) = 54 + ds; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
		*(int32_t*)(hdr + 18) = W; *(int32_t*)(hdr + 22) = H; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24; *(uint32_t*)(hdr + 34) = ds;
		fwrite(hdr, 1, 54, f); fwrite(img, 1, ds, f); fclose(f);
		printf("[shot %7.2f] #%02d %s visible=%d\n", now_s(), g_saved, tag, g_visible);
		g_saved++;
	}
	free(img);
}

static char* w2u8(const wchar_t* w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char* s = (char*)malloc(n);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}

static void dump_log(void)
{
	static char buf[65536];
	int n = bdnav_get_log(buf, sizeof(buf), 1);
	if (n > 0) {
		char* p = buf;
		while (*p) {
			char* e = strchr(p, '\n');
			if (e) *e = 0;
			if (*p && !strstr(p, "jni,resolve")) printf("[lib %7.2f] %s\n", now_s(), p);
			if (!e) break;
			p = e + 1;
		}
	}
}

static DWORD WINAPI watchdog(LPVOID arg)
{
	Sleep((DWORD)(uintptr_t)arg);
	printf("\nWATCHDOG: no finish (hang?) -> terminate\nSUMMARY: watchdog=yes flushes=%ld mismatch_frames=%ld\n", g_flushes, g_mismatch_frames);
	fflush(stdout);
	TerminateProcess(GetCurrentProcess(), 9);
	return 0;
}

typedef struct { double t; uint32_t key; const char* tag; } ACT;

int wmain(int argc, wchar_t** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 5) { printf("usage: bdnavtest <disc|iso> <outdir> <java_home> <jar> [seconds]\n"); return 1; }
	g_t0 = GetTickCount();
	int secs = argc > 5 ? _wtoi(argv[5]) : 120;
	if (secs < 30) secs = 30;
	CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)((secs + 60) * 1000), 0, NULL);
	if (!_wfullpath(g_out, argv[2], MAX_PATH)) wcsncpy(g_out, argv[2], MAX_PATH - 1);
	CreateDirectoryW(g_out, NULL);
	wchar_t st[MAX_PATH];
	swprintf(st, MAX_PATH, L"%ls\\storage", g_out);

	printf("bdnavtest: api %d, %s\n", bdnav_api_version(), bdnav_lib_version());
	if (bdnav_api_version() != BDNAV_API_VERSION) { printf("RESULT: FAIL api version\n"); return 2; }
	bdnav_set_debug(2);

	BDNAV_CONFIG cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.size = sizeof(cfg);
	char* jh = w2u8(argv[3]); char* jar = w2u8(argv[4]); char* st8 = w2u8(st); char* disc = w2u8(argv[1]);
	cfg.java_home = jh; cfg.bdj_jar = jar; cfg.storage_dir = st8;
	cfg.menu_lang = "eng"; cfg.audio_lang = "eng"; cfg.pg_lang = "chi"; cfg.country = "us"; cfg.region_mask = 1;
	char err[4096] = { 0 };
	g_nav = bdnav_open_ex(disc, &cfg, err, sizeof(err));
	dump_log();
	if (!g_nav) { printf("RESULT: FAIL open: %s\n", err); return 3; }
	BDNAV_DISC_INFO di;
	bdnav_get_disc_info(g_nav, &di);
	printf("[info] titles=%d hdmv=%d bdj=%d bdj_detected=%d libjvm=%d bdj_handled=%d first_play=%d top_menu=%d name=%s\n",
	       di.num_titles, di.num_hdmv_titles, di.num_bdj_titles, di.bdj_detected, di.libjvm_detected, di.bdj_handled,
	       di.first_play_supported, di.top_menu_supported, di.disc_name);
	printf("[info] kind first_play=%d top_menu=%d title1=%d\n", bdnav_get_title_kind(g_nav, 0xFFFF), bdnav_get_title_kind(g_nav, 0), bdnav_get_title_kind(g_nav, 1));
	bdnav_set_notify(g_nav, notify_cb, NULL);
	if (!bdnav_play(g_nav)) { dump_log(); printf("RESULT: FAIL play\n"); return 4; }
	printf("[%7.2f] play ok, jvm=%d\n", now_s(), bdnav_bdj_started(g_nav));

	ACT acts[] = {
		{ 45, BDNAV_KEY_DOWN, "DOWN" }, { 48, BDNAV_KEY_UP, "UP" }, { 51, BDNAV_KEY_RIGHT, "RIGHT" }, { 54, BDNAV_KEY_LEFT, "LEFT" },
		{ 60, BDNAV_KEY_ENTER, "ENTER" }, { 75, BDNAV_KEY_RIGHT, "RIGHT2" }, { 78, BDNAV_KEY_ENTER, "ENTER2" },
		{ 95, BDNAV_KEY_POPUP, "POPUP" }, { 100, BDNAV_KEY_DOWN, "popup_DOWN" }, { 105, BDNAV_KEY_POPUP, "POPUP_off" },
		{ 110, BDNAV_KEY_ROOT_MENU, "ROOT_MENU" },
	};
	int nacts = sizeof(acts) / sizeof(acts[0]), ai = 0;
	static uint8_t buf[6144 * 16];
	double anchor_w = -1, shot_at = -1, next_stat = 5, last_auto = 0;
	uint64_t anchor_t = 0, total = 0;
	long jumps = 0, still_time = 0, idle = 0;
	const char* shot_tag = "";

	while (now_s() < secs) {
		BDNAV_EVENT ev;
		uint32_t flags = 0;
		int r = bdnav_read_ex(g_nav, buf, sizeof(buf), &ev, &flags);
		if (r < 0) { printf("[%7.2f] read error %d\n", now_s(), r); Sleep(50); }
		if (r > 0) total += (unsigned)r;
		if (flags & BDNAV_READ_POS_JUMP) { jumps++; if (jumps <= 30) printf("[jump %7.2f] position jump (event %s)\n", now_s(), ev.event < 34 ? g_ev_names[ev.event] : "?"); }
		for (int guard = 0; ev.event != BDNAV_EV_NONE && guard < 64; guard++) {
			if (ev.event == BDNAV_EV_STILL_TIME) still_time++;
			else if (ev.event == BDNAV_EV_IDLE) idle++;
			else if (ev.event != BDNAV_EV_PLAYMARK) {
				printf("[ev %7.2f] %-16s %u (%.1f MB)", now_s(), ev.event < 34 ? g_ev_names[ev.event] : "?", ev.param, total / 1048576.0);
				if (ev.event == BDNAV_EV_TITLE) printf(" kind=%d", bdnav_get_title_kind(g_nav, ev.param));
				printf("\n");
			}
			if (ev.event == BDNAV_EV_PLAYLIST || ev.event == BDNAV_EV_SEEK) anchor_w = -1;
			if (!bdnav_get_event(g_nav, &ev)) break;
		}
		if (r > 0) {
			uint64_t t = bdnav_tell_time(g_nav);
			double w = now_s();
			if (anchor_w < 0 || t < anchor_t) { anchor_w = w; anchor_t = t; }
			double ahead = (double)(t - anchor_t) / 90000.0 - (w - anchor_w);
			if (ahead > 8.0) { anchor_w = w; anchor_t = t; }
			else if (ahead > 0.5) Sleep((DWORD)((ahead - 0.4) * 1000));
		} else Sleep(20);

		if (ai < nacts && now_s() >= acts[ai].t) {
			printf("[act %7.2f] %s -> %d\n", now_s(), acts[ai].tag, bdnav_user_input(g_nav, -1, acts[ai].key));
			shot_at = now_s() + 1.5; shot_tag = acts[ai].tag; ai++;
		}
		if (shot_at > 0 && now_s() >= shot_at) { save_shot(shot_tag); shot_at = -1; g_changed = 0; last_auto = now_s(); }
		else if (g_changed && now_s() - last_auto > 3) { save_shot("auto"); g_changed = 0; last_auto = now_s(); }
		if (now_s() >= next_stat) {
			next_stat += 10;
			dump_log();
			printf("[stat %7.2f] read=%.1f MB t=%.1fs flushes=%ld (argb %ld) partial=%ld full=%ld copied=%.1f Mpx mismatch=%ld still_time=%ld idle=%ld jvm=%d\n",
			       now_s(), total / 1048576.0, bdnav_tell_time(g_nav) / 90000.0, g_flushes, g_argb_flushes, g_partial, g_full,
			       g_copied_px / 1e6, g_mismatch_frames, still_time, idle, bdnav_bdj_started(g_nav));
		}
	}
	dump_log();
	printf("SUMMARY: flushes=%ld argb=%ld partial=%ld full=%ld empty_dirty=%ld copied_Mpx=%.1f full_equiv_Mpx=%.1f mismatch_frames=%ld jumps=%ld jvm=%d read=%.0fMB shots=%d\n",
	       g_flushes, g_argb_flushes, g_partial, g_full, g_empty_dirty, g_copied_px / 1e6, g_flushes * (double)g_w * g_h / 1e6, g_mismatch_frames, jumps,
	       bdnav_bdj_started(g_nav), total / 1048576.0, g_saved);
	printf("RESULT: %s\n", g_flushes > 0 && g_mismatch_frames == 0 ? "OK" : (g_flushes == 0 ? "NO MENU GRAPHICS" : "MISMATCH"));
	bdnav_set_notify(g_nav, NULL, NULL);
	bdnav_close(g_nav);
	dump_log();
	printf("[%7.2f] closed\n", now_s());
	return 0;
}
