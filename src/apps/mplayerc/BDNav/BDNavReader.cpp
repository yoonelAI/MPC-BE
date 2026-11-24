/*
 * BDNavReader.cpp - DirectShow source filter for the libbluray navigation stream (MPC-BE BDMenu)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDNavReader.h"
#include "BDNavigator.h"

#define BDNAV_MAXSTORESIZE  (8 * MEGABYTE)   // buffered TS data before the reader thread waits
#define BDNAV_KEEPBEHIND    (512 * KILOBYTE) // data kept behind the read position
#define BDNAV_READSIZE      (188 * 1024)

//
// CBDNavStream
//

CBDNavStream::CPacket::CPacket(const BYTE* p, ULONGLONG start, UINT size)
	: m_start(start)
	, m_end(start + size)
{
	m_buff = std::make_unique<BYTE[]>(size);
	memcpy(m_buff.get(), p, size);
}

CBDNavStream::~CBDNavStream()
{
	Clear();
}

void CBDNavStream::Clear()
{
	if (CAMThread::ThreadExists()) {
		CAMThread::CallWorker(CMD_EXIT);
		CAMThread::Close();
	}
	EmptyBuffer();
	m_pos = m_len = 0;
	m_SizeComplete = 0;
}

bool CBDNavStream::Start()
{
	m_bEndOfStream = FALSE;
	CAMThread::Create();
	if (FAILED(CAMThread::CallWorker(CMD_INIT))) {
		Clear();
		return false;
	}

	// give the navigation some time to produce data for stream probing
	const DWORD start = ::GetTickCount();
	while (::GetTickCount() - start < 3000 && m_len < 512 * KILOBYTE && !m_bEndOfStream) {
		Sleep(20);
	}
	return m_len > 0;
}

void CBDNavStream::Append(const BYTE* buff, UINT len)
{
	CAutoLock cPacketLock(&m_csPacketsLock);

	m_packets.emplace_back(buff, m_len, len);
	m_len += len;

	if (m_SizeComplete && m_len >= m_SizeComplete) {
		m_EventComplete.Set();
	}
}

ULONGLONG CBDNavStream::GetPacketsSize()
{
	CAutoLock cPacketLock(&m_csPacketsLock);
	return m_packets.empty() ? 0 : m_packets.back().m_end - m_packets.front().m_start;
}

void CBDNavStream::CheckBuffer()
{
	if (m_RequestCmd == CMD_RUN) {
		CAutoLock cPacketLock(&m_csPacketsLock);
		if (m_pos > BDNAV_KEEPBEHIND) {
			while (!m_packets.empty() && m_packets.front().m_end < m_pos - BDNAV_KEEPBEHIND) {
				m_packets.pop_front();
			}
		}
	}
}

void CBDNavStream::EmptyBuffer()
{
	CAutoLock cPacketLock(&m_csPacketsLock);
	m_packets.clear();
	m_len = m_pos;
}

DWORD CBDNavStream::ThreadProc()
{
	SetThreadPriority(m_hThread, THREAD_PRIORITY_ABOVE_NORMAL);

	auto& nav = CBDNavigator::Get();
	auto buff = std::make_unique<BYTE[]>(BDNAV_READSIZE);

	for (;;) {
		m_RequestCmd = GetRequest();

		switch (m_RequestCmd) {
			default:
			case CMD_EXIT:
				Reply(S_OK);
				m_bEndOfStream = TRUE;
				m_EventComplete.Set();
				return 0;
			case CMD_STOP:
				Reply(S_OK);
				break;
			case CMD_INIT:
			case CMD_RUN:
				Reply(S_OK);

				while (!CheckRequest(nullptr) && !m_bEndOfStream) {
					if (!m_SizeComplete && GetPacketsSize() > BDNAV_MAXSTORESIZE) {
						Sleep(10);
						continue;
					}

					bool bEvent = false;
					const int n = nav.ReadTS(buff.get(), BDNAV_READSIZE, bEvent);
					if (n < 0) {
						m_bEndOfStream = TRUE;
						m_EventComplete.Set();
						break;
					}
					if (n > 0) {
						Append(buff.get(), (UINT)n);
					} else if (!bEvent) {
						Sleep(10); // still frame / waiting: keep the navigation VM running
					}
				}
				break;
		}
	}

	return 0;
}

HRESULT CBDNavStream::SetPointer(LONGLONG llPos)
{
	CAutoLock cAutoLock(&m_csLock);

	if (llPos < 0) {
		return E_FAIL;
	}

	m_pos = llPos;

	CAutoLock cPacketLock(&m_csPacketsLock);
	const __int64 start = m_packets.empty() ? 0 : m_packets.front().m_start;
	if (llPos < start) {
		return S_FALSE;
	}
	return S_OK;
}

HRESULT CBDNavStream::Read(PBYTE pbBuffer, DWORD dwBytesToRead, BOOL bAlign, LPDWORD pdwBytesRead)
{
	DWORD len = dwBytesToRead;
	BYTE* ptr = pbBuffer;

	if (m_pos + len > m_len) {
		m_SizeComplete = m_pos + len;
		while (!m_bEndOfStream && m_len < m_SizeComplete && !m_EventComplete.Wait(5));
		m_SizeComplete = 0;

		if (m_bEndOfStream && m_pos >= m_len) {
			if (pdwBytesRead) {
				*pdwBytesRead = 0;
			}
			return E_FAIL;
		}
	}

	CAutoLock cAutoLock(&m_csLock);
	CAutoLock cPacketLock(&m_csPacketsLock);

	auto it = m_packets.cbegin();
	while (it != m_packets.cend() && len > 0) {
		const auto& p = *it++;
		if (p.m_start <= m_pos && m_pos < p.m_end) {
			const DWORD size = (DWORD)std::min<ULONGLONG>(len, p.m_end - m_pos);
			memcpy(ptr, &p.m_buff.get()[m_pos - p.m_start], size);
			m_pos += size;
			ptr += size;
			len -= size;
		}
	}

	if (pdwBytesRead) {
		*pdwBytesRead = (DWORD)(ptr - pbBuffer);
	}

	CheckBuffer();

	return len == dwBytesToRead ? E_FAIL : (len > 0 ? S_FALSE : S_OK);
}

LONGLONG CBDNavStream::Size(LONGLONG* pSizeAvailable)
{
	CAutoLock cAutoLock(&m_csLock);
	if (pSizeAvailable) {
		*pSizeAvailable = m_len;
	}
	return 0; // unknown total size -> streaming mode in the splitter
}

DWORD CBDNavStream::Alignment()
{
	return 1;
}

void CBDNavStream::Lock()
{
	m_csLock.Lock();
}

void CBDNavStream::Unlock()
{
	m_csLock.Unlock();
}

//
// CBDNavReader
//

CBDNavReader::CBDNavReader(IUnknown* pUnk, HRESULT* phr)
	: CAsyncReader(L"CBDNavReader", pUnk, &m_stream, phr, __uuidof(this))
{
	if (phr) {
		*phr = S_OK;
	}
}

STDMETHODIMP CBDNavReader::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
	CheckPointer(ppv, E_POINTER);

	return
		QI(IFileSourceFilter)
		__super::NonDelegatingQueryInterface(riid, ppv);
}

STDMETHODIMP CBDNavReader::QueryFilterInfo(FILTER_INFO* pInfo)
{
	CheckPointer(pInfo, E_POINTER);
	ValidateReadWritePtr(pInfo, sizeof(FILTER_INFO));

	wcscpy_s(pInfo->achName, BDNavReaderName);
	pInfo->pGraph = m_pGraph;
	if (m_pGraph) {
		m_pGraph->AddRef();
	}
	return S_OK;
}

STDMETHODIMP CBDNavReader::Stop()
{
	if (m_stream.ThreadExists()) {
		m_stream.CallWorker(CBDNavStream::CMD_STOP);
	}
	return S_OK;
}

STDMETHODIMP CBDNavReader::Pause()
{
	// the splitter prerolls in paused state: keep the navigation stream flowing
	if (m_stream.ThreadExists()) {
		m_stream.CallWorker(CBDNavStream::CMD_RUN);
	}
	return S_OK;
}

STDMETHODIMP CBDNavReader::Run(REFERENCE_TIME tStart)
{
	if (m_stream.ThreadExists()) {
		m_stream.CallWorker(CBDNavStream::CMD_RUN);
	}
	return S_OK;
}

STDMETHODIMP CBDNavReader::Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE* pmt)
{
	auto& nav = CBDNavigator::Get();
	if (!pszFileName || !nav.IsOpen() || nav.GetMarkerPath().CompareNoCase(pszFileName) != 0) {
		BDNavLog(L"CBDNavReader::Load: no active navigation session for " + CString(pszFileName ? pszFileName : L"(null)"));
		return E_FAIL;
	}
	if (!nav.StartPlayback()) {
		return E_FAIL;
	}
	if (!m_stream.Start()) {
		BDNavLog(L"CBDNavReader::Load: navigation produced no data");
		return E_FAIL;
	}

	m_fn = pszFileName;
	m_mt.majortype = MEDIATYPE_Stream;
	m_mt.subtype   = MEDIASUBTYPE_MPEG2_TRANSPORT;
	return S_OK;
}

STDMETHODIMP CBDNavReader::GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt)
{
	CheckPointer(ppszFileName, E_POINTER);

	const size_t nCount = m_fn.GetLength() + 1;
	*ppszFileName = (LPOLESTR)CoTaskMemAlloc(nCount * sizeof(WCHAR));
	CheckPointer(*ppszFileName, E_OUTOFMEMORY);
	wcscpy_s(*ppszFileName, nCount, m_fn);
	return S_OK;
}
