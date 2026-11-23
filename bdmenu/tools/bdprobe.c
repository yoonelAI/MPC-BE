/*
 * bdprobe.c - MPC-BE BDMenu P0 probe
 * Opens a Blu-ray (BDMV folder / disc / ISO), runs the HDMV navigation VM
 * via libbluray, captures IG (menu) overlay planes to BMP files and logs
 * navigation events. Also injects DOWN / RIGHT keys to verify highlight.
 *
 * usage: bdprobe <disc path> <output dir> [region bitmask: A=1 B=2 C=4]
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

typedef struct {
    int w, h;
    uint32_t *px;      /* ARGB */
} PLANE;

typedef struct {
    PLANE pl[2];       /* 0 = PG, 1 = IG */
    int flushes[2];
    int saved;
    int new_save;
    const wchar_t *outdir;
} CTX;

static uint32_t ycc2argb(const BD_PG_PALETTE_ENTRY *e)
{
    double Y  = (e->Y  - 16)  * 255.0 / 219.0;
    double Cb = (e->Cb - 128) * 255.0 / 224.0;
    double Cr = (e->Cr - 128) * 255.0 / 224.0;
    double r = Y + 1.5748 * Cr;
    double g = Y - 0.1873 * Cb - 0.4681 * Cr;
    double b = Y + 1.8556 * Cb;
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
    *(uint32_t *)(hdr + 2)  = 54 + datasz;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = p->w;
    *(int32_t  *)(hdr + 22) = p->h;      /* bottom-up */
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = datasz;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t *)calloc(stride, 1);
    for (int y = p->h - 1; y >= 0; y--) {
        for (int x = 0; x < p->w; x++) {
            uint32_t c = p->px[(size_t)y * p->w + x];
            unsigned a = c >> 24;
            /* composite over dark grey so transparent areas are visible */
            unsigned bg = 0x30;
            row[x * 3 + 0] = (uint8_t)(((c       & 0xff) * a + bg * (255 - a)) / 255);
            row[x * 3 + 1] = (uint8_t)((((c >> 8) & 0xff) * a + bg * (255 - a)) / 255);
            row[x * 3 + 2] = (uint8_t)((((c >> 16) & 0xff) * a + bg * (255 - a)) / 255);
        }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
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
        free(pl->px);
        pl->w = ov->w; pl->h = ov->h;
        pl->px = (uint32_t *)calloc((size_t)ov->w * ov->h, 4);
        printf("[ov] %s INIT %dx%d at %d,%d\n", pn, ov->w, ov->h, ov->x, ov->y);
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
        if (pl->px) {
            for (int y = ov->y; y < ov->y + ov->h && y < pl->h; y++)
                for (int x = ov->x; x < ov->x + ov->w && x < pl->w; x++)
                    if (x >= 0 && y >= 0) pl->px[(size_t)y * pl->w + x] = 0;
        }
        break;
    case BD_OVERLAY_DRAW: {
        if (!pl->px || !ov->img || !ov->palette) break;
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) pal[i] = ycc2argb(&ov->palette[i]);
        const BD_PG_RLE_ELEM *r = ov->img;
        for (int y = 0; y < ov->h; y++) {
            int x = 0;
            while (x < ov->w) {
                if (r->len == 0) { r++; break; }       /* end of line */
                int py = ov->y + y;
                for (int k = 0; k < r->len && x + k < ov->w; k++) {
                    int px = ov->x + x + k;
                    if (px >= 0 && px < pl->w && py >= 0 && py < pl->h)
                        pl->px[(size_t)py * pl->w + px] = pal[r->color & 0xff];
                }
                x += r->len;
                r++;
            }
            if (x >= ov->w && r->len == 0) r++;       /* consume EOL marker */
        }
        break;
    }
    case BD_OVERLAY_FLUSH: {
        c->flushes[ov->plane]++;
        if (!pl->px) break;
        size_t n = (size_t)pl->w * pl->h, vis = 0;
        for (size_t i = 0; i < n; i++) if (pl->px[i] >> 24) vis++;
        printf("[ov] %s FLUSH #%d visible_px=%zu\n", pn, c->flushes[ov->plane], vis);
        if (ov->plane == BD_OVERLAY_IG && vis > 0 && c->saved < 12) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\menu_%02d.bmp", c->outdir, c->saved);
            if (save_bmp(path, pl)) {
                printf("[ov] saved menu_%02d.bmp\n", c->saved);
                c->saved++;
                c->new_save = 1;
            }
        }
        break;
    }
    default:
        printf("[ov] %s cmd=%d\n", pn, ov->cmd);
    }
}

static const char *ev_name(uint32_t e)
{
    switch (e) {
    case BD_EVENT_NONE: return "NONE";
    case BD_EVENT_ERROR: return "ERROR";
    case BD_EVENT_READ_ERROR: return "READ_ERROR";
    case BD_EVENT_ENCRYPTED: return "ENCRYPTED";
    case BD_EVENT_ANGLE: return "ANGLE";
    case BD_EVENT_TITLE: return "TITLE";
    case BD_EVENT_PLAYLIST: return "PLAYLIST";
    case BD_EVENT_PLAYITEM: return "PLAYITEM";
    case BD_EVENT_CHAPTER: return "CHAPTER";
    case BD_EVENT_PLAYMARK: return "PLAYMARK";
    case BD_EVENT_END_OF_TITLE: return "END_OF_TITLE";
    case BD_EVENT_AUDIO_STREAM: return "AUDIO_STREAM";
    case BD_EVENT_IG_STREAM: return "IG_STREAM";
    case BD_EVENT_PG_TEXTST_STREAM: return "PG_TEXTST_STREAM";
    case BD_EVENT_PG_TEXTST: return "PG_TEXTST";
    case BD_EVENT_SEEK: return "SEEK";
    case BD_EVENT_STILL: return "STILL";
    case BD_EVENT_STILL_TIME: return "STILL_TIME";
    case BD_EVENT_SOUND_EFFECT: return "SOUND_EFFECT";
    case BD_EVENT_IDLE: return "IDLE";
    case BD_EVENT_POPUP: return "POPUP";
    case BD_EVENT_MENU: return "MENU";
    case BD_EVENT_DISCONTINUITY: return "DISCONTINUITY";
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

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: bdprobe <disc path> <output dir> [region A=1 B=2 C=4]\n");
        return 1;
    }
    char *path = w2u8(argv[1]);
    int region = argc > 3 ? _wtoi(argv[3]) : 2;
    _wmkdir(argv[2]);

    int maj, min, mic;
    bd_get_version(&maj, &min, &mic);
    printf("libbluray %d.%d.%d\n", maj, min, mic);
    printf("disc: %s\nregion mask: %d\n", path, region);

    BLURAY *bd = bd_open(path, NULL);
    if (!bd) { printf("RESULT: FAIL bd_open\n"); return 2; }

    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    printf("bluray_detected=%d first_play=%d top_menu=%d\n",
           di->bluray_detected, di->first_play_supported, di->top_menu_supported);
    printf("titles=%u hdmv=%u bdj=%u unsupported=%u\n",
           di->num_titles, di->num_hdmv_titles, di->num_bdj_titles, di->num_unsupported_titles);
    printf("bdj_detected=%d bdj_supported=%d libjvm=%d bdj_handled=%d\n",
           di->bdj_detected, di->bdj_supported, di->libjvm_detected, di->bdj_handled);
    printf("aacs_detected=%d aacs_handled=%d bdplus_detected=%d\n",
           di->aacs_detected, di->aacs_handled, di->bdplus_detected);
    if (di->disc_name) printf("disc_name=%s\n", di->disc_name);

    uint32_t nt = bd_get_titles(bd, TITLES_RELEVANT, 0);
    int mt = bd_get_main_title(bd);
    printf("relevant playlists=%u main_title_idx=%d\n", nt, mt);
    if (mt >= 0) {
        BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, (uint32_t)mt, 0);
        if (ti) {
            printf("main: %05u.mpls duration=%.1fs chapters=%u\n",
                   ti->playlist, ti->duration / 90000.0, ti->chapter_count);
            bd_free_title_info(ti);
        }
    }

    bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_REGION_CODE, (uint32_t)region);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_MENU_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_PG_LANG, "chi");

    CTX ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.outdir = argv[2];
    bd_register_overlay_proc(bd, &ctx, overlay_cb);

    if (!bd_play(bd)) { printf("RESULT: FAIL bd_play\n"); bd_close(bd); return 3; }
    printf("bd_play ok, running navigation...\n");

    static unsigned char buf[6144 * 32];
    unsigned long long total = 0;
    DWORD t0 = GetTickCount(), tlast = t0;
    int phase = 0, idle = 0;
    const uint32_t keys[] = { BD_VK_DOWN, BD_VK_RIGHT, BD_VK_UP };

    for (;;) {
        BD_EVENT ev;
        int r = bd_read_ext(bd, buf, sizeof(buf), &ev);
        if (r < 0) { printf("read error %d\n", r); break; }
        total += (unsigned)r;
        if (ev.event != BD_EVENT_NONE)
            printf("[ev] %-14s param=%u  (read %.1f MB)\n", ev_name(ev.event), ev.param, total / 1048576.0);

        if (ev.event == BD_EVENT_STILL_TIME || ev.event == BD_EVENT_STILL) {
            /* still frame in menu: keep VM running */
            Sleep(20);
        }
        if (r == 0 && ev.event == BD_EVENT_NONE) {
            if (++idle % 50 == 0) Sleep(10);
        } else idle = 0;

        DWORD now = GetTickCount();
        if (ctx.new_save) {
            ctx.new_save = 0;
            tlast = now;
            if (phase < 3) {
                printf("[key] send %s\n", phase == 0 ? "DOWN" : phase == 1 ? "RIGHT" : "UP");
                bd_user_input(bd, -1, keys[phase]);
                phase++;
            }
        }
        if (phase >= 3 && now - tlast > 3000) break;
        if (now - tlast > 20000 && ctx.saved > 0) break;
        if (now - t0 > 90000) { printf("timeout\n"); break; }
        if (total > 1500ull * 1048576) { printf("read limit\n"); break; }
    }

    printf("IG flushes=%d PG flushes=%d menu images=%d\n", ctx.flushes[1], ctx.flushes[0], ctx.saved);
    printf("RESULT: %s\n", ctx.saved > 0 ? "OK menu captured" : "NO MENU captured");
    bd_close(bd);
    free(ctx.pl[0].px); free(ctx.pl[1].px);
    free(path);
    return ctx.saved > 0 ? 0 : 4;
}
