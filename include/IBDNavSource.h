/*
 * IBDNavSource.h - private interface between the MPC-BE BDMenu navigation source
 * (CBDNavReader) and the MPEG splitter.
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

// The navigation source increments the flush generation when the user made the disc jump
// (menu button, top menu, chapter). Bytes from FlushPos on belong to the new position:
// the splitter flushes everything downstream when its read position reaches FlushPos
// and re-times the new data to the current stream time.
interface __declspec(uuid("A4F3C1D2-6B7E-4F0A-9C3D-BD0A7E11F1A5"))
IBDNavSource : public IUnknown
{
	STDMETHOD_(LONG, GetFlushGeneration)(LONGLONG* pFlushPos) PURE;

	// Time markers (title/playlist start, seek points). The splitter reports the stream time
	// of the first packet at/after the marker byte position -> seek bar / time display.
	STDMETHOD(GetNextMarker)(LONG afterId, LONG* pId, LONGLONG* pPos) PURE;   // S_OK if a marker with id > afterId exists
	STDMETHOD(SetMarkerTime)(LONG id, REFERENCE_TIME rt) PURE;
};
