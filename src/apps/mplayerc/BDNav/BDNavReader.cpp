/*
 * BDNavReader.cpp - DirectShow source filter for the libbluray navigation stream (MPC-BE BDMenu)
 * License: GPLv3 (same as MPC-BE)
 */

#include "stdafx.h"
#include "BDNavReader.h"
#include "BDNavigator.h"

#define BDNAV_MAXSTORESIZE  (3 * MEGABYTE)   // buffered TS data before the reader thread waits
#define BDNAV_KEEPBEHIND    (512 * KILOBYTE) // data kept behind the read position
#define BDNAV_READSIZE      (6144 * 8)        // 8 aligned units = 256 BD-TS packets (short lock hold)
// read at most this far ahead of playback (media time). The disc navigation (BD-J menus, HDMV timers,
// seamless menu loops) works on the read position: with low-bitrate menu clips the splitter would
// otherwise accept a minute of data and every menu action would be delayed by that much.
#define BDNAV_MAXAHEAD      (25000000LL)      // 2.5 s

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
	m_bSegmentEnd = FALSE;
	m_bDrainedPosted = FALSE;
	m_rtDelivered = -1;
	CAMThread::Create();
	if (FAILED(CAMThread::CallWorker(CMD_INIT))) {
		Clear();
		return false;
	}

	// give the navigation some time to produce data for stream probing
	// (BD-J: the Java VM and the disc's Xlets start first, the first playlist can take several seconds)
	auto& nav = CBDNavigator::Get();
	const DWORD wait = nav.GetStartWaitMs();
	const DWORD start = ::GetTickCount();
	while (::GetTickCount() - start < wait && m_len < 512 * KILOBYTE && !m_bEndOfStream && !m_bSegmentEnd && nav.IsOpen()) {
		Sleep(20);
	}
	if (nav.IsBDJDisc()) {
		CString m;
		m.Format(L"stream: first data after %u ms (%.1f MB)", ::GetTickCount() - start, m_len / 1048576.0);
		BDNavLog(m);
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

void CBDNavStream::TruncateUnread()
{
	// drop data that was buffered but not read yet (old position after a user jump)
	// NOTE: no m_csLock here: CAsyncRequest::Complete holds it while the splitter waits in Read()
	// for new data, which this (reader) thread has to deliver -> deadlock (P2-2, Poor Things)
	CAutoLock cPacketLock(&m_csPacketsLock);

	const ULONGLONG cut = ((m_pos + 191) / 192) * 192;
	{
		CAutoLock lockM(&m_csMarkers);
		while (!m_markers.empty() && m_markers.back().pos > cut) {
			m_markers.pop_back();
		}
	}
	if (cut >= m_len) {
		m_flushPos = (LONGLONG)m_len;
		InterlockedIncrement(&m_flushGen);
		return;
	}
	const ULONGLONG dropped = m_len - cut;
	while (!m_packets.empty() && m_packets.back().m_start >= cut) {
		m_packets.pop_back();
	}
	if (!m_packets.empty() && m_packets.back().m_end > cut) {
		m_packets.back().m_end = cut;
	}
	m_len = cut;
	m_flushPos = (LONGLONG)cut;
	InterlockedIncrement(&m_flushGen);

	CString msg;
	msg.Format(L"stream: dropped %.1f MB of buffered data after jump, flush #%ld at %llu", dropped / 1048576.0, m_flushGen, cut);
	BDNavLog(msg);
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
	std::vector<CBDNavigator::Marker> markers;

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

				while (!CheckRequest(nullptr) && !m_bEndOfStream && !m_bSegmentEnd) {
					if (!m_SizeComplete && GetPacketsSize() > BDNAV_MAXSTORESIZE) {
						Sleep(10);
						continue;
					}
					if (!m_SizeComplete && m_pOwner && m_RequestCmd == CMD_RUN) {
						const REFERENCE_TIME delivered = m_rtDelivered;
						if (delivered >= 0 && delivered - m_pOwner->GetStreamTimeNow() > BDNAV_MAXAHEAD) {
							Sleep(10);
							continue;
						}
					}

					bool bEvent = false;
					bool bFlush = false;
					markers.clear();
					const int n = nav.ReadTS(buff.get(), BDNAV_READSIZE, bEvent, bFlush, markers);
					if (bFlush) {
						TruncateUnread();
					}
					if (n == CBDNavigator::kReadSegmentEnd) {
						// format change: this graph gets no more data (EOF after the buffered data)
						m_bSegmentEnd = TRUE;
						m_EventComplete.Set();
						BDNavLog(L"stream: end of graph segment (format change)");
						break;
					}
					if (n < 0) {
						m_bEndOfStream = TRUE;
						m_EventComplete.Set();
						break;
					}
					if (!markers.empty()) {
						CAutoLock lockM(&m_csMarkers);
						for (const auto& mk : markers) {
							m_markers.push_back({ m_nextMarkerId++, m_len, mk.title90k, mk.dur90k, mk.playlist, mk.seq, mk.flags });
						}
						while (m_markers.size() > 64) {
							m_markers.pop_front();
						}
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

HRESULT CBDNavStream::GetNextMarker(LONG afterId, LONG* pId, LONGLONG* pPos)
{
	CAutoLock lockM(&m_csMarkers);
	for (const auto& m : m_markers) {
		if (m.id > afterId) {
			if (pId) *pId = m.id;
			if (pPos) *pPos = (LONGLONG)m.pos;
			return S_OK;
		}
	}
	return S_FALSE;
}

HRESULT CBDNavStream::SetMarkerTime(LONG id, REFERENCE_TIME rt)
{
	CBDNavigator::Marker mk = {};
	{
		CAutoLock lockM(&m_csMarkers);
		auto it = std::find_if(m_markers.begin(), m_markers.end(), [id](const CMarker& m) { return m.id == id; });
		if (it == m_markers.end()) {
			return E_INVALIDARG;
		}
		mk.title90k = it->title90k;
		mk.dur90k   = it->dur90k;
		mk.playlist = it->playlist;
		mk.seq      = it->seq;
		mk.flags    = it->flags;
		m_markers.erase(m_markers.begin(), it + 1);
	}
	CBDNavigator::Get().AddAnchor(rt, mk);
	return S_OK;
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
		while (!m_bEndOfStream && !m_bSegmentEnd && m_len < m_SizeComplete && !m_EventComplete.Wait(5));
		m_SizeComplete = 0;

		if (m_bSegmentEnd && m_pos >= m_len && !m_bDrainedPosted) {
			m_bDrainedPosted = TRUE;
			CBDNavigator::Get().OnSegmentDrained();
		}
		if ((m_bEndOfStream || m_bSegmentEnd) && m_pos >= m_len) {
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
	m_stream.m_pOwner = this;
	if (phr) {
		*phr = S_OK;
	}
}

CBDNavReader::~CBDNavReader()
{
	CBDNavigator::Get().ClearReader(this);
}

REFERENCE_TIME CBDNavReader::GetStreamTimeNow()
{
	if (m_State == State_Running) {
		CRefTime t;
		if (SUCCEEDED(StreamTime(t))) {
			m_rtLastStreamTime = t.m_time;
		}
	}
	return m_rtLastStreamTime;
}

int CBDNavReader::RemainingMs()
{
	const REFERENCE_TIME last = m_stream.GetDelivered();
	if (last < 0) {
		return -1;
	}
	const REFERENCE_TIME d = last - GetStreamTimeNow();
	return d > 0 ? (int)std::min<REFERENCE_TIME>(d / 10000, 600000) : 0;
}

bool CBDNavReader::IsPlayedOut()
{
	if (!m_stream.IsDrained()) {
		return false;
	}
	const REFERENCE_TIME last = m_stream.GetDelivered();
	if (last < 0) {
		return true;
	}
	const REFERENCE_TIME now = GetStreamTimeNow();
	if (now + 300000 >= last) {
		CString m;
		m.Format(L"stream: played out (stream %.3f s, last sample %.3f s)", now / 1e7, last / 1e7);
		BDNavLog(m);
		return true;
	}
	return false;   // within 30 ms of the last sample
}

STDMETHODIMP CBDNavReader::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
	CheckPointer(ppv, E_POINTER);

	return
		QI(IFileSourceFilter)
		QI(IBDNavSource)
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
	return __super::Stop();
}

STDMETHODIMP CBDNavReader::Pause()
{
	// the splitter prerolls in paused state: keep the navigation stream flowing
	if (m_stream.ThreadExists()) {
		m_stream.CallWorker(CBDNavStream::CMD_RUN);
	}
	GetStreamTimeNow();
	return __super::Pause();
}

STDMETHODIMP CBDNavReader::Run(REFERENCE_TIME tStart)
{
	if (m_stream.ThreadExists()) {
		m_stream.CallWorker(CBDNavStream::CMD_RUN);
	}
	return __super::Run(tStart);
}

STDMETHODIMP CBDNavReader::Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE* pmt)
{
	auto& nav = CBDNavigator::Get();
	if (!pszFileName || !nav.IsOpen() || nav.GetMarkerPath().CompareNoCase(pszFileName) != 0) {
		BDNavLog(L"CBDNavReader::Load: no active navigation session for " + CString(pszFileName ? pszFileName : L"(null)"));
		return E_FAIL;
	}
	nav.BeginSegment();
	if (!nav.StartPlayback()) {
		return E_FAIL;
	}
	if (!m_stream.Start()) {
		BDNavLog(L"CBDNavReader::Load: navigation produced no data");
		return E_FAIL;
	}

	// virtual ...\BDMV\STREAM\<clip>.m2ts path: the MPEG splitter reads the clip info (stream types) next to it
	nav.SetReader(this);
	m_fn = nav.PrepareClipInfoPath();
	if (m_fn.IsEmpty()) {
		m_fn = pszFileName;
	}
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
