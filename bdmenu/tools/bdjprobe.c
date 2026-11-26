/*
 * bdjprobe.c - MPC-BE BDMenu P2-0 probe: BD-J menus via libbluray + bundled JRE
 *
 * usage: bdjprobe <disc|iso> <outdir> <region> <java_home> <libbluray-j2se jar> [uhd|default] [seconds]
 *
 * - runs First Play with a real-time paced reader (bd_tell_time vs wall clock)
 * - captures ARGB (BD-J) and YUV (HDMV) overlay planes, saves composites (960x540 BMP)
 * - scripted input: arrow keys, mouse moves/click, ENTER, POPUP
 * - logs libbluray BD-J / JVM messages and all events; watchdog kills a hung JVM
 * License: GPLv3 (same as MPC-BE)
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

#include <libbluray/bluray.h>
#include <libbluray/overlay.h>
#include <libbluray/keys.h>
#include <libbluray/log_control.h>
#include <libbluray/player_settings.h>

typedef struct { int w, h; uint32_t *px; int flushes; int dirty; } PLANE;

static PLANE g_argb[2];     /* BD-J planes: 0 PG, 1 IG */
static PLANE g_yuv[2];      /* HDMV planes (already converted to ARGB) */
static CRITICAL_SECTION g_cs;
static wchar_t g_out[MAX_PATH];
static int g_saved, g_changed;
static DWORD g_t0, g_last_save;
static const char *g_tag = "auto";
static volatile LONG g_lib_lines, g_jvm_created, g_xlet_msgs;
static int g_menu = -1, g_popup = -1, g_idle_events, g_kit = -1;
static unsigned g_uo = 0;
static int g_playlists[64], g_npl;

static double now_s(void) { return (GetTickCount() - g_t0) / 1000.0; }

static void lib_log(const char *msg)
{
    InterlockedIncrement(&g_lib_lines);
    if (strstr(msg, "Created Java VM")) InterlockedExchange(&g_jvm_created, 1);
    if (strstr(msg, "Xlet") || strstr(msg, "xlet")) InterlockedIncrement(&g_xlet_msgs);
    printf("[lib %7.2f] %s", now_s(), msg);
    size_t n = strlen(msg);
    if (!n || msg[n - 1] != '\n') printf("\n");
}

static uint32_t ycc2argb(const BD_PG_PALETTE_ENTRY *e)
{
    double Y = (e->Y - 16) * 255.0 / 219.0, Cb = (e->Cb - 128) * 255.0 / 224.0, Cr = (e->Cr - 128) * 255.0 / 224.0;
    double r = Y + 1.5748 * Cr, g = Y - 0.1873 * Cb - 0.4681 * Cr, b = Y + 1.8556 * Cb;
    int R = r < 0 ? 0 : r > 255 ? 255 : (int)(r + 0.5);
    int G = g < 0 ? 0 : g > 255 ? 255 : (int)(g + 0.5);
    int B = b < 0 ? 0 : b > 255 ? 255 : (int)(b + 0.5);
    return ((uint32_t)e->T << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static void plane_init(PLANE *p, int w, int h)
{
    free(p->px); p->w = w; p->h = h;
    p->px = (uint32_t *)calloc((size_t)w * h, 4);
}

static size_t plane_visible(const PLANE *p)
{
    size_t v = 0;
    if (!p->px) return 0;
    for (size_t i = 0, n = (size_t)p->w * p->h; i < n; i += 7) if (p->px[i] >> 24) v++;
    return v * 7;
}

/* composite all planes over grey, downscale 2x, save 24-bit BMP */
static int save_composite(const char *tag)
{
    const int W = 960, H = 540;
    int stride = (W * 3 + 3) & ~3;
    uint8_t *img = (uint8_t *)malloc((size_t)stride * H);
    if (!img) return 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double r = 0x30, g = 0x30, b = 0x30;
            const PLANE *order[4] = { &g_yuv[0], &g_argb[0], &g_yuv[1], &g_argb[1] };
            for (int k = 0; k < 4; k++) {
                const PLANE *p = order[k];
                if (!p->px || p->w <= 0) continue;
                int sx = x * p->w / W, sy = y * p->h / H;
                uint32_t c = p->px[(size_t)sy * p->w + sx];
                double a = (c >> 24) / 255.0;
                r = r * (1 - a) + ((c >> 16) & 0xff) * a;
                g = g * (1 - a) + ((c >> 8) & 0xff) * a;
                b = b * (1 - a) + (c & 0xff) * a;
            }
            uint8_t *o = img + (size_t)(H - 1 - y) * stride + x * 3;
            o[0] = (uint8_t)b; o[1] = (uint8_t)g; o[2] = (uint8_t)r;
        }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\shot_%02d_%05.1fs_%hs.bmp", g_out, g_saved, now_s(), tag);
    FILE *f = _wfopen(path, L"wb");
    if (!f) { free(img); return 0; }
    uint8_t hdr[54] = { 0 };
    uint32_t datasz = (uint32_t)stride * H;
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2) = 54 + datasz; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = W; *(int32_t *)(hdr + 22) = H; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = datasz;
    fwrite(hdr, 1, 54, f); fwrite(img, 1, datasz, f); fclose(f); free(img);
    printf("[shot %7.2f] #%02d %s argbIG=%zu argbPG=%zu yuvIG=%zu yuvPG=%zu\n", now_s(), g_saved, tag,
           plane_visible(&g_argb[1]), plane_visible(&g_argb[0]), plane_visible(&g_yuv[1]), plane_visible(&g_yuv[0]));
    g_saved++;
    return 1;
}

static void argb_cb(void *h, const BD_ARGB_OVERLAY *const ov)
{
    (void)h;
    if (!ov) { printf("[argb %7.2f] all closed\n", now_s()); return; }
    if (ov->plane > 1) { printf("[argb] plane %d cmd %d ignored\n", ov->plane, ov->cmd); return; }
    PLANE *p = &g_argb[ov->plane];
    EnterCriticalSection(&g_cs);
    switch (ov->cmd) {
    case BD_ARGB_OVERLAY_INIT:
        plane_init(p, ov->w, ov->h);
        printf("[argb %7.2f] plane %d INIT %dx%d at %d,%d\n", now_s(), ov->plane, ov->w, ov->h, ov->x, ov->y);
        break;
    case BD_ARGB_OVERLAY_CLOSE:
        free(p->px); p->px = NULL; p->w = p->h = 0;
        printf("[argb %7.2f] plane %d CLOSE\n", now_s(), ov->plane);
        break;
    case BD_ARGB_OVERLAY_DRAW:
        if (p->px && ov->argb)
            for (int y = 0; y < ov->h; y++) {
                int py = ov->y + y;
                if (py < 0 || py >= p->h) continue;
                for (int x = 0; x < ov->w; x++) {
                    int px = ov->x + x;
                    if (px >= 0 && px < p->w) p->px[(size_t)py * p->w + px] = ov->argb[(size_t)y * ov->stride + x];
                }
            }
        p->dirty = 1;
        break;
    case BD_ARGB_OVERLAY_FLUSH:
        p->flushes++;
        if (p->dirty) g_changed = 1;
        p->dirty = 0;
        if (p->flushes <= 5 || p->flushes % 50 == 0)
            printf("[argb %7.2f] plane %d FLUSH #%d visible=%zu\n", now_s(), ov->plane, p->flushes, plane_visible(p));
        break;
    }
    LeaveCriticalSection(&g_cs);
}

static void yuv_cb(void *h, const BD_OVERLAY *const ov)
{
    (void)h;
    if (!ov || ov->plane > 1) return;
    PLANE *p = &g_yuv[ov->plane];
    EnterCriticalSection(&g_cs);
    switch (ov->cmd) {
    case BD_OVERLAY_INIT: plane_init(p, ov->w, ov->h); printf("[yuv %7.2f] plane %d INIT %dx%d\n", now_s(), ov->plane, ov->w, ov->h); break;
    case BD_OVERLAY_CLOSE: free(p->px); p->px = NULL; p->w = p->h = 0; break;
    case BD_OVERLAY_CLEAR: if (p->px) memset(p->px, 0, (size_t)p->w * p->h * 4); p->dirty = 1; break;
    case BD_OVERLAY_HIDE: if (p->px) memset(p->px, 0, (size_t)p->w * p->h * 4); g_changed = 1; break;
    case BD_OVERLAY_WIPE:
        if (p->px)
            for (int y = ov->y; y < ov->y + ov->h && y < p->h; y++)
                for (int x = ov->x; x < ov->x + ov->w && x < p->w; x++)
                    if (x >= 0 && y >= 0) p->px[(size_t)y * p->w + x] = 0;
        p->dirty = 1;
        break;
    case BD_OVERLAY_DRAW:
        if (p->px && ov->img && ov->palette) {
            uint32_t pal[256];
            for (int i = 0; i < 256; i++) pal[i] = ycc2argb(&ov->palette[i]);
            const BD_PG_RLE_ELEM *r = ov->img;
            for (int y = 0; y < ov->h; y++) {
                int x = 0;
                while (x < ov->w) {
                    if (r->len == 0) { r++; break; }
                    int py = ov->y + y;
                    for (int k = 0; k < r->len && x + k < ov->w; k++) {
                        int px = ov->x + x + k;
                        if (px >= 0 && px < p->w && py >= 0 && py < p->h) p->px[(size_t)py * p->w + px] = pal[r->color & 0xff];
                    }
                    x += r->len; r++;
                }
                if (x >= ov->w && r->len == 0) r++;
            }
        }
        p->dirty = 1;
        break;
    case BD_OVERLAY_FLUSH: p->flushes++; if (p->dirty) g_changed = 1; p->dirty = 0; break;
    default: break;
    }
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    Sleep((DWORD)(uintptr_t)arg);
    printf("\nWATCHDOG: probe did not finish in time (JVM hang?) -> terminate\n");
    printf("SUMMARY: watchdog=yes jvm_created=%ld lib_lines=%ld shots=%d\n", g_jvm_created, g_lib_lines, g_saved);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 9);
    return 0;
}

static char *w2u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void print_title(const char *name, const BLURAY_TITLE *t)
{
    if (!t) { printf("  %s: none\n", name); return; }
    printf("  %s: %s id_ref=%u interactive=%d accessible=%d hidden=%d\n", name, t->bdj ? "BD-J" : "HDMV",
           t->id_ref, t->interactive, t->accessible, t->hidden);
}

static unsigned char g_buf[6144 * 16];

typedef struct { double t; int kind; int a, b; const char *tag; } ACTION;
enum { A_KEY, A_MOUSE, A_CLICK };

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 6) {
        printf("usage: bdjprobe <disc|iso> <outdir> <region> <java_home> <jar> [uhd|default] [seconds]\n");
        return 1;
    }
    InitializeCriticalSection(&g_cs);
    g_t0 = GetTickCount();
    char *disc = w2u8(argv[1]);
    if (!_wfullpath(g_out, argv[2], MAX_PATH)) wcsncpy(g_out, argv[2], MAX_PATH - 1);
    _wmkdir(g_out);
    int region = _wtoi(argv[3]);
    char *jhome = w2u8(argv[4]);
    char *jar = w2u8(argv[5]);
    int uhd = argc > 6 && !_wcsicmp(argv[6], L"uhd");
    int secs = argc > 7 ? _wtoi(argv[7]) : 120;
    if (secs < 30) secs = 30;
    CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)((secs + 60) * 1000), 0, NULL);

    /* JVM / jar location (environment of this process only) */
    _wputenv_s(L"JAVA_HOME", argv[4]);
    _wputenv_s(L"LIBBLURAY_CP", argv[5]);
    wchar_t wp[MAX_PATH];
    swprintf(wp, MAX_PATH, L"%ls\\persist", g_out); _wmkdir(wp);
    char *persist = w2u8(wp);
    swprintf(wp, MAX_PATH, L"%ls\\cache", g_out); _wmkdir(wp);
    char *cache = w2u8(wp);

    bd_set_debug_handler(lib_log);
    bd_set_debug_mask(DBG_CRIT | DBG_BDJ | DBG_BLURAY);

    int maj, min, mic; bd_get_version(&maj, &min, &mic);
    printf("bdjprobe: libbluray %d.%d.%d\ndisc: %s\nregion=%d java_home=%s\njar=%s\nprofile=%s seconds=%d\n",
           maj, min, mic, disc, region, jhome, jar, uhd ? "uhd(6 v3.1)" : "default", secs);

    BLURAY *bd = bd_open(disc, NULL);
    if (!bd) { printf("RESULT: FAIL bd_open\n"); return 2; }
    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    printf("[info] bluray=%d no_menu=%d first_play=%d top_menu=%d titles=%u hdmv=%u bdj=%u unsupported=%u\n",
           di->bluray_detected, di->no_menu_support, di->first_play_supported, di->top_menu_supported,
           di->num_titles, di->num_hdmv_titles, di->num_bdj_titles, di->num_unsupported_titles);
    printf("[info] bdj_detected=%d libjvm_detected=%d bdj_handled=%d org=%s disc_id=%s video_format=%d\n",
           di->bdj_detected, di->libjvm_detected, di->bdj_handled, di->bdj_org_id, di->bdj_disc_id, di->video_format);
    printf("[info] aacs=%d/%d bdplus=%d\n", di->aacs_detected, di->aacs_handled, di->bdplus_detected);
    print_title("first_play", di->first_play);
    print_title("top_menu", di->top_menu);

    bd_set_player_setting_str(bd, BLURAY_PLAYER_JAVA_HOME, jhome);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_PERSISTENT_ROOT, persist);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_CACHE_ROOT, cache);
    bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_REGION_CODE, (uint32_t)region);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_MENU_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_PG_LANG, "chi");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, "us");
    bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_AUDIO_CAP,
        BLURAY_ACAP_LPCM_48_96_SURROUND | BLURAY_ACAP_LPCM_192_SURROUND | BLURAY_ACAP_DDPLUS_SURROUND |
        BLURAY_ACAP_DDPLUS_DEP_SURROUND | BLURAY_ACAP_DTSHD_CORE_SURROUND | BLURAY_ACAP_DTSHD_EXT_SURROUND |
        BLURAY_ACAP_DD_SURROUND | BLURAY_ACAP_MLP_SURROUND);
    if (uhd) bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_6_v3_1);

    bd_register_argb_overlay_proc(bd, NULL, argb_cb, NULL);
    bd_register_overlay_proc(bd, NULL, yuv_cb);
    bd_get_event(bd, NULL);

    printf("[%7.2f] bd_play...\n", now_s());
    if (!bd_play(bd)) { printf("RESULT: FAIL bd_play\n"); return 3; }
    printf("[%7.2f] bd_play ok\n", now_s());

    ACTION acts[] = {
        { 45, A_KEY, BD_VK_DOWN, 0, "DOWN" },  { 48, A_KEY, BD_VK_DOWN, 0, "DOWN2" }, { 51, A_KEY, BD_VK_UP, 0, "UP" },
        { 54, A_KEY, BD_VK_RIGHT, 0, "RIGHT" }, { 57, A_KEY, BD_VK_LEFT, 0, "LEFT" },
        { 60, A_MOUSE, 960, 540, "mouse_c" }, { 62, A_MOUSE, 400, 900, "mouse_bl" }, { 64, A_MOUSE, 1500, 900, "mouse_br" },
        { 66, A_MOUSE, 960, 950, "mouse_bc" }, { 68, A_CLICK, 960, 950, "click_bc" },
        { 78, A_KEY, BD_VK_ENTER, 0, "ENTER" }, { 95, A_KEY, BD_VK_POPUP, 0, "POPUP" },
        { 100, A_KEY, BD_VK_DOWN, 0, "popup_DOWN" }, { 105, A_KEY, BD_VK_POPUP, 0, "POPUP_off" },
        { 110, A_KEY, BD_VK_ROOT_MENU, 0, "ROOT_MENU" },
    };
    int nacts = sizeof(acts) / sizeof(acts[0]), ai = 0;
    double shot_after = -1; const char *shot_tag = "";

    uint64_t total = 0, last_pos = 0;
    double anchor_wall = -1; uint64_t anchor_pts = 0;
    double next_status = 5;
    int reads = 0, zero_reads = 0, jumps = 0;

    while (now_s() < secs) {
        BD_EVENT ev;
        int r = bd_read_ext(bd, g_buf, sizeof(g_buf), &ev);
        if (r < 0) { printf("[%7.2f] read error %d\n", now_s(), r); Sleep(50); }
        if (r > 0) {
            uint64_t pos = bd_tell(bd);
            if (last_pos && pos != last_pos + (uint64_t)r) { jumps++; if (jumps <= 20) printf("[jump %7.2f] pos %llu -> %llu\n", now_s(), last_pos, pos - r); }
            last_pos = pos; total += r; reads++;
        } else zero_reads++;
        for (int guard = 0; ev.event != BD_EVENT_NONE && guard < 64; guard++) {
            if (ev.event != BD_EVENT_PLAYMARK)
                printf("[ev %7.2f] %-16s param=%u (%.1f MB)\n", now_s(), bd_event_name(ev.event), ev.param, total / 1048576.0);
            switch (ev.event) {
            case BD_EVENT_MENU: g_menu = (int)ev.param; g_changed = 1; break;
            case BD_EVENT_POPUP: g_popup = (int)ev.param; break;
            case BD_EVENT_IDLE: g_idle_events++; break;
            case BD_EVENT_KEY_INTEREST_TABLE: g_kit = (int)ev.param; break;
            case BD_EVENT_UO_MASK_CHANGED: g_uo = ev.param; break;
            case BD_EVENT_PLAYLIST: if (g_npl < 64) g_playlists[g_npl++] = (int)ev.param; anchor_wall = -1; break;
            case BD_EVENT_SEEK: case BD_EVENT_DISCONTINUITY: anchor_wall = -1; break;
            default: break;
            }
            if (!bd_get_event(bd, &ev)) break;
        }

        /* real-time pacing: keep stream time ~ wall clock */
        if (r > 0) {
            uint64_t t = bd_tell_time(bd);
            double w = now_s();
            if (anchor_wall < 0 || t < anchor_pts) { anchor_wall = w; anchor_pts = t; }
            double ahead = (double)(t - anchor_pts) / 90000.0 - (w - anchor_wall);
            if (ahead > 8.0) { anchor_wall = w; anchor_pts = t; }        /* big jump: re-anchor */
            else if (ahead > 0.5) Sleep((DWORD)((ahead - 0.4) * 1000));
        } else Sleep(20);   /* idle / still: BD-J runs on its own, poll */

        /* scripted input */
        if (ai < nacts && now_s() >= acts[ai].t) {
            ACTION *a = &acts[ai++];
            printf("[act %7.2f] %s\n", now_s(), a->tag);
            if (a->kind == A_KEY) bd_user_input(bd, -1, (uint32_t)a->a);
            else {
                int rr = bd_mouse_select(bd, -1, (uint16_t)a->a, (uint16_t)a->b);
                printf("[act] mouse_select(%d,%d) -> %d\n", a->a, a->b, rr);
                if (a->kind == A_CLICK) bd_user_input(bd, -1, BD_VK_MOUSE_ACTIVATE);
            }
            shot_after = now_s() + 1.5; shot_tag = a->tag;
        }

        /* screenshots */
        EnterCriticalSection(&g_cs);
        if (g_saved < 70) {
            if (shot_after > 0 && now_s() >= shot_after) { save_composite(shot_tag); shot_after = -1; g_changed = 0; g_last_save = GetTickCount(); }
            else if (g_changed && GetTickCount() - g_last_save > 2000) { save_composite(g_tag); g_changed = 0; g_last_save = GetTickCount(); }
        }
        LeaveCriticalSection(&g_cs);

        if (now_s() >= next_status) {
            next_status += 5;
            printf("[stat %7.2f] read=%.1f MB reads=%d zero=%d title_time=%.2fs menu=%d popup=%d idle=%d kit=0x%x uo=0x%x argbIG=%d argbPG=%d yuvIG=%d jvm=%ld\n",
                   now_s(), total / 1048576.0, reads, zero_reads, bd_tell_time(bd) / 90000.0, g_menu, g_popup, g_idle_events,
                   g_kit, g_uo, g_argb[1].flushes, g_argb[0].flushes, g_yuv[1].flushes, g_jvm_created);
        }
    }

    printf("[pl] playlists:"); for (int i = 0; i < g_npl; i++) printf(" %05d", g_playlists[i]); printf("\n");
    printf("SUMMARY: bdj_handled=%d jvm_created=%ld argb_flushes=IG:%d/PG:%d yuv_flushes=IG:%d shots=%d menu_event=%d popup=%d kit=0x%x idle=%d playlists=%d jumps=%d read=%.0fMB lib_lines=%ld xlet_msgs=%ld\n",
           di->bdj_handled, g_jvm_created, g_argb[1].flushes, g_argb[0].flushes, g_yuv[1].flushes, g_saved, g_menu, g_popup,
           g_kit, g_idle_events, g_npl, jumps, total / 1048576.0, g_lib_lines, g_xlet_msgs);
    printf("RESULT: %s\n", g_argb[1].flushes > 0 ? "OK BD-J graphics captured" : (g_jvm_created ? "JVM started, no BD-J graphics" : "NO JVM"));
    printf("[%7.2f] closing...\n", now_s());
    bd_close(bd);
    printf("[%7.2f] closed\n", now_s());
    return 0;
}
