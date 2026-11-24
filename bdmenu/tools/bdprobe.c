/*
 * bdprobe.c - MPC-BE BDMenu P0 probe (v4)
 * Opens a Blu-ray (BDMV folder or ISO), runs the HDMV navigation VM via
 * libbluray, captures IG (menu) overlay planes to BMP, tests keyboard and
 * mouse navigation (bd_mouse_select / MOUSE_ACTIVATE) and logs events.
 *
 * usage: bdprobe <disc path or .iso> <output dir> [region A=1 B=2 C=4]
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

typedef struct { int w, h; uint32_t *px; } PLANE;

typedef struct {
    PLANE pl[2];
    int flushes[2];
    int saved;
    int menu_seen;
    int save_enabled;
    const wchar_t *outdir;
    char tag[32];
    int minx, miny, maxx, maxy;   /* bbox of visible IG pixels at last flush */
} CTX;

static uint32_t ycc2argb(const BD_PG_PALETTE_ENTRY *e)
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

static int save_bmp(const wchar_t *path, const PLANE *p)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    int stride = (p->w * 3 + 3) & ~3;
    uint32_t datasz = (uint32_t)stride * p->h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2) = 54 + datasz; *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40; *(int32_t *)(hdr + 18) = p->w; *(int32_t *)(hdr + 22) = p->h;
    *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24; *(uint32_t *)(hdr + 34) = datasz;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t *)calloc(stride, 1);
    for (int y = p->h - 1; y >= 0; y--) {
        for (int x = 0; x < p->w; x++) {
            uint32_t c = p->px[(size_t)y * p->w + x];
            unsigned a = c >> 24, bg = 0x30;
            row[x * 3 + 0] = (uint8_t)(((c & 0xff) * a + bg * (255 - a)) / 255);
            row[x * 3 + 1] = (uint8_t)((((c >> 8) & 0xff) * a + bg * (255 - a)) / 255);
            row[x * 3 + 2] = (uint8_t)((((c >> 16) & 0xff) * a + bg * (255 - a)) / 255);
        }
        fwrite(row, 1, stride, f);
    }
    free(row); fclose(f);
    return 1;
}

static void overlay_cb(void *handle, const BD_OVERLAY *const ov)
{
    CTX *c = (CTX *)handle;
    if (!ov) { printf("[ov] all overlays closed\n"); return; }
    if (ov->plane > 1) return;
    PLANE *pl = &c->pl[ov->plane];
    const char *pn = ov->plane == BD_OVERLAY_IG ? "IG" : "PG";
    switch (ov->cmd) {
    case BD_OVERLAY_INIT:
        free(pl->px); pl->w = ov->w; pl->h = ov->h;
        pl->px = (uint32_t *)calloc((size_t)ov->w * ov->h, 4);
        printf("[ov] %s INIT %dx%d\n", pn, ov->w, ov->h);
        break;
    case BD_OVERLAY_CLOSE:
        free(pl->px); pl->px = NULL; pl->w = pl->h = 0;
        printf("[ov] %s CLOSE\n", pn);
        break;
    case BD_OVERLAY_CLEAR:
        if (pl->px) memset(pl->px, 0, (size_t)pl->w * pl->h * 4);
        break;
    case BD_OVERLAY_HIDE:
        printf("[ov] %s HIDE\n", pn);
        break;
    case BD_OVERLAY_WIPE:
        if (pl->px)
            for (int y = ov->y; y < ov->y + ov->h && y < pl->h; y++)
                for (int x = ov->x; x < ov->x + ov->w && x < pl->w; x++)
                    if (x >= 0 && y >= 0) pl->px[(size_t)y * pl->w + x] = 0;
        break;
    case BD_OVERLAY_DRAW: {
        if (!pl->px || !ov->img || !ov->palette) break;
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
                    if (px >= 0 && px < pl->w && py >= 0 && py < pl->h)
                        pl->px[(size_t)py * pl->w + px] = pal[r->color & 0xff];
                }
                x += r->len; r++;
            }
            if (x >= ov->w && r->len == 0) r++;
        }
        break;
    }
    case BD_OVERLAY_FLUSH: {
        c->flushes[ov->plane]++;
        if (!pl->px || ov->plane != BD_OVERLAY_IG) break;
        size_t vis = 0; int x0 = pl->w, y0 = pl->h, x1 = -1, y1 = -1;
        for (int y = 0; y < pl->h; y++)
            for (int x = 0; x < pl->w; x++)
                if (pl->px[(size_t)y * pl->w + x] >> 24) {
                    vis++;
                    if (x < x0) x0 = x; if (x > x1) x1 = x;
                    if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
        c->minx = x0; c->miny = y0; c->maxx = x1; c->maxy = y1;
        printf("[ov] IG FLUSH #%d visible_px=%zu bbox=(%d,%d)-(%d,%d)\n", c->flushes[1], vis, x0, y0, x1, y1);
        if (vis > 0) {
            c->menu_seen = 1;
            if (c->save_enabled && c->saved < 24) {
                wchar_t path[MAX_PATH];
                swprintf(path, MAX_PATH, L"%ls\\menu_%02d_%hs.bmp", c->outdir, c->saved, c->tag);
                if (save_bmp(path, pl)) { printf("[ov] saved menu_%02d_%s.bmp\n", c->saved, c->tag); c->saved++; }
                c->save_enabled = 0;   /* one image per step */
            }
        }
        break;
    }
    default: break;
    }
}

static const char *ev_name(uint32_t e)
{
    switch (e) {
    case BD_EVENT_NONE: return "NONE"; case BD_EVENT_ERROR: return "ERROR";
    case BD_EVENT_READ_ERROR: return "READ_ERROR"; case BD_EVENT_ENCRYPTED: return "ENCRYPTED";
    case BD_EVENT_ANGLE: return "ANGLE"; case BD_EVENT_TITLE: return "TITLE";
    case BD_EVENT_PLAYLIST: return "PLAYLIST"; case BD_EVENT_PLAYITEM: return "PLAYITEM";
    case BD_EVENT_CHAPTER: return "CHAPTER"; case BD_EVENT_PLAYMARK: return "PLAYMARK";
    case BD_EVENT_END_OF_TITLE: return "END_OF_TITLE"; case BD_EVENT_AUDIO_STREAM: return "AUDIO_STREAM";
    case BD_EVENT_IG_STREAM: return "IG_STREAM"; case BD_EVENT_PG_TEXTST_STREAM: return "PG_TEXTST_STREAM";
    case BD_EVENT_PIP_PG_TEXTST_STREAM: return "PIP_PG_STREAM";
    case BD_EVENT_SECONDARY_AUDIO_STREAM: return "SEC_AUDIO_STREAM";
    case BD_EVENT_SECONDARY_VIDEO_STREAM: return "SEC_VIDEO_STREAM";
    case BD_EVENT_PG_TEXTST: return "PG_TEXTST"; case BD_EVENT_PIP_PG_TEXTST: return "PIP_PG_TEXTST";
    case BD_EVENT_SECONDARY_AUDIO: return "SEC_AUDIO"; case BD_EVENT_SECONDARY_VIDEO: return "SEC_VIDEO";
    case BD_EVENT_SECONDARY_VIDEO_SIZE: return "SEC_VIDEO_SIZE"; case BD_EVENT_PLAYLIST_STOP: return "PLAYLIST_STOP";
    case BD_EVENT_DISCONTINUITY: return "DISCONTINUITY"; case BD_EVENT_SEEK: return "SEEK";
    case BD_EVENT_STILL: return "STILL"; case BD_EVENT_STILL_TIME: return "STILL_TIME";
    case BD_EVENT_SOUND_EFFECT: return "SOUND_EFFECT"; case BD_EVENT_IDLE: return "IDLE";
    case BD_EVENT_POPUP: return "POPUP"; case BD_EVENT_MENU: return "MENU";
    case BD_EVENT_STEREOSCOPIC_STATUS: return "STEREO_STATUS";
    case BD_EVENT_KEY_INTEREST_TABLE: return "KEY_INTEREST"; case BD_EVENT_UO_MASK_CHANGED: return "UO_MASK";
    default: return "?";
    }
}

static char *w2u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static unsigned char g_buf[6144 * 32];
static unsigned long long g_total;
static int g_popup_avail = -1;   /* last BD_EVENT_POPUP param */
static int g_title = -1;         /* last BD_EVENT_TITLE param */
static unsigned long long g_title_at; /* bytes read when title started */
static unsigned long long g_limit = 3000ull * 1048576;  /* per-phase read budget */

/* pump the VM for ms milliseconds, logging events */
static int pump(BLURAY *bd, DWORD ms, CTX *ctx)
{
    DWORD t0 = GetTickCount();
    int idle = 0;
    while (GetTickCount() - t0 < ms) {
        BD_EVENT ev;
        int r = bd_read_ext(bd, g_buf, sizeof(g_buf), &ev);
        if (r < 0) { printf("read error %d\n", r); return -1; }
        g_total += (unsigned)r;
        if (ev.event != BD_EVENT_NONE)
            printf("[ev] %-16s (%2u) param=%u  (read %.1f MB)\n", ev_name(ev.event), ev.event, ev.param, g_total / 1048576.0);
        if (ev.event == BD_EVENT_POPUP) g_popup_avail = (int)ev.param;
        if (ev.event == BD_EVENT_TITLE) { g_title = (int)ev.param; g_title_at = g_total; }
        if (r == 0 && ev.event == BD_EVENT_NONE) { if (++idle % 20 == 0) Sleep(5); }
        else idle = 0;
        if (g_total > g_limit) { printf("read budget reached (%.0f MB)\n", g_total / 1048576.0); return -2; }
    }
    (void)ctx;
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: bdprobe <disc|iso> <outdir> [region A=1 B=2 C=4]\n"); return 1; }
    char *path = w2u8(argv[1]);
    int region = argc > 3 ? _wtoi(argv[3]) : 1;
    static wchar_t outabs[MAX_PATH];
    if (!_wfullpath(outabs, argv[2], MAX_PATH)) wcsncpy(outabs, argv[2], MAX_PATH - 1);
    _wmkdir(outabs);

    int maj, min, mic; bd_get_version(&maj, &min, &mic);
    printf("libbluray %d.%d.%d\ndisc: %s\nregion mask: %d\n", maj, min, mic, path, region);

    BLURAY *bd = bd_open(path, NULL);
    if (!bd) { printf("RESULT: FAIL bd_open\n"); return 2; }
    printf("[open] OK\n");
    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    printf("bluray_detected=%d first_play=%d top_menu=%d titles=%u hdmv=%u bdj=%u\n",
           di->bluray_detected, di->first_play_supported, di->top_menu_supported,
           di->num_titles, di->num_hdmv_titles, di->num_bdj_titles);
    printf("aacs_detected=%d aacs_handled=%d bdplus_detected=%d\n", di->aacs_detected, di->aacs_handled, di->bdplus_detected);
    if (di->disc_name) printf("disc_name=%s\n", di->disc_name);

    bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_REGION_CODE, (uint32_t)region);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_MENU_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_PG_LANG, "chi");

    CTX ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.outdir = outabs; strcpy(ctx.tag, "first"); ctx.save_enabled = 1;
    bd_register_overlay_proc(bd, &ctx, overlay_cb);

    if (!bd_play(bd)) { printf("RESULT: FAIL bd_play\n"); bd_close(bd); return 3; }
    printf("bd_play ok\n");

    /* phase 0: let First Play run; then try (a) pop-up menu, (b) top menu */
    DWORD t0 = GetTickCount();
    int step = 0;             /* 0 = waiting first play, 1 = popup tried, 2 = top menu tried */
    unsigned long long mark = 0;
    while (!ctx.menu_seen && GetTickCount() - t0 < 60000) {
        if (pump(bd, 150, &ctx) == -1) break;
        if (ctx.menu_seen) break;
        int in_title = g_title >= 1 && g_title != 65535;
        if (step == 0 && in_title && g_total > g_title_at + 80ull * 1048576) {  /* let IG stream decode first */
            if (g_popup_avail == 1) {
                printf("[nav] title %d playing, pop-up menu available -> BD_VK_POPUP\n", g_title);
                strcpy(ctx.tag, "popup"); ctx.save_enabled = 1;
                bd_user_input(bd, -1, BD_VK_POPUP);
            } else {
                printf("[nav] title %d playing, no pop-up -> skip to top menu\n", g_title);
            }
            step = 1; mark = g_total; g_limit = g_total + 1500ull * 1048576;
            continue;
        }
        if (step == 1 && (g_total > mark + 400ull * 1048576 || g_limit - g_total < 50ull * 1048576)) {
            printf("[nav] no menu yet -> bd_menu_call (top menu)\n");
            strcpy(ctx.tag, "topmenu"); ctx.save_enabled = 1;
            bd_menu_call(bd, -1);
            step = 2; mark = g_total; g_limit = g_total + 1500ull * 1048576;
            continue;
        }
        if (step == 0 && g_total > 2000ull * 1048576) {
            printf("[nav] first play still running after 2 GB -> bd_menu_call\n");
            strcpy(ctx.tag, "topmenu"); ctx.save_enabled = 1;
            bd_menu_call(bd, -1); step = 2; mark = g_total; g_limit = g_total + 1500ull * 1048576;
        }
        if (step == 2 && g_total > g_limit - 10ull * 1048576) break;
    }
    if (!ctx.menu_seen) {
        printf("IG flushes=%d PG flushes=%d popup_avail=%d title=%d\nRESULT: NO MENU captured\n",
               ctx.flushes[1], ctx.flushes[0], g_popup_avail, g_title);
        bd_close(bd); return 4;
    }
    g_limit = g_total + 4000ull * 1048576;
    printf("[nav] menu visible after %.1fs\n", (GetTickCount() - t0) / 1000.0);
    pump(bd, 1500, &ctx);

    /* phase 1: keyboard */
    struct { uint32_t k; const char *n; } keys[] = {
        { BD_VK_DOWN, "DOWN" }, { BD_VK_RIGHT, "RIGHT" }, { BD_VK_UP, "UP" }, { BD_VK_LEFT, "LEFT" } };
    int key_changes = 0;
    for (int i = 0; i < 4; i++) {
        int f0 = ctx.flushes[1];
        strcpy(ctx.tag, keys[i].n); ctx.save_enabled = 1;
        printf("[key] %s\n", keys[i].n);
        bd_user_input(bd, -1, keys[i].k);
        pump(bd, 1200, &ctx);
        if (ctx.flushes[1] > f0) key_changes++;
        printf("[key] %s -> IG flushes +%d\n", keys[i].n, ctx.flushes[1] - f0);
    }

    /* phase 2: mouse hover sweep over 1920x1080 grid */
    int hits = 0, hx = -1, hy = -1, last_bbox = -1;
    for (int y = 45; y < 1080 && hits < 40; y += 60) {
        for (int x = 40; x < 1920; x += 80) {
            int f0 = ctx.flushes[1];
            int rr = bd_mouse_select(bd, -1, (uint16_t)x, (uint16_t)y);
            if (ctx.flushes[1] == f0) pump(bd, 30, &ctx);
            if (ctx.flushes[1] > f0) {
                int b = ctx.minx * 7 + ctx.miny * 13 + ctx.maxx * 17 + ctx.maxy;
                hits++;
                printf("[mouse] hover (%d,%d) ret=%d -> highlight changed\n", x, y, rr);
                if (hx < 0) { hx = x; hy = y; }
                if (b != last_bbox && ctx.saved < 20) {
                    sprintf(ctx.tag, "mouse_%d_%d", x, y); ctx.save_enabled = 1;
                    bd_mouse_select(bd, -1, (uint16_t)(x + 1), (uint16_t)y);
                    pump(bd, 50, &ctx);
                    last_bbox = b;
                }
            }
        }
    }
    printf("[mouse] hover hits=%d\n", hits);

    /* phase 3: mouse click on first hit */
    int click_nav = 0;
    if (hx >= 0) {
        printf("[mouse] click at (%d,%d)\n", hx, hy);
        bd_mouse_select(bd, -1, (uint16_t)hx, (uint16_t)hy);
        pump(bd, 300, &ctx);
        bd_user_input(bd, -1, BD_VK_MOUSE_ACTIVATE);
        unsigned long long before = g_total;
        g_limit = g_total + 200ull * 1048576;
        strcpy(ctx.tag, "after_click"); ctx.save_enabled = 1;
        pump(bd, 6000, &ctx);
        click_nav = g_total > before + 4 * 1048576;  /* playback consumed data */
        printf("[mouse] after click read +%.1f MB\n", (g_total - before) / 1048576.0);
    }

    printf("SUMMARY: menu=yes via=%s images=%d key_highlight_changes=%d/4 mouse_hover_hits=%d click_started_playback=%s\n",
           step == 2 ? "top_menu" : (step == 1 ? "popup" : "first_play"), ctx.saved, key_changes, hits, click_nav ? "yes" : "no");
    printf("RESULT: OK menu captured\n");
    bd_close(bd);
    free(ctx.pl[0].px); free(ctx.pl[1].px); free(path);
    return 0;
}
