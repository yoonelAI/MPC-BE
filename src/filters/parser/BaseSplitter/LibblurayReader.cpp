/*
 * (C) 2026 - MPC-BE BD menu project
 * GPLv3 - see header
 */

#include "stdafx.h"
#include "LibblurayReader.h"

#include "DSUtil/FileHandle.h"
#include <atlconv.h>

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

    // BDMV folder: <disc root>\BDMV\index.bdmv
    CStringW probe = path;
    probe.TrimRight(L"\\/");
    probe += L"\\BDMV\\index.bdmv";
    return (::GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES);
}

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

    // libbluray reads UDF itself (via libudfread); no virtual drive mounting
    // and therefore no administrator privileges are needed.
    if (!bd_open_disc(m_pBD, pathUtf8, nullptr)) {
        return false;
    }

    if (m_playlist) {
        if (!bd_select_playlist(m_pBD, m_playlist)) {
            // Fall back to the first relevant title.
            bd_get_titles(m_pBD, 0, 0);
            bd_select_title(m_pBD, 0);
        }
    } else {
        bd_get_titles(m_pBD, 0, 0);
        bd_select_title(m_pBD, 0);
    }

    const uint32_t title = bd_get_current_title(m_pBD);
    m_pTitleInfo = bd_get_title_info(m_pBD, title, 0);
    m_total = static_cast<LONGLONG>(bd_get_title_size(m_pBD));
    m_pos = 0;

    return m_total > 0;
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
    // MPC-BE's CMultiFiles updates m_rtPTSOffset when a new part starts.
    // libbluray exposes the same information in BLURAY_CLIP_INFO:
    //   start_time = playlist media time, in_time = original stream time (90 kHz)
    // Their difference (converted to 100 ns) is the offset the splitter needs
    // to keep timestamps continuous across seamless branches.
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

    if (llPosition != m_pos) {
        bd_seek(m_pBD, static_cast<uint64_t>(llPosition));
        m_pos = llPosition;
    }

    LONG done = 0;
    while (done < lLength) {
        BD_EVENT ev = {};
        const int ret = bd_read_ext(m_pBD, pBuffer + done, lLength - done, &ev);
        if (ret <= 0) {
            HandleEvent(ev);
            break;
        }
        HandleEvent(ev);
        done += ret;
        m_pos += ret;
    }

    if (done < lLength) {
        // CAsyncFileReader returns zero-filled buffers past EOF as well.
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
    // M1: only reset the stream. M2 will map the selected title/playlist from
    // the menu to bd_select_playlist() here.
    if (!m_pBD) {
        return E_FAIL;
    }
    bd_seek(m_pBD, 0);
    m_pos = 0;
    m_bError = false;
    return S_OK;
}
