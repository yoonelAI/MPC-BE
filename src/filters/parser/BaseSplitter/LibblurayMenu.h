/*
 * (C) 2026 - MPC-BE BD menu project
 * GPLv3
 */

#pragma once

#include <Unknwn.h>

// Exposed by CMpegSplitterFilter when a libbluray disc reader is active.
// Coordinates for MouseSelect are normalized to 0..65535.
interface __declspec(uuid("0ecba3e1-d97d-496e-9916-76083e97bbf5")) ILibblurayMenu : public IUnknown
{
	// Returns a snapshot of the pre-multiplied ARGB (BGRA byte order) menu plane.
	STDMETHOD(GetOverlay)(const BYTE** ppBits, ULONG* pWidth, ULONG* pHight,
		ULONG* pVersion, BOOL* pVisible) PURE;
	STDMETHOD(MouseSelect)(WORD x, WORD y, BOOL bActivate) PURE;
	STDMETHOD(UserInput)(DWORD key) PURE;
	STDMETHOD(MenuCall)() PURE;
	STDMETHOD(IsMenuMode)(BOOL* pMenuMode) PURE;
};
