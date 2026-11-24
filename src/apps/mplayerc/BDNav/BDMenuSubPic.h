/*
 * BDMenuSubPic.h - subpicture provider showing the Blu-ray menu (IG plane) on any MPC-BE renderer
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include "SubPic/SubPicProviderImpl.h"

#define BDMENU_WINDOW 800000LL   // 80 ms subpicture windows

class CBDMenuSubPicProvider : public CSubPicProviderImpl
{
	CCritSec m_csLock;

public:
	CBDMenuSubPicProvider();

	// ISubPicProvider
	STDMETHODIMP_(POSITION) GetStartPosition(REFERENCE_TIME rt, double fps, bool CleanOld = false) override;
	STDMETHODIMP_(POSITION) GetNext(POSITION pos) override;
	STDMETHODIMP_(REFERENCE_TIME) GetStart(POSITION pos, double fps) override;
	STDMETHODIMP_(REFERENCE_TIME) GetStop(POSITION pos, double fps) override;
	STDMETHODIMP_(bool) IsAnimated(POSITION pos) override;
	STDMETHODIMP Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox) override;
	STDMETHODIMP GetTextureSize(POSITION pos, SIZE& MaxTextureSize, SIZE& VirtualSize, POINT& VirtualTopLeft) override;
	STDMETHODIMP_(SUBTITLE_TYPE) GetType() override;
};
