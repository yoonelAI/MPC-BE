/*
 * BDMenuSubPic.h - subpicture provider showing the Blu-ray menu (IG plane) on any MPC-BE renderer.
 * The current subtitle (PGS from the splitter, external subtitles) is rendered underneath, so that
 * subtitles stay visible while a pop-up menu is shown (as on a Blu-ray player).
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include "SubPic/SubPicProviderImpl.h"

#define BDMENU_WINDOW 200000LL   // 20 ms subpicture windows

class CBDMenuSubPicProvider : public CSubPicProviderImpl
{
	CCritSec m_csLock;
	const bool m_bSingle;   // synchronous queue: one subpicture per segment, invalidated on change

	CCritSec m_csInner;
	CComPtr<ISubPicProvider> m_pInner;   // subtitle shown under the menu (may be null)

	// segment returned by GetStartPosition (synchronous queue)
	REFERENCE_TIME m_segStart = 0;
	REFERENCE_TIME m_segStop = 0;
	bool m_bInnerAnimated = false;

	CComPtr<ISubPicProvider> GetInner();
	// start / stop of the segment at rt: bounded by the subtitle's own changes
	void InnerSegment(ISubPicProvider* pInner, REFERENCE_TIME rt, double fps, REFERENCE_TIME& stop, bool& animated);

public:
	CBDMenuSubPicProvider();
	bool IsSingle() const { return m_bSingle; }
	void SetInner(ISubPicProvider* pInner);

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
