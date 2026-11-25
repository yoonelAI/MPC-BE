/*
 * BDNavReader.h - DirectShow source filter feeding the libbluray navigation stream (MPEG-TS)
 * into MPC-BE's MPEG splitter. Modelled on MPC Stream Reader (CUDPReader / CLiveStream).
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include <deque>
#include <ExtLib/AsyncReader/asyncio.h>
#include <ExtLib/AsyncReader/asyncrdr.h>
#include <IBDNavSource.h>

#define BDNavReaderName L"MPC BD Navigation Source"

class CBDNavStream
	: public CAsyncStream
	, public CAMThread
{
	struct CPacket {
		std::unique_ptr<BYTE[]> m_buff;
		ULONGLONG m_start, m_end;
		CPacket(const BYTE* p, ULONGLONG start, UINT size);
	};

	CCritSec            m_csLock;
	CCritSec            m_csPacketsLock;
	std::deque<CPacket> m_packets;

	DWORD               m_RequestCmd = 0;
	ULONGLONG           m_pos = 0;
	ULONGLONG           m_len = 0;
	CAMEvent            m_EventComplete;
	volatile ULONGLONG  m_SizeComplete = 0;
	volatile BOOL       m_bEndOfStream = FALSE;
	volatile BOOL       m_bSegmentEnd = FALSE;   // format change: no more data for this graph
	volatile BOOL       m_bDrainedPosted = FALSE;
	volatile LONGLONG   m_rtDelivered = -1;

	volatile LONG       m_flushGen = 0;
	volatile LONGLONG   m_flushPos = 0;

	struct CMarker {
		LONG      id;
		ULONGLONG pos;
		uint64_t  title90k, dur90k;
		int       playlist;
		uint32_t  seq;
	};
	CCritSec            m_csMarkers;
	std::deque<CMarker> m_markers;
	LONG                m_nextMarkerId = 1;

	void Append(const BYTE* buff, UINT len);
	ULONGLONG GetPacketsSize();
	void CheckBuffer();
	void EmptyBuffer();
	void TruncateUnread();

	DWORD ThreadProc() override;

public:
	enum CMD {
		CMD_INIT,
		CMD_RUN,
		CMD_STOP,
		CMD_EXIT
	};

	CBDNavStream() = default;
	virtual ~CBDNavStream();

	bool Start();
	void Clear();

	// CAsyncStream
	HRESULT SetPointer(LONGLONG llPos) override;
	HRESULT Read(PBYTE pbBuffer, DWORD dwBytesToRead, BOOL bAlign, LPDWORD pdwBytesRead) override;
	LONGLONG Size(LONGLONG* pSizeAvailable) override;

	LONG GetFlushGeneration(LONGLONG* pPos) { if (pPos) *pPos = m_flushPos; return m_flushGen; }
	HRESULT GetNextMarker(LONG afterId, LONG* pId, LONGLONG* pPos);
	HRESULT SetMarkerTime(LONG id, REFERENCE_TIME rt);
	void SetDelivered(REFERENCE_TIME rt) { m_rtDelivered = rt; }
	REFERENCE_TIME GetDelivered() const { return m_rtDelivered; }
	bool IsDrained() const { return !!m_bDrainedPosted; }
	DWORD Alignment() override;
	void Lock() override;
	void Unlock() override;
};

class __declspec(uuid("6B1C2A51-8E1F-4C55-9D2B-BD0A7E11C0DE"))
	CBDNavReader
	: public CAsyncReader
	, public IFileSourceFilter
	, public IBDNavSource
{
	CBDNavStream m_stream;
	CStringW     m_fn;
	REFERENCE_TIME m_rtLastStreamTime = 0;

public:
	CBDNavReader(IUnknown* pUnk, HRESULT* phr);
	~CBDNavReader();

	REFERENCE_TIME GetStreamTimeNow();

	DECLARE_IUNKNOWN
	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);

	// CBaseFilter
	STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo);
	STDMETHODIMP Stop();
	STDMETHODIMP Pause();
	STDMETHODIMP Run(REFERENCE_TIME tStart);

	// IFileSourceFilter
	STDMETHODIMP Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE* pmt);
	STDMETHODIMP GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt);

	// IBDNavSource
	STDMETHODIMP_(LONG) GetFlushGeneration(LONGLONG* pFlushPos) { return m_stream.GetFlushGeneration(pFlushPos); }
	STDMETHODIMP GetNextMarker(LONG afterId, LONG* pId, LONGLONG* pPos) { return m_stream.GetNextMarker(afterId, pId, pPos); }
	STDMETHODIMP SetMarkerTime(LONG id, REFERENCE_TIME rt) { return m_stream.SetMarkerTime(id, rt); }
	STDMETHODIMP SetDeliveredTime(REFERENCE_TIME rt) { m_stream.SetDelivered(rt); return S_OK; }

	// graph segment played out: all data read and the stream time passed the last delivered sample
	bool IsPlayedOut();
	// ms of delivered data not yet played, -1 = unknown
	int  RemainingMs();
};
