/*
 * BDMenuSubPic.cpp - subpicture provider showing the Blu-ray menu (IG plane) over the current subtitle
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDMenuSubPic.h"
#include "BDNavigator.h"

// Buffered queue: the menu is delivered as consecutive short windows so that a change only has to
// invalidate the windows after the current one (no blank frame -> no flicker), see CMainFrame::BDNavUpdateMenu.
// Synchronous queue (navigation mode default): one segment that lasts until the subtitle underneath
// changes (or 1 s), the main window invalidates it when the menu changes.

static const REFERENCE_TIME kMenuStop = 1LL << 60;
static const REFERENCE_TIME kMaxSeg   = 10000000LL;   // re-check the subtitle at least every second

CBDMenuSubPicProvider::CBDMenuSubPicProvider()
	: CSubPicProviderImpl(&m_csLock)
	, m_bSingle(g_bBDNavNoQueueApplied)
{
}

void CBDMenuSubPicProvider::SetInner(ISubPicProvider* pInner)
{
	CAutoLock lock(&m_csInner);
	m_pInner = pInner;
}

CComPtr<ISubPicProvider> CBDMenuSubPicProvider::GetInner()
{
	CAutoLock lock(&m_csInner);
	return m_pInner;
}

void CBDMenuSubPicProvider::InnerSegment(ISubPicProvider* pInner, REFERENCE_TIME rt, double fps, REFERENCE_TIME& stop, bool& animated)
{
	stop = rt + kMaxSeg;
	animated = false;
	if (!pInner || FAILED(pInner->Lock())) {
		return;
	}
	POSITION p = pInner->GetStartPosition(rt, fps);
	for (int n = 0; p && n < 16; n++) {
		const REFERENCE_TIME s = pInner->GetStart(p, fps);
		REFERENCE_TIME e = pInner->GetStop(p, fps);
		if (e == ISubPicProvider::UNKNOWN_TIME) {
			e = rt + 1;
		}
		if (s > rt) {
			stop = std::min(stop, s);   // next subtitle starts
			break;
		}
		if (e > rt) {
			stop = std::min(stop, e);   // current subtitle ends
			if (pInner->IsAnimated(p)) {
				animated = true;
			}
		}
		p = pInner->GetNext(p);
	}
	pInner->Unlock();
	if (stop <= rt) {
		stop = rt + 1;
	}
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetStartPosition(REFERENCE_TIME rt, double fps, bool CleanOld)
{
	if (!CBDNavigator::Get().IsOverlayOn()) {
		return nullptr;
	}
	if (m_bSingle) {
		CComPtr<ISubPicProvider> pInner = GetInner();
		if (pInner) {
			m_segStart = rt;
			InnerSegment(pInner, rt, fps, m_segStop, m_bInnerAnimated);
		} else {
			m_segStart = 0;
			m_segStop = kMenuStop;
			m_bInnerAnimated = false;
		}
		return (POSITION)1;
	}
	if (rt < 0) {
		rt = 0;
	}
	return (POSITION)(INT_PTR)(rt / BDMENU_WINDOW + 1);
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetNext(POSITION pos)
{
	return m_bSingle ? nullptr : (POSITION)((INT_PTR)pos + 1);
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStart(POSITION pos, double fps)
{
	return m_bSingle ? m_segStart : ((INT_PTR)pos - 1) * BDMENU_WINDOW;
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStop(POSITION pos, double fps)
{
	return m_bSingle ? m_segStop : (INT_PTR)pos * BDMENU_WINDOW;
}

STDMETHODIMP_(bool) CBDMenuSubPicProvider::IsAnimated(POSITION pos)
{
	return m_bSingle && m_bInnerAnimated;
}

STDMETHODIMP CBDMenuSubPicProvider::Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox)
{
	SetRectEmpty(&bbox);
	bool bSub = false;
	RECT rcSub = {};
	CComPtr<ISubPicProvider> pInner = GetInner();
	if (pInner && SUCCEEDED(pInner->Lock())) {
		POSITION p = pInner->GetStartPosition(rt, fps);
		for (int n = 0; p && n < 16; n++) {
			const REFERENCE_TIME s = pInner->GetStart(p, fps);
			const REFERENCE_TIME e = pInner->GetStop(p, fps);
			if (s > rt) {
				break;
			}
			if (rt < e || e == ISubPicProvider::UNKNOWN_TIME) {
				// the subtitle renders everything that is active at rt
				bSub = SUCCEEDED(pInner->Render(spd, rt, fps, rcSub)) && !IsRectEmpty(&rcSub);
				break;
			}
			p = pInner->GetNext(p);
		}
		pInner->Unlock();
	}

	RECT rcMenu = {};
	const bool bMenu = CBDNavigator::Get().RenderMenu(spd, rcMenu, bSub);
	if (bSub && bMenu) {
		UnionRect(&bbox, &rcSub, &rcMenu);
	} else if (bSub) {
		bbox = rcSub;
	} else if (bMenu) {
		bbox = rcMenu;
	}
	return (bSub || bMenu) ? S_OK : E_FAIL;
}

STDMETHODIMP CBDMenuSubPicProvider::GetTextureSize(POSITION pos, SIZE& MaxTextureSize, SIZE& VirtualSize, POINT& VirtualTopLeft)
{
	// with a subtitle underneath, use its texture (PGS coordinates); the menu is scaled into it
	CComPtr<ISubPicProvider> pInner = GetInner();
	if (pInner && SUCCEEDED(pInner->Lock())) {
		HRESULT hr = E_FAIL;
		const REFERENCE_TIME rt = m_bSingle ? m_segStart : ((INT_PTR)pos - 1) * BDMENU_WINDOW;
		if (POSITION p = pInner->GetStartPosition(rt, 25.0)) {
			hr = pInner->GetTextureSize(p, MaxTextureSize, VirtualSize, VirtualTopLeft);
		}
		pInner->Unlock();
		if (SUCCEEDED(hr) && MaxTextureSize.cx > 0 && MaxTextureSize.cy > 0) {
			return S_OK;
		}
	}
	SIZE sz = {};
	CBDNavigator::Get().GetPlaneSize(sz);
	MaxTextureSize = sz;
	VirtualSize = sz;
	VirtualTopLeft = { 0, 0 };
	return S_OK;
}

STDMETHODIMP_(SUBTITLE_TYPE) CBDMenuSubPicProvider::GetType()
{
	return ST_HDMV;
}
