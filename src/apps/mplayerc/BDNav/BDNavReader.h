/*
 * BDNavReader.h - DirectShow source filter feeding the libbluray navigation stream (MPEG-TS)
 * into MPC-BE's MPEG splitter. Modelled on MPC Stream Reader (CUDPReader / CLiveStream).
 * License: GPLv3 (same as MPC-BE)
 */

#pragma once

#include <deque>
#include <ExtLib/AsyncReader/asyncio.h>
#include <ExtLib/AsyncReader/asyncrdr.h>

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

	void Append(const BYTE* buff, UINT len);
	ULONGLONG GetPacketsSize();
	void CheckBuffer();
	void EmptyBuffer();

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
	DWORD Alignment() override;
	void Lock() override;
	void Unlock() override;
};

class __declspec(uuid("6B1C2A51-8E1F-4C55-9D2B-BD0A7E11C0DE"))
	CBDNavReader
	: public CAsyncReader
	, public IFileSourceFilter
{
	CBDNavStream m_stream;
	CStringW     m_fn;

public:
	CBDNavReader(IUnknown* pUnk, HRESULT* phr);
	~CBDNavReader() = default;

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
};
