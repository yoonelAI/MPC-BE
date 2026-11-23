/*
 * (C) 2026 - MPC-BE BD menu project
 * GPLv3 - see header
 */

#include "stdafx.h"
#include "LibblurayReader.h"

#include "DSUtil/FileHandle.h"
#include <atlconv.h>
#include <algorithm>
#include <cmath>

#include <libbluray/keys.h>

// static
bool CLibblurayReader::IsDiscPath(LPCWSTR pszPath)
{
    if (!pszPath || !*pszPath) {
        return false;
    }

    const CStringW path(pszPath);
    const CStringW ext = GetFileExt(path).MakeLower();

    if (ext == L".iso" || ext == L".mdf" || ext == L".img") {
        return true;
    }

    CStringW probe = path;
    probe.TrimRight(L"\\/");
    probe += L"\\BDMV\\index.bdmv";
    return (::GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES);
}

// ---------------------------------------------------------------------------
// HDMV IG overlay canvas (ported from mpv stream/stream_bluray.c)
// ---------------------------------------------------------------------------

void CLibblurayReader::OverlayCanvas::Init(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (w == static_cast<unsigned>(width) && h == static_cast<unsigned>(height)) {
        return;
    }
    w = width;
    h = height;
    work.assign(static_cast<size_t>(w) * h, 0);
    publish.assign(static_cast<size_t>(w) * h, 0);
    version++;
}

void CLibblurayReader::OverlayCanvas::Clear()
{
    std::fill(work.begin(), work.end(), 0u);
}

void CLibblurayReader::OverlayCanvas::Wipe(int x, int y, int width, int height)
{
    if (work.empty()) {
        return;
    }
    x = std::max(x, 0);
    y = std::max(y, 0);
    const int x2 = std::min(x + width, static_cast<int>(w));
    const int y2 = std::min(y + height, static_cast<int>(h));
    for (int yy = y; yy < y2; ++yy) {
        std::fill(work.begin() + static_cast<size_t>(yy) * w + x,
            work.begin() + static_cast<size_t>(yy) * w + x2, 0u);
    }
}

void CLibblurayReader::OverlayCanvas::Flush()
{
    if (work.empty()) {
        return;
    }
    publish = work;
    visible = false;
    for (uint32_t px : publish) {
        if (px & 0xFF000000u) {
            visible = true;
            break;
        }
    }
    version++;
}

void CLibblurayReader::PaletteToBGRA(const BD_PG_PALETTE_ENTRY* pal, uint32_t out[256])
{
    // Limited-range YCbCr -> full-range RGB (BT.709; menus look close enough
    // with BT.601 discs too, and libbluray does not expose the CSP per event).
    for (int i = 0; i < 256; ++i) {
        const double y = (pal[i].Y - 16) * 255.0 / 219.0;
        const double cb = (pal[i].Cb - 128) * 255.0 / 224.0;
        const double cr = (pal[i].Cr - 128) * 255.0 / 224.0;
        double r = y + 1.5748 * cr;
        double g = y - 0.187324 * cb - 0.468124 * cr;
        double b = y + 1.8556 * cb;
        r = std::clamp(r, 0.0, 255.0);
        g = std::clamp(g, 0.0, 255.0);
        b = std::clamp(b, 0.0, 255.0);
        const int t = pal[i].T;
        out[i] = (static_cast<uint32_t>(t) << 24)
            | (static_cast<uint32_t>(r * t / 255) << 16)
            | (static_cast<uint32_t>(g * t / 255) << 8)
            | static_cast<uint32_t>(b * t / 255);
    }
    out[0xFF] = 0; // index 0xFF is always transparent
}

void CLibblurayReader::DrawRLE(OverlayCanvas& canvas, const BD_OVERLAY* ov)
{
    if (!ov->img || !ov->palette || ov->w <= 0 || ov->h <= 0 || canvas.work.empty()) {
        return;
    }

    uint32_t pal[256];
    PaletteToBGRA(ov->palette, pal);

    const BD_PG_RLE_ELEM* rle = ov->img;
    for (int y = 0; y < ov->h; ++y) {
        const int dstY = ov->y + y;
        if (dstY < 0 || dstY >= static_cast<int>(canvas.h)) {
            continue;
        }
        uint32_t* row = canvas.work.data() + static_cast<size_t>(dstY) * canvas.w;
        int x = 0;
        int cols = ov->w;
        while (x < ov->w && cols-- >= 0) {
            const int len = rle->len;
            const int color = rle->color;
            ++rle;
            if (len == 0) {
                continue;
            }
            int dstX = ov->x + x;
            int run = len;
            if (dstX < 0) {
                const int skip = std::min(-dstX, run);
                run -= skip;
                dstX += skip;
            }
            if (dstX + run > static_cast<int>(canvas.w)) {
                run = static_cast<int>(canvas.w) - dstX;
            }
            if (run > 0) {
                const uint32_t c = pal[color & 0xFF];
                std::fill(row + dstX, row + dstX + run, c);
            }
            x += len;
        }
        if (rle->len == 0) {
            ++rle;
        }
    }
}

void CLibblurayReader::OverlayProc(void* handle, const BD_OVERLAY* ov)
{
    auto* self = static_cast<CLibblurayReader*>(handle);
    if (!self || !ov || ov->plane != BD_OVERLAY_IG) {
        return;
    }

    std::lock_guard<std::mutex> lock(self->m_overlayLock);
    switch (ov->cmd) {
    case BD_OVERLAY_INIT:
        self->m_overlay.Init(ov->w, ov->h);
        self->m_overlay.Clear();
        self->m_overlay.visible = false;
        break;
    case BD_OVERLAY_CLOSE:
    case BD_OVERLAY_HIDE:
        self->m_overlay.visible = false;
        self->m_overlay.version++;
        break;
    case BD_OVERLAY_CLEAR:
        self->m_overlay.Clear();
        break;
    case BD_OVERLAY_WIPE:
        self->m_overlay.Wipe(ov->x, ov->y, ov->w, ov->h);
        break;
    case BD_OVERLAY_DRAW:
        DrawRLE(self->m_overlay, ov);
        break;
    case BD_OVERLAY_FLUSH:
        self->m_overlay.Flush();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------

CLibblurayReader::CLibblurayReader(LPUNKNOWN pUnk, HRESULT& hr, LPCWSTR pszPath, uint32_t playlist)
    : CUnknown(L"CLibblurayReader", pUnk)
    , m_playlist(playlist)
{
    if (!OpenDisc(pszPath, hr)) {
        hr = E_FAIL;
    }
}

CLibblurayReader::~CLibblurayReader()
{
    if (m_pTitleInfo) {
        bd_free_title_info(m_pTitleInfo);
        m_pTitleInfo = nullptr;
    }
    if (m_pBD) {
        bd_close(m_pBD);
        m_pBD = nullptr;
    }
}

STDMETHODIMP CLibblurayReader::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IAsyncReader) {
        return GetInterface(static_cast<IAsyncReader*>(this), ppv);
    }
    if (riid == __uuidof(ISyncReader)) {
        return GetInterface(static_cast<ISyncReader*>(this), ppv);
    }
    if (riid == __uuidof(ILibblurayMenu)) {
        return GetInterface(static_cast<ILibblurayMenu*>(this), ppv);
    }
    return __super::NonDelegatingQueryInterface(riid, ppv);
}

bool CLibblurayReader::OpenDisc(LPCWSTR pszPath, HRESULT& hr)
{
    CW2A pathUtf8(pszPath, CP_UTF8);

    m_pBD = bd_init();
    if (!m_pBD) {
        hr = E_OUTOFMEMORY;
        return false;
    }

    if (!bd_open_disc(m_pBD, pathUtf8, nullptr)) {
        return false;
    }

    bd_get_titles(m_pBD, 0, 0);

    // HDMV menus only run while libbluray is in navigation ("play") mode.
    // bd_register_overlay_proc() must be installed before bd_play().
    bd_register_overlay_proc(m_pBD, this, &CLibblurayReader::OverlayProc);
    if (bd_play(m_pBD)) {
        m_bMenuMode = true;
    } else if (m_playlist) {
        bd_select_playlist(m_pBD, m_playlist);
    } else {
        bd_select_title(m_pBD, 0);
    }

    const uint32_t title = bd_get_current_title(m_pBD);
    m_pTitleInfo = bd_get_title_info(m_pBD, title, 0);
    m_total = static_cast<LONGLONG>(bd_get_title_size(m_pBD));
    m_pos = 0;

    return true; // menu mode may legitimately have no known title size yet
}

void CLibblurayReader::HandleEvent(const BD_EVENT& ev)
{
    switch (ev.event) {
    case BD_EVENT_ERROR:
    case BD_EVENT_ENCRYPTED:
        m_bError = true;
        break;
    case BD_EVENT_PLAYITEM:
        UpdatePtsOffset(ev.param);
        break;
    default:
        break;
    }
}

void CLibblurayReader::UpdatePtsOffset(unsigned playitem)
{
    if (!m_pPTSOffset || !m_pTitleInfo || playitem >= m_pTitleInfo->clip_count) {
        return;
    }

    const BLURAY_CLIP_INFO& clip = m_pTitleInfo->clips[playitem];
    if (clip.start_time >= clip.in_time) {
        const uint64_t diff90k = clip.start_time - clip.in_time;
        *m_pPTSOffset = static_cast<REFERENCE_TIME>(diff90k * 1000 / 9);
    }
}

STDMETHODIMP CLibblurayReader::SyncRead(LONGLONG llPosition, LONG lLength, BYTE* pBuffer)
{
    CheckPointer(pBuffer, E_POINTER);
    if (lLength <= 0) {
        return S_OK;
    }
    if (!m_pBD) {
        return E_FAIL;
    }

    if (llPosition != m_pos && !m_bMenuMode) {
        bd_seek(m_pBD, static_cast<uint64_t>(llPosition));
        m_pos = llPosition;
    }

    LONG done = 0;
    while (done < lLength) {
        BD_EVENT ev = {};
        const int ret = bd_read_ext(m_pBD, pBuffer + done, lLength - done, &ev);
        HandleEvent(ev);
        if (ret <= 0) {
            break;
        }
        done += ret;
        m_pos += ret;
    }

    if (done < lLength) {
        memset(pBuffer + done, 0, lLength - done);
    }

    return S_OK;
}

STDMETHODIMP CLibblurayReader::Length(LONGLONG* pTotal, LONGLONG* pAvailable)
{
    CheckPointer(pTotal, E_POINTER);
    *pTotal = m_total;
    if (pAvailable) {
        *pAvailable = m_total;
    }
    return S_OK;
}

STDMETHODIMP CLibblurayReader::ReOpen(CHdmvClipInfo::CPlaylist& Items)
{
    if (!m_pBD) {
        return E_FAIL;
    }
    if (m_bMenuMode) {
        bd_menu_call(m_pBD, -1);
    } else {
        bd_seek(m_pBD, 0);
    }
    m_pos = 0;
    m_bError = false;
    return S_OK;
}

// ILibblurayMenu

STDMETHODIMP CLibblurayReader::GetOverlay(const BYTE** ppBits, ULONG* pWidth,
    ULONG* pHight, ULONG* pVersion, BOOL* pVisible)
{
    CheckPointer(ppBits, E_POINTER);
    std::lock_guard<std::mutex> lock(m_overlayLock);
    m_snapshot = m_overlay.publish;
    *ppBits = reinterpret_cast<const BYTE*>(m_snapshot.data());
    if (pWidth) *pWidth = m_overlay.w;
    if (pHight) *pHight = m_overlay.h;
    if (pVersion) *pVersion = m_overlay.version;
    if (pVisible) *pVisible = m_overlay.visible && !m_snapshot.empty();
    return S_OK;
}

STDMETHODIMP CLibblurayReader::MouseSelect(WORD x, WORD y, BOOL bActivate)
{
    if (!m_pBD) {
        return E_FAIL;
    }
    bd_mouse_select(m_pBD, -1, x, y);
    if (bActivate) {
        bd_user_input(m_pBD, -1, BD_VK_MOUSE_ACTIVATE);
    }
    return S_OK;
}

STDMETHODIMP CLibblurayReader::UserInput(DWORD key)
{
    if (!m_pBD) {
        return E_FAIL;
    }
    bd_user_input(m_pBD, -1, static_cast<uint32_t>(key));
    return S_OK;
}

STDMETHODIMP CLibblurayReader::MenuCall()
{
    if (!m_pBD) {
        return E_FAIL;
    }
    bd_menu_call(m_pBD, -1);
    return S_OK;
}

STDMETHODIMP CLibblurayReader::IsMenuMode(BOOL* pMenuMode)
{
    CheckPointer(pMenuMode, E_POINTER);
    *pMenuMode = m_bMenuMode;
    return S_OK;
}
