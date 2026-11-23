/*
 * (C) 2026 - MPC-BE BD menu project
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "AsyncReader.h"
#include "LibblurayMenu.h"

#include <libbluray/bluray.h>
#include <libbluray/overlay.h>

#include <mutex>
#include <vector>

/*
 * An IAsyncReader whose byte stream comes from libbluray instead of a file.
 *
 * M1: direct ISO/BDMV byte stream (no mounting, no admin).
 * M2: HDMV menu overlay canvas + mouse/navigation control (ILibblurayMenu).
 */
class CLibblurayReader :
    public CUnknown,
    public IAsyncReader,
    public ISyncReader,
    public ILibblurayMenu
{
    struct OverlayCanvas {
        unsigned w = 0, h = 0;
        ULONG version = 0;
        bool visible = false;
        std::vector<uint32_t> work;
        std::vector<uint32_t> publish;

        void Init(int width, int height);
        void Clear();
        void Wipe(int x, int y, int width, int height);
        void Flush();
    };

    BLURAY* m_pBD = nullptr;
    BLURAY_TITLE_INFO* m_pTitleInfo = nullptr;

    uint32_t m_playlist = 0;
    LONGLONG m_total = 0;
    LONGLONG m_pos = 0;

    REFERENCE_TIME* m_pPTSOffset = nullptr;

    bool m_bError = false;
    bool m_bMenuMode = false;

    OverlayCanvas m_overlay;
    std::vector<uint32_t> m_snapshot;
    std::mutex m_overlayLock;

    bool OpenDisc(LPCWSTR pszPath, HRESULT& hr);
    void HandleEvent(const BD_EVENT& ev);
    void UpdatePtsOffset(unsigned playitem);

    static void CALLBACK OverlayProc(void* handle, const BD_OVERLAY* ov);
    static void PaletteToBGRA(const BD_PG_PALETTE_ENTRY* pal, uint32_t out[256]);
    static void DrawRLE(OverlayCanvas& canvas, const BD_OVERLAY* ov);

public:
    CLibblurayReader(LPUNKNOWN pUnk, HRESULT& hr, LPCWSTR pszPath, uint32_t playlist);
    virtual ~CLibblurayReader();

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // IAsyncReader
    STDMETHODIMP RequestAllocator(IMemAllocator*, ALLOCATOR_PROPERTIES*, IMemAllocator**) override { return E_NOTIMPL; }
    STDMETHODIMP Request(IMediaSample*, DWORD_PTR) override { return E_NOTIMPL; }
    STDMETHODIMP WaitForNext(DWORD, IMediaSample**, DWORD_PTR*) override { return E_NOTIMPL; }
    STDMETHODIMP SyncReadAligned(IMediaSample*) override { return E_NOTIMPL; }
    STDMETHODIMP SyncRead(LONGLONG llPosition, LONG lLength, BYTE* pBuffer) override;
    STDMETHODIMP Length(LONGLONG* pTotal, LONGLONG* pAvailable) override;
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }

    // ISyncReader
    STDMETHODIMP_(void) SetBreakEvent(HANDLE) override {}
    STDMETHODIMP_(bool) HasErrors() override { return m_bError; }
    STDMETHODIMP_(void) ClearErrors() override { m_bError = false; }
    STDMETHODIMP_(void) SetPTSOffset(REFERENCE_TIME* rt) override { m_pPTSOffset = rt; }
    STDMETHODIMP_(int) GetSourceType() override { return 0; }
    STDMETHODIMP ReOpen(CHdmvClipInfo::CPlaylist& Items) override;

    // ILibblurayMenu
    STDMETHODIMP GetOverlay(const BYTE** ppBits, ULONG* pWidth, ULONG* pHight,
        ULONG* pVersion, BOOL* pVisible) override;
    STDMETHODIMP MouseSelect(WORD x, WORD y, BOOL bActivate) override;
    STDMETHODIMP UserInput(DWORD key) override;
    STDMETHODIMP MenuCall() override;
    STDMETHODIMP IsMenuMode(BOOL* pMenuMode) override;

    static bool IsDiscPath(LPCWSTR pszPath);
};
