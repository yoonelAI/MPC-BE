/*
 * bdnav_api.h - C interface of bdnav.dll (libbluray navigation wrapper for MPC-BE BDMenu)
 *
 * bdnav.dll statically links libbluray (+udfread, freetype, libxml2). MPC-BE loads it at
 * runtime (CBDNavLib); if it is missing or incompatible, MPC-BE falls back to its original
 * Blu-ray playback path.
 *
 * This file is part of MPC-BE BDMenu. License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BDNAV_API_VERSION 6

#ifdef BDNAV_BUILD
#define BDNAV_EXPORT __declspec(dllexport)
#else
#define BDNAV_EXPORT
#endif

typedef struct bdnav_s BDNAV;

typedef struct {
	int  bluray_detected;
	int  no_menu_support;
	int  first_play_supported;
	int  top_menu_supported;
	int  num_titles;
	int  num_hdmv_titles;
	int  num_bdj_titles;
	int  num_unsupported_titles;
	int  bdj_detected;
	int  bdj_supported;
	int  libjvm_detected;
	int  bdj_handled;
	int  aacs_detected;
	int  aacs_handled;
	int  aacs_error_code;
	int  bdplus_detected;
	int  bdplus_handled;
	char disc_name[256];      /* UTF-8, may be empty */
	char udf_volume_id[64];   /* UTF-8, may be empty */
} BDNAV_DISC_INFO;

typedef struct {
	uint32_t idx;
	uint32_t playlist;        /* xxxxx.mpls */
	uint64_t duration;        /* 90 kHz */
	uint32_t clip_count;
	uint32_t angle_count;
	uint32_t chapter_count;
	uint32_t mark_count;
} BDNAV_TITLE_INFO;

/* events: numeric values are identical to libbluray BD_EVENT_* (checked at compile time) */
enum {
	BDNAV_EV_NONE                   = 0,
	BDNAV_EV_ERROR                  = 1,
	BDNAV_EV_READ_ERROR             = 2,
	BDNAV_EV_ENCRYPTED              = 3,
	BDNAV_EV_ANGLE                  = 4,
	BDNAV_EV_TITLE                  = 5,
	BDNAV_EV_PLAYLIST               = 6,
	BDNAV_EV_PLAYITEM               = 7,
	BDNAV_EV_CHAPTER                = 8,
	BDNAV_EV_PLAYMARK               = 9,
	BDNAV_EV_END_OF_TITLE           = 10,
	BDNAV_EV_AUDIO_STREAM           = 11,
	BDNAV_EV_IG_STREAM              = 12,
	BDNAV_EV_PG_TEXTST_STREAM       = 13,
	BDNAV_EV_PIP_PG_TEXTST_STREAM   = 14,
	BDNAV_EV_SECONDARY_AUDIO_STREAM = 15,
	BDNAV_EV_SECONDARY_VIDEO_STREAM = 16,
	BDNAV_EV_PG_TEXTST              = 17,
	BDNAV_EV_PIP_PG_TEXTST          = 18,
	BDNAV_EV_SECONDARY_AUDIO        = 19,
	BDNAV_EV_SECONDARY_VIDEO        = 20,
	BDNAV_EV_SECONDARY_VIDEO_SIZE   = 21,
	BDNAV_EV_PLAYLIST_STOP          = 22,
	BDNAV_EV_DISCONTINUITY          = 23,
	BDNAV_EV_SEEK                   = 24,
	BDNAV_EV_STILL                  = 25,
	BDNAV_EV_STILL_TIME             = 26,
	BDNAV_EV_SOUND_EFFECT           = 27,
	BDNAV_EV_IDLE                   = 28,
	BDNAV_EV_POPUP                  = 29,
	BDNAV_EV_MENU                   = 30,
	BDNAV_EV_STEREOSCOPIC_STATUS    = 31,
	BDNAV_EV_KEY_INTEREST_TABLE     = 32,
	BDNAV_EV_UO_MASK_CHANGED        = 33,
};

typedef struct {
	uint32_t event;   /* BDNAV_EV_* */
	uint32_t param;
} BDNAV_EVENT;

/* keys for bdnav_user_input() (translated to libbluray BD_VK_* inside the dll) */
enum {
	BDNAV_KEY_0 = 0, BDNAV_KEY_1, BDNAV_KEY_2, BDNAV_KEY_3, BDNAV_KEY_4,
	BDNAV_KEY_5, BDNAV_KEY_6, BDNAV_KEY_7, BDNAV_KEY_8, BDNAV_KEY_9,
	BDNAV_KEY_ROOT_MENU      = 10,
	BDNAV_KEY_POPUP          = 11,
	BDNAV_KEY_UP             = 12,
	BDNAV_KEY_DOWN           = 13,
	BDNAV_KEY_LEFT           = 14,
	BDNAV_KEY_RIGHT          = 15,
	BDNAV_KEY_ENTER          = 16,
	BDNAV_KEY_MOUSE_ACTIVATE = 17,
	BDNAV_KEY_RED            = 403,
	BDNAV_KEY_GREEN          = 404,
	BDNAV_KEY_YELLOW         = 405,
	BDNAV_KEY_BLUE           = 406,
};

/* overlay planes */
enum {
	BDNAV_PLANE_PG = 0,
	BDNAV_PLANE_IG = 1,   /* interactive graphics = menus */
};

/* notification reasons passed to the notify callback */
enum {
	BDNAV_OV_INIT  = 1,
	BDNAV_OV_CLOSE = 2,
	BDNAV_OV_FLUSH = 3,   /* plane content changed */
	BDNAV_OV_HIDE  = 4,   /* plane is empty and can be hidden */
};

typedef struct {
	int             w, h;        /* plane size (0 if not initialised) */
	const uint32_t* argb;        /* premultiplied: no; straight ARGB (A in bits 24-31), may be NULL */
	int             stride;      /* in pixels */
	uint32_t        version;     /* incremented on every flush */
	int             visible;     /* 1 if plane has visible pixels and is not hidden */
	int             x0, y0, x1, y1; /* bounding box of visible pixels (inclusive), valid if visible */
	/* v6: area changed since the previous bdnav_unlock_plane() (inclusive; dx1 < dx0 = nothing changed).
	   Pixels outside it are unchanged since the last lock. */
	int             dx0, dy0, dx1, dy1;
	int             argb_source;  /* v6: 1 = BD-J (ARGB overlay), 0 = HDMV (palette overlay) */
} BDNAV_PLANE;

/* v6: configuration for bdnav_open_ex(). All strings UTF-8, NULL / "" = not used. */
typedef struct {
	uint32_t    size;          /* sizeof(BDNAV_CONFIG) */
	const char* java_home;     /* JRE folder (bin\server\jvm.dll). NULL = no BD-J */
	const char* bdj_jar;       /* full path of libbluray-j2se-x.y.z.jar (the awt jar must be next to it) */
	const char* storage_dir;   /* BD-J persistent storage + cache root (created if missing) */
	int         region_mask;   /* 1 A, 2 B, 4 C; 0 = default */
	const char* menu_lang;     /* ISO 639-2, e.g. "chi" */
	const char* audio_lang;
	const char* pg_lang;
	const char* country;       /* ISO 3166-1 alpha-2, e.g. "cn" */
	int         uhd_profile;   /* 1 = report a UHD player (profile 6) even for non-UHD discs; 0 = libbluray default */
	int         reserved[8];
} BDNAV_CONFIG;

/* flags returned by bdnav_read_ex() */
enum {
	BDNAV_READ_POS_JUMP = 1,   /* data does not continue the previous read (playlist change / seek / silent BD-J jump) */
};

/* called from inside bdnav_read/bdnav_user_input/... (decoder thread). Keep it short. */
typedef void (*bdnav_notify_cb)(void* ctx, int plane, int reason);

BDNAV_EXPORT int         bdnav_api_version(void);
BDNAV_EXPORT const char* bdnav_lib_version(void);          /* "libbluray x.y.z" */
BDNAV_EXPORT int         bdnav_get_log(char* buf, int len, int clear); /* last libbluray log lines */
BDNAV_EXPORT void        bdnav_set_debug(int level);       /* 0 = errors only, 1 = verbose libbluray log */

BDNAV_EXPORT BDNAV*      bdnav_open(const char* utf8_path, char* err, int errlen); /* folder (disc root) or .iso */
BDNAV_EXPORT void        bdnav_close(BDNAV* nav);
BDNAV_EXPORT int         bdnav_get_disc_info(BDNAV* nav, BDNAV_DISC_INFO* info);
BDNAV_EXPORT void        bdnav_set_player(BDNAV* nav, int region_mask, const char* menu_lang, const char* audio_lang, const char* pg_lang);
BDNAV_EXPORT void        bdnav_set_notify(BDNAV* nav, bdnav_notify_cb cb, void* ctx);

BDNAV_EXPORT int         bdnav_play(BDNAV* nav);                       /* start navigation (First Play) */
BDNAV_EXPORT int         bdnav_read(BDNAV* nav, uint8_t* buf, int len, BDNAV_EVENT* ev); /* bytes (0..len) or <0 */
BDNAV_EXPORT int         bdnav_get_event(BDNAV* nav, BDNAV_EVENT* ev); /* 1 = got event */

BDNAV_EXPORT int         bdnav_user_input(BDNAV* nav, int64_t pts90k, uint32_t key);
BDNAV_EXPORT int         bdnav_mouse_select(BDNAV* nav, int64_t pts90k, int x, int y);
BDNAV_EXPORT int         bdnav_menu_call(BDNAV* nav, int64_t pts90k);
BDNAV_EXPORT void        bdnav_set_scr(BDNAV* nav, int64_t pts90k);

BDNAV_EXPORT uint64_t    bdnav_tell_time(BDNAV* nav);                  /* 90 kHz */
BDNAV_EXPORT int64_t     bdnav_seek_time(BDNAV* nav, uint64_t tick90k);   /* seek in current title, returns new position or <0 */
BDNAV_EXPORT int64_t     bdnav_seek_chapter(BDNAV* nav, uint32_t chapter);
BDNAV_EXPORT uint32_t    bdnav_get_current_chapter(BDNAV* nav);
BDNAV_EXPORT int         bdnav_get_playlist_info(BDNAV* nav, uint32_t playlist, BDNAV_TITLE_INFO* info);
BDNAV_EXPORT int         bdnav_get_clip_info(BDNAV* nav, uint32_t playlist, uint32_t item, char* clip_id, int idlen, char* desc, int desclen); /* clip_id "00001" + stream summary */
BDNAV_EXPORT int         bdnav_get_chapters(BDNAV* nav, uint32_t playlist, uint64_t* starts90k, int max); /* chapter count */
BDNAV_EXPORT int         bdnav_get_stream_pid(BDNAV* nav, uint32_t playlist, uint32_t item, int kind, int number); /* kind 0 audio, 1 PG; number 1..N; pid or -1 */
BDNAV_EXPORT int         bdnav_read_file(BDNAV* nav, const char* path, uint8_t* buf, int buflen); /* path relative to disc root; bytes or -1 */

/* v6 */
BDNAV_EXPORT BDNAV*      bdnav_open_ex(const char* utf8_path, const BDNAV_CONFIG* cfg, char* err, int errlen);
BDNAV_EXPORT int         bdnav_read_ex(BDNAV* nav, uint8_t* buf, int len, BDNAV_EVENT* ev, uint32_t* flags);
BDNAV_EXPORT int         bdnav_get_title_kind(BDNAV* nav, uint32_t title); /* 0 HDMV, 1 BD-J, -1 unknown; 0 = top menu, 0xFFFF = first play */
BDNAV_EXPORT int         bdnav_bdj_started(BDNAV* nav);   /* 1 if a Java VM was created in this process */

/* plane access: always pair with bdnav_unlock_plane() when lock returned 1 */
BDNAV_EXPORT int         bdnav_lock_plane(BDNAV* nav, int plane, BDNAV_PLANE* out);
BDNAV_EXPORT void        bdnav_unlock_plane(BDNAV* nav, int plane);

#ifdef __cplusplus
}
#endif
