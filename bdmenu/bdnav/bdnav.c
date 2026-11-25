/*
 * bdnav.c - libbluray navigation wrapper (bdnav.dll) for MPC-BE BDMenu
 * Build: cl /MT /LD /DBDNAV_BUILD ... (see .github/workflows/main.yml)
 * License: GPLv3 (same as MPC-BE)
 */

#define _CRT_SECURE_NO_WARNINGS
#ifndef BDNAV_BUILD
#define BDNAV_BUILD
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libbluray/bluray.h>
#include <libbluray/overlay.h>
#include <libbluray/keys.h>
#include <libbluray/log_control.h>

#include "bdnav_api.h"

/* compile-time check: BDNAV_EV_* must equal BD_EVENT_* */
#define BDNAV_CHECK(name, a, b) typedef char name[((int)(a) == (int)(b)) ? 1 : -1]
BDNAV_CHECK(chk_ev_title,    BD_EVENT_TITLE,           BDNAV_EV_TITLE);
BDNAV_CHECK(chk_ev_playlist, BD_EVENT_PLAYLIST,        BDNAV_EV_PLAYLIST);
BDNAV_CHECK(chk_ev_eot,      BD_EVENT_END_OF_TITLE,    BDNAV_EV_END_OF_TITLE);
BDNAV_CHECK(chk_ev_ig,       BD_EVENT_IG_STREAM,       BDNAV_EV_IG_STREAM);
BDNAV_CHECK(chk_ev_pls_stop, BD_EVENT_PLAYLIST_STOP,   BDNAV_EV_PLAYLIST_STOP);
BDNAV_CHECK(chk_ev_disc,     BD_EVENT_DISCONTINUITY,   BDNAV_EV_DISCONTINUITY);
BDNAV_CHECK(chk_ev_seek,     BD_EVENT_SEEK,            BDNAV_EV_SEEK);
BDNAV_CHECK(chk_ev_still,    BD_EVENT_STILL,           BDNAV_EV_STILL);
BDNAV_CHECK(chk_ev_stilltm,  BD_EVENT_STILL_TIME,      BDNAV_EV_STILL_TIME);
BDNAV_CHECK(chk_ev_idle,     BD_EVENT_IDLE,            BDNAV_EV_IDLE);
BDNAV_CHECK(chk_ev_popup,    BD_EVENT_POPUP,           BDNAV_EV_POPUP);
BDNAV_CHECK(chk_ev_menu,     BD_EVENT_MENU,            BDNAV_EV_MENU);
BDNAV_CHECK(chk_ev_uo,       BD_EVENT_UO_MASK_CHANGED, BDNAV_EV_UO_MASK_CHANGED);

/* ---------------- libbluray log capture ---------------- */

static CRITICAL_SECTION g_log_cs;
static char   g_log[65536];
static size_t g_log_len;

static void log_cb(const char* msg)
{
	if (!msg) return;
	EnterCriticalSection(&g_log_cs);
	size_t n = strlen(msg);
	const size_t cap = sizeof(g_log) - 1;
	if (n > cap) { msg += n - cap; n = cap; }
	if (g_log_len + n > cap) {
		size_t drop = g_log_len + n - cap;
		memmove(g_log, g_log + drop, g_log_len - drop);
		g_log_len -= drop;
	}
	memcpy(g_log + g_log_len, msg, n);
	g_log_len += n;
	g_log[g_log_len] = 0;
	LeaveCriticalSection(&g_log_cs);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_log_cs);
		DisableThreadLibraryCalls(hinst);
		bd_set_debug_handler(log_cb);
		bd_set_debug_mask(DBG_CRIT);
	} else if (reason == DLL_PROCESS_DETACH) {
		bd_set_debug_handler(NULL);
		DeleteCriticalSection(&g_log_cs);
	}
	return TRUE;
}

BDNAV_EXPORT int bdnav_get_log(char* buf, int len, int clear)
{
	if (!buf || len <= 0) return 0;
	EnterCriticalSection(&g_log_cs);
	size_t n = g_log_len < (size_t)(len - 1) ? g_log_len : (size_t)(len - 1);
	memcpy(buf, g_log + (g_log_len - n), n);
	buf[n] = 0;
	if (clear) { g_log_len = 0; g_log[0] = 0; }
	LeaveCriticalSection(&g_log_cs);
	return (int)n;
}

/* ---------------- handle ---------------- */

typedef struct {
	int       w, h;
	uint32_t* px;
	uint32_t  version;
	int       hidden;
	int       visible;
	int       x0, y0, x1, y1;
} PLANE;

struct bdnav_s {
	BLURAY*          bd;
	CRITICAL_SECTION cs;
	PLANE            pl[2];
	bdnav_notify_cb  notify;
	void*            notify_ctx;
};

BDNAV_EXPORT int bdnav_api_version(void) { return BDNAV_API_VERSION; }

BDNAV_EXPORT void bdnav_set_debug(int level)
{
	bd_set_debug_mask(level > 0 ? (DBG_CRIT | DBG_HDMV | DBG_GC | DBG_NAV | DBG_BLURAY) : DBG_CRIT);
}

BDNAV_EXPORT const char* bdnav_lib_version(void)
{
	static char ver[64];
	int maj = 0, min = 0, mic = 0;
	bd_get_version(&maj, &min, &mic);
	_snprintf(ver, sizeof(ver) - 1, "libbluray %d.%d.%d", maj, min, mic);
	return ver;
}

static uint32_t ycc2argb(const BD_PG_PALETTE_ENTRY* e)
{
	double Y  = (e->Y  - 16)  * 255.0 / 219.0;
	double Cb = (e->Cb - 128) * 255.0 / 224.0;
	double Cr = (e->Cr - 128) * 255.0 / 224.0;
	double r = Y + 1.5748 * Cr, g = Y - 0.1873 * Cb - 0.4681 * Cr, b = Y + 1.8556 * Cb;
	int R = r < 0 ? 0 : r > 255 ? 255 : (int)(r + 0.5);
	int G = g < 0 ? 0 : g > 255 ? 255 : (int)(g + 0.5);
	int B = b < 0 ? 0 : b > 255 ? 255 : (int)(b + 0.5);
	return ((uint32_t)e->T << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static void plane_bbox(PLANE* pl)
{
	int x0 = pl->w, y0 = pl->h, x1 = -1, y1 = -1;
	for (int y = 0; y < pl->h; y++) {
		const uint32_t* row = pl->px + (size_t)y * pl->w;
		for (int x = 0; x < pl->w; x++) {
			if (row[x] >> 24) {
				if (x < x0) x0 = x;
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
				if (y > y1) y1 = y;
			}
		}
	}
	pl->x0 = x0; pl->y0 = y0; pl->x1 = x1; pl->y1 = y1;
	pl->visible = (x1 >= 0) && !pl->hidden;
}

static void overlay_cb(void* handle, const BD_OVERLAY* const ov)
{
	BDNAV* nav = (BDNAV*)handle;
	int reason = 0, plane = -1;

	if (!ov) {
		EnterCriticalSection(&nav->cs);
		for (int i = 0; i < 2; i++) {
			free(nav->pl[i].px);
			memset(&nav->pl[i], 0, sizeof(PLANE));
		}
		LeaveCriticalSection(&nav->cs);
		if (nav->notify) {
			nav->notify(nav->notify_ctx, BDNAV_PLANE_PG, BDNAV_OV_CLOSE);
			nav->notify(nav->notify_ctx, BDNAV_PLANE_IG, BDNAV_OV_CLOSE);
		}
		return;
	}
	if (ov->plane > 1) return;
	plane = ov->plane;

	EnterCriticalSection(&nav->cs);
	PLANE* pl = &nav->pl[plane];
	switch (ov->cmd) {
	case BD_OVERLAY_INIT:
		free(pl->px);
		memset(pl, 0, sizeof(PLANE));
		pl->w = ov->w; pl->h = ov->h;
		pl->px = (uint32_t*)calloc((size_t)ov->w * ov->h, 4);
		reason = BDNAV_OV_INIT;
		break;
	case BD_OVERLAY_CLOSE:
		free(pl->px);
		memset(pl, 0, sizeof(PLANE));
		reason = BDNAV_OV_CLOSE;
		break;
	case BD_OVERLAY_CLEAR:
		if (pl->px) memset(pl->px, 0, (size_t)pl->w * pl->h * 4);
		break;
	case BD_OVERLAY_HIDE:
		pl->hidden = 1;
		pl->visible = 0;
		pl->version++;
		reason = BDNAV_OV_HIDE;
		break;
	case BD_OVERLAY_WIPE:
		if (pl->px) {
			for (int y = ov->y; y < ov->y + ov->h && y < pl->h; y++)
				for (int x = ov->x; x < ov->x + ov->w && x < pl->w; x++)
					if (x >= 0 && y >= 0) pl->px[(size_t)y * pl->w + x] = 0;
		}
		break;
	case BD_OVERLAY_DRAW:
		if (pl->px && ov->img && ov->palette) {
			uint32_t pal[256];
			for (int i = 0; i < 256; i++) pal[i] = ycc2argb(&ov->palette[i]);
			const BD_PG_RLE_ELEM* r = ov->img;
			for (int y = 0; y < ov->h; y++) {
				int x = 0;
				while (x < ov->w) {
					if (r->len == 0) { r++; break; }
					int py = ov->y + y;
					for (int k = 0; k < r->len && x + k < ov->w; k++) {
						int px = ov->x + x + k;
						if (px >= 0 && px < pl->w && py >= 0 && py < pl->h)
							pl->px[(size_t)py * pl->w + px] = pal[r->color & 0xff];
					}
					x += r->len;
					r++;
				}
				if (x >= ov->w && r->len == 0) r++;
			}
		}
		break;
	case BD_OVERLAY_FLUSH:
		pl->hidden = 0;
		if (pl->px) plane_bbox(pl);
		pl->version++;
		reason = BDNAV_OV_FLUSH;
		break;
	default:
		break;
	}
	LeaveCriticalSection(&nav->cs);

	if (reason && nav->notify) nav->notify(nav->notify_ctx, plane, reason);
}

BDNAV_EXPORT BDNAV* bdnav_open(const char* utf8_path, char* err, int errlen)
{
	if (err && errlen > 0) err[0] = 0;
	if (!utf8_path) return NULL;

	BLURAY* bd = bd_open(utf8_path, NULL);
	if (!bd) {
		if (err && errlen > 0) bdnav_get_log(err, errlen, 0);
		return NULL;
	}
	BDNAV* nav = (BDNAV*)calloc(1, sizeof(BDNAV));
	if (!nav) { bd_close(bd); return NULL; }
	nav->bd = bd;
	InitializeCriticalSection(&nav->cs);
	bd_register_overlay_proc(bd, nav, overlay_cb);
	return nav;
}

BDNAV_EXPORT void bdnav_close(BDNAV* nav)
{
	if (!nav) return;
	nav->notify = NULL;
	bd_register_overlay_proc(nav->bd, NULL, NULL);
	bd_close(nav->bd);
	for (int i = 0; i < 2; i++) free(nav->pl[i].px);
	DeleteCriticalSection(&nav->cs);
	free(nav);
}

static void copy_str(char* dst, size_t cap, const char* src)
{
	if (!dst || !cap) return;
	dst[0] = 0;
	if (src) { strncpy(dst, src, cap - 1); dst[cap - 1] = 0; }
}

BDNAV_EXPORT int bdnav_get_disc_info(BDNAV* nav, BDNAV_DISC_INFO* info)
{
	if (!nav || !info) return 0;
	memset(info, 0, sizeof(*info));
	const BLURAY_DISC_INFO* di = bd_get_disc_info(nav->bd);
	if (!di) return 0;
	info->bluray_detected        = di->bluray_detected;
	info->no_menu_support        = di->no_menu_support;
	info->first_play_supported   = di->first_play_supported;
	info->top_menu_supported     = di->top_menu_supported;
	info->num_titles             = (int)di->num_titles;
	info->num_hdmv_titles        = (int)di->num_hdmv_titles;
	info->num_bdj_titles         = (int)di->num_bdj_titles;
	info->num_unsupported_titles = (int)di->num_unsupported_titles;
	info->bdj_detected           = di->bdj_detected;
	info->bdj_supported          = di->bdj_supported;
	info->libjvm_detected        = di->libjvm_detected;
	info->bdj_handled            = di->bdj_handled;
	info->aacs_detected          = di->aacs_detected;
	info->aacs_handled           = di->aacs_handled;
	info->aacs_error_code        = di->aacs_error_code;
	info->bdplus_detected        = di->bdplus_detected;
	info->bdplus_handled         = di->bdplus_handled;
	copy_str(info->disc_name, sizeof(info->disc_name), di->disc_name);
	copy_str(info->udf_volume_id, sizeof(info->udf_volume_id), di->udf_volume_id);
	return 1;
}

BDNAV_EXPORT void bdnav_set_player(BDNAV* nav, int region_mask, const char* menu_lang, const char* audio_lang, const char* pg_lang)
{
	if (!nav) return;
	if (region_mask > 0) bd_set_player_setting(nav->bd, BLURAY_PLAYER_SETTING_REGION_CODE, (uint32_t)region_mask);
	if (menu_lang  && *menu_lang)  bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_MENU_LANG, menu_lang);
	if (audio_lang && *audio_lang) bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, audio_lang);
	if (pg_lang    && *pg_lang)    bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_PG_LANG, pg_lang);
}

BDNAV_EXPORT void bdnav_set_notify(BDNAV* nav, bdnav_notify_cb cb, void* ctx)
{
	if (!nav) return;
	EnterCriticalSection(&nav->cs);
	nav->notify = cb;
	nav->notify_ctx = ctx;
	LeaveCriticalSection(&nav->cs);
}

BDNAV_EXPORT int bdnav_play(BDNAV* nav) { return nav ? bd_play(nav->bd) : 0; }

BDNAV_EXPORT int bdnav_read(BDNAV* nav, uint8_t* buf, int len, BDNAV_EVENT* ev)
{
	if (!nav) return -1;
	BD_EVENT e;
	memset(&e, 0, sizeof(e));
	int r = bd_read_ext(nav->bd, buf, len, &e);
	if (ev) { ev->event = e.event; ev->param = e.param; }
	return r;
}

BDNAV_EXPORT int bdnav_get_event(BDNAV* nav, BDNAV_EVENT* ev)
{
	if (!nav) return 0;
	BD_EVENT e;
	memset(&e, 0, sizeof(e));
	int r = bd_get_event(nav->bd, &e);
	if (ev) { ev->event = e.event; ev->param = e.param; }
	return r && e.event != BD_EVENT_NONE;
}

static uint32_t map_key(uint32_t k)
{
	if (k <= 9) return BD_VK_0 + k;
	switch (k) {
	case BDNAV_KEY_ROOT_MENU:      return BD_VK_ROOT_MENU;
	case BDNAV_KEY_POPUP:          return BD_VK_POPUP;
	case BDNAV_KEY_UP:             return BD_VK_UP;
	case BDNAV_KEY_DOWN:           return BD_VK_DOWN;
	case BDNAV_KEY_LEFT:           return BD_VK_LEFT;
	case BDNAV_KEY_RIGHT:          return BD_VK_RIGHT;
	case BDNAV_KEY_ENTER:          return BD_VK_ENTER;
	case BDNAV_KEY_MOUSE_ACTIVATE: return BD_VK_MOUSE_ACTIVATE;
	/* colour keys: numeric values from the BD-ROM spec (VK_COLORED_KEY_0..3);
	   libbluray headers do not name all of them consistently */
	case BDNAV_KEY_RED:            return 403;
	case BDNAV_KEY_GREEN:          return 404;
	case BDNAV_KEY_YELLOW:         return 405;
	case BDNAV_KEY_BLUE:           return 406;
	default:                       return BD_VK_NONE;
	}
}

BDNAV_EXPORT int bdnav_user_input(BDNAV* nav, int64_t pts90k, uint32_t key)
{
	if (!nav) return -1;
	uint32_t k = map_key(key);
	if (k == BD_VK_NONE) return -1;
	return bd_user_input(nav->bd, pts90k, k);
}

BDNAV_EXPORT int bdnav_mouse_select(BDNAV* nav, int64_t pts90k, int x, int y)
{
	if (!nav || x < 0 || y < 0 || x > 0xffff || y > 0xffff) return -1;
	return bd_mouse_select(nav->bd, pts90k, (uint16_t)x, (uint16_t)y);
}

BDNAV_EXPORT int bdnav_menu_call(BDNAV* nav, int64_t pts90k) { return nav ? bd_menu_call(nav->bd, pts90k) : 0; }

BDNAV_EXPORT void bdnav_set_scr(BDNAV* nav, int64_t pts90k) { if (nav) bd_set_scr(nav->bd, pts90k); }

BDNAV_EXPORT uint64_t bdnav_tell_time(BDNAV* nav) { return nav ? bd_tell_time(nav->bd) : 0; }

BDNAV_EXPORT int64_t bdnav_seek_time(BDNAV* nav, uint64_t tick90k) { return nav ? bd_seek_time(nav->bd, tick90k) : -1; }

BDNAV_EXPORT int64_t bdnav_seek_chapter(BDNAV* nav, uint32_t chapter) { return nav ? bd_seek_chapter(nav->bd, chapter) : -1; }

BDNAV_EXPORT uint32_t bdnav_get_current_chapter(BDNAV* nav) { return nav ? bd_get_current_chapter(nav->bd) : 0; }

BDNAV_EXPORT int bdnav_get_playlist_info(BDNAV* nav, uint32_t playlist, BDNAV_TITLE_INFO* info)
{
	if (!nav || !info) return 0;
	memset(info, 0, sizeof(*info));
	BLURAY_TITLE_INFO* ti = bd_get_playlist_info(nav->bd, playlist, 0);
	if (!ti) return 0;
	info->idx           = ti->idx;
	info->playlist      = ti->playlist;
	info->duration      = ti->duration;
	info->clip_count    = ti->clip_count;
	info->angle_count   = ti->angle_count;
	info->chapter_count = ti->chapter_count;
	info->mark_count    = ti->mark_count;
	bd_free_title_info(ti);
	return 1;
}

static const char* coding_name(uint8_t t)
{
	switch (t) {
	case 0x01: return "MPEG1";   case 0x02: return "MPEG2";  case 0x1b: return "H264";
	case 0x20: return "MVC";     case 0x24: return "HEVC";   case 0xea: return "VC1";
	case 0x03: return "MP1A";    case 0x04: return "MP2A";
	case 0x80: return "LPCM";    case 0x81: return "AC3";    case 0x82: return "DTS";
	case 0x83: return "TrueHD";  case 0x84: return "AC3+";   case 0x85: return "DTS-HD";
	case 0x86: return "DTS-HDMA";case 0xa1: return "AC3+sec";case 0xa2: return "DTS-HDsec";
	case 0x90: return "PGS";     case 0x91: return "IGS";    case 0x92: return "TextST";
	default:   return "?";
	}
}

BDNAV_EXPORT int bdnav_get_clip_info(BDNAV* nav, uint32_t playlist, uint32_t item, char* clip_id, int idlen, char* desc, int desclen)
{
	if (clip_id && idlen > 0) clip_id[0] = 0;
	if (desc && desclen > 0) desc[0] = 0;
	if (!nav) return 0;
	BLURAY_TITLE_INFO* ti = bd_get_playlist_info(nav->bd, playlist, 0);
	if (!ti) return 0;
	if (item >= ti->clip_count) { bd_free_title_info(ti); return 0; }
	const BLURAY_CLIP_INFO* ci = &ti->clips[item];
	if (clip_id && idlen > 0) {
		char id[6]; memcpy(id, ci->clip_id, 5); id[5] = 0;
		copy_str(clip_id, (size_t)idlen, id);
	}
	if (desc && desclen > 0) {
		size_t n = 0;
		int k;
		n += _snprintf(desc + n, desclen - 1 - n, "V:");
		for (k = 0; k < ci->video_stream_count && n < (size_t)desclen - 64; k++)
			n += _snprintf(desc + n, desclen - 1 - n, " %04x=%s", ci->video_streams[k].pid, coding_name(ci->video_streams[k].coding_type));
		n += _snprintf(desc + n, desclen - 1 - n, " A:");
		for (k = 0; k < ci->audio_stream_count && n < (size_t)desclen - 64; k++)
			n += _snprintf(desc + n, desclen - 1 - n, " %04x=%s/f%u/r%u/%.3s", ci->audio_streams[k].pid,
				coding_name(ci->audio_streams[k].coding_type), ci->audio_streams[k].format, ci->audio_streams[k].rate,
				(const char*)ci->audio_streams[k].lang);
		n += _snprintf(desc + n, desclen - 1 - n, " PG:%u IG:%u", ci->pg_stream_count, ci->ig_stream_count);
		desc[desclen - 1] = 0;
	}
	bd_free_title_info(ti);
	return 1;
}

BDNAV_EXPORT int bdnav_get_chapters(BDNAV* nav, uint32_t playlist, uint64_t* starts90k, int max)
{
	if (!nav) return 0;
	BLURAY_TITLE_INFO* ti = bd_get_playlist_info(nav->bd, playlist, 0);
	if (!ti) return 0;
	int n = (int)ti->chapter_count;
	if (starts90k) {
		for (int i = 0; i < n && i < max; i++) starts90k[i] = ti->chapters[i].start;
	}
	bd_free_title_info(ti);
	return n;
}

BDNAV_EXPORT int bdnav_get_stream_pid(BDNAV* nav, uint32_t playlist, uint32_t item, int kind, int number)
{
	if (!nav || number < 1) return -1;
	BLURAY_TITLE_INFO* ti = bd_get_playlist_info(nav->bd, playlist, 0);
	if (!ti) return -1;
	int pid = -1;
	if (item < ti->clip_count) {
		const BLURAY_CLIP_INFO* ci = &ti->clips[item];
		if (kind == 0 && number <= ci->audio_stream_count) pid = ci->audio_streams[number - 1].pid;
		if (kind == 1 && number <= ci->pg_stream_count)    pid = ci->pg_streams[number - 1].pid;
	}
	bd_free_title_info(ti);
	return pid;
}

BDNAV_EXPORT int bdnav_read_file(BDNAV* nav, const char* path, uint8_t* buf, int buflen)
{
	if (!nav || !path || !buf || buflen <= 0) return -1;
	void* data = NULL;
	int64_t size = 0;
	if (!bd_read_file(nav->bd, path, &data, &size) || !data) {
		return -1;
	}
	int n = size < (int64_t)buflen ? (int)size : buflen;
	if (n > 0) memcpy(buf, data, (size_t)n);
	free(data);
	return n;
}

BDNAV_EXPORT int bdnav_lock_plane(BDNAV* nav, int plane, BDNAV_PLANE* out)
{
	if (!nav || plane < 0 || plane > 1 || !out) return 0;
	EnterCriticalSection(&nav->cs);
	const PLANE* pl = &nav->pl[plane];
	out->w = pl->w; out->h = pl->h;
	out->argb = pl->px;
	out->stride = pl->w;
	out->version = pl->version;
	out->visible = pl->visible;
	out->x0 = pl->x0; out->y0 = pl->y0; out->x1 = pl->x1; out->y1 = pl->y1;
	return 1;
}

BDNAV_EXPORT void bdnav_unlock_plane(BDNAV* nav, int plane)
{
	(void)plane;
	if (nav) LeaveCriticalSection(&nav->cs);
}
