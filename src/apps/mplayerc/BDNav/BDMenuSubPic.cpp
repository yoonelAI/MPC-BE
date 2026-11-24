/*
 * BDMenuSubPic.cpp - subpicture provider showing the Blu-ray menu (IG plane)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDMenuSubPic.h"
#include "BDNavigator.h"

// The menu is delivered as consecutive short windows so that a change only has to invalidate
// the windows after the current one (no blank frame -> no flicker), see CMainFrame::BDNavUpdateMenu.

CBDMenuSubPicProvider::CBDMenuSubPicProvider()
	: CSubPicProviderImpl(&m_csLock)
{
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetStartPosition(REFERENCE_TIME rt, double fps, bool CleanOld)
{
	if (!CBDNavigator::Get().IsOverlayOn()) {
		return nullptr;
	}
	if (rt < 0) {
		rt = 0;
	}
	return (POSITION)(INT_PTR)(rt / BDMENU_WINDOW + 1);
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetNext(POSITION pos)
{
	return (POSITION)((INT_PTR)pos + 1);
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStart(POSITION pos, double fps)
{
	return ((INT_PTR)pos - 1) * BDMENU_WINDOW;
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStop(POSITION pos, double fps)
{
	return (INT_PTR)pos * BDMENU_WINDOW;
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
