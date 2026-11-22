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

#include <libbluray/bluray.h>

#include <vector>

/*
 * An IAsyncReader whose byte stream comes from libbluray instead of a file.
 *
 * This is the ONLY seam needed for disc playback: CBaseSplitterFilter::Load()
 * normally creates CAsyncFileReader; returning this object instead means every
 * downstream component (splitter / decoder / renderer) stays untouched.
 *
 * M1 scope: open ISO / BDMV folder via libbluray, select a playlist, expose a
 * seekable virtual m2ts byte stream. Menu overlays and BD-J are added in M2/M5.
 *
 * NOTE: not yet compiled - VS2022 is not installed on the development machine.
 */
class CLibblurayReader :
    public CUnknown,
    public IAsyncReader,
    public ISyncReader
{
    BLURAY* m_pBD = nullptr;
    BLURAY_TITLE_INFO* m_pTitleInfo = nullptr;

    uint32_t m_playlist = 0;
    LONGLONG m_total = 0;
    LONGLONG m_pos = 0;

    REFERENCE_TIME* m_pPTSOffset = nullptr;

    bool OpenDisc(LPCWSTR pszPath, HRESULT& hr);
    void HandleEvent(const BD_EVENT& ev);
    void UpdatePtsOffset(unsigned playitem);

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
    STDMETHODIMP_(int) GetSourceType() override { return 0; /* LOCAL - do not disturb existing logic */ }
    STDMETHODIMP ReOpen(CHdmvClipInfo::CPlaylist& Items) override;

    static bool IsDiscPath(LPCWSTR pszPath);

private:
    bool m_bError = false;
};
