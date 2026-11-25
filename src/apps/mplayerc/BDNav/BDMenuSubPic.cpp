/*
 * BDMenuSubPic.cpp - subpicture provider showing the Blu-ray menu (IG plane)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDMenuSubPic.h"
#include "BDNavigator.h"

// The menu is delivered as consecutive short windows so that a change only has to invalidate
// the windows after the current one (no blank frame -> no flicker), see CMainFrame::BDNavUpdateMenu.

static const REFERENCE_TIME kMenuStop = 1LL << 60;

CBDMenuSubPicProvider::CBDMenuSubPicProvider()
	: CSubPicProviderImpl(&m_csLock)
	, m_bSingle(g_bBDNavNoQueueApplied)
{
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetStartPosition(REFERENCE_TIME rt, double fps, bool CleanOld)
{
	if (!CBDNavigator::Get().IsOverlayOn()) {
		return nullptr;
	}
	if (m_bSingle) {
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
	return m_bSingle ? 0 : ((INT_PTR)pos - 1) * BDMENU_WINDOW;
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStop(POSITION pos, double fps)
{
	return m_bSingle ? kMenuStop : (INT_PTR)pos * BDMENU_WINDOW;
}

STDMETHODIMP_(bool) CBDMenuSubPicProvider::IsAnimated(POSITION pos)
{
	return false;
}

STDMETHODIMP CBDMenuSubPicProvider::Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox)
{
	return CBDNavigator::Get().RenderMenu(spd, bbox) ? S_OK : E_FAIL;
}

STDMETHODIMP CBDMenuSubPicProvider::GetTextureSize(POSITION pos, SIZE& MaxTextureSize, SIZE& VirtualSize, POINT& VirtualTopLeft)
{
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
