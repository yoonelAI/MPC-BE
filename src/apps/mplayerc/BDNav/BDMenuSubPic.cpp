/*
 * BDMenuSubPic.cpp - subpicture provider showing the Blu-ray menu (IG plane)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDMenuSubPic.h"
#include "BDNavigator.h"

static const REFERENCE_TIME kMenuStop = 1LL << 60;

CBDMenuSubPicProvider::CBDMenuSubPicProvider()
	: CSubPicProviderImpl(&m_csLock)
{
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetStartPosition(REFERENCE_TIME rt, double fps, bool CleanOld)
{
	return CBDNavigator::Get().IsMenuVisible() ? (POSITION)1 : nullptr;
}

STDMETHODIMP_(POSITION) CBDMenuSubPicProvider::GetNext(POSITION pos)
{
	return nullptr;
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStart(POSITION pos, double fps)
{
	return 0;
}

STDMETHODIMP_(REFERENCE_TIME) CBDMenuSubPicProvider::GetStop(POSITION pos, double fps)
{
	return kMenuStop;
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
