#include <Windows.h>
#include <process.h>
#include "BufferedFileReader.h"

CBufferedFileReader::CBufferedFileReader()
    : m_file(nullptr)
    , m_hThread(nullptr)
{
}

CBufferedFileReader::~CBufferedFileReader()
{
    SetFile(nullptr);
}

// ファイルを設定する
void CBufferedFileReader::SetFile(IReadOnlyFile *file)
{
    SetupBuffer(0, 0, 0);
    m_file = file;
    if (m_file) {
        m_fileSize = m_file->GetSize();
    }
}

// バッファを設定する
// bufPreSizeだけ、Read()で返されるポインタより手前にも確保される
bool CBufferedFileReader::SetupBuffer(int bufSize, int bufPreSize, int bufNum)
{
    if (m_hThread) {
        m_fStop = true;
        ::SetEvent(m_hThreadEvent);
        ::WaitForSingleObject(m_hThread, INFINITE);
        ::CloseHandle(m_hThread);
        ::CloseHandle(m_hThreadEvent);
        if(m_hFileSizeEvent) ::CloseHandle(m_hFileSizeEvent);
        if(m_hReadInEvent) ::CloseHandle(m_hReadInEvent);
        m_hThread = nullptr;
    }
    m_queue.clear();
    if (m_file && bufSize >= 1 && bufPreSize >= 0 && bufNum >= 1) {
        // 返却状態のものを1つ余分に確保
        for (int i = 0; i < min(bufNum, BUF_NUM_MAX) + 1; ++i) {
            m_queue.resize(m_queue.size() + 1);
            m_queue.back().reserve(bufPreSize + bufSize);
        }
        m_tail = m_queue.begin();
        m_bufSize = bufSize;
        m_bufPreSize = bufPreSize;
        m_fRead = false;
        m_hThreadEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_hFileSizeEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_hReadInEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_hThreadEvent) {
            m_fStop = false;
            m_hThread = reinterpret_cast<HANDLE>(::_beginthreadex(nullptr, 0, ReadThread, this, 0, nullptr));
            if (!m_hThread) {
                ::CloseHandle(m_hThreadEvent);
                if(m_hFileSizeEvent) ::CloseHandle(m_hFileSizeEvent);
            }
        }
    }
    return m_hThread != nullptr;
}

// 先読みを終了する
void CBufferedFileReader::Flush()
{
    if (m_hThread) {
        lock_recursive_mutex rdlock(m_lockRead);
        lock_recursive_mutex lock(m_lock);
        __int64 rewind = 0;
        while (m_tail != m_queue.begin()) {
            rewind += static_cast<int>((--m_tail)->size()) - m_bufPreSize;
        }
        if (rewind > 0) {
            m_file->SetPointer(-rewind, IReadOnlyFile::MOVE_METHOD_CURRENT);
        }
        m_fRead = false;
    }
}

// 先読みを開始し、結果を1つ受け取る
// 先読み中はSetFile()に渡したfileにアクセスしてはいけない
int CBufferedFileReader::Read(BYTE **ppBuf)
{
    if (m_hThread) {
        lock_recursive_mutex locker(m_lock);
        if(m_tail == m_queue.begin()) {
            locker.unlock();
            lock_recursive_mutex rdlocker(m_lockRead);
            if(m_tail == m_queue.begin()) {
                int numRead = SyncRead(ppBuf);
                m_fRead = true;
                ::SetEvent(m_hThreadEvent);
                return numRead;
            }
            locker.lock();
        }
        m_queue.splice(m_queue.end(), m_queue, m_queue.begin());
        *ppBuf = m_queue.back().data() + m_bufPreSize;
        ::SetEvent(m_hThreadEvent);
        return static_cast<int>(m_queue.back().size()) - m_bufPreSize;
    }
    return -1;
}

// 先読みを終了し、同期読み込みの結果を1つ受け取る
int CBufferedFileReader::SyncRead(BYTE **ppBuf)
{
    if (m_hThread) {
        Flush();
        m_queue.back().resize(m_bufPreSize + m_bufSize);
        *ppBuf = &m_queue.back()[m_bufPreSize];
        return m_file->Read(*ppBuf, m_bufSize);
    }
    return -1;
}

// 現在のファイルポインタの位置を取得する
__int64 CBufferedFileReader::GetFilePosition() const
{
    if (m_file) {
        lock_recursive_mutex lock(m_lock);
        __int64 rewind = 0;
        if (m_hThread) {
            for (auto it = m_tail; it != m_queue.begin(); ) {
                rewind += static_cast<int>((--it)->size()) - m_bufPreSize;
            }
        }
        return m_file->SetPointer(0, IReadOnlyFile::MOVE_METHOD_CURRENT) - rewind;
    }
    return -1;
}

// 現在のファイルサイズを取得する
__int64 CBufferedFileReader::GetFileSize() const
{
    if (m_file) {
        if(m_file->IsShareWrite()&&m_hFileSizeEvent)
            ::SetEvent(m_hFileSizeEvent);
        if (m_fileSize < 0) {
            lock_recursive_mutex lock(m_lock);
            return m_file->GetSize();
        }
        return m_fileSize;
    }
    return -1;
}

void CBufferedFileReader::SuspendReadIn()
{
    m_lockRead.lock();
    m_lock.lock();
}

void CBufferedFileReader::ResumeReadIn()
{
    m_lock.unlock();
    m_lockRead.unlock();
}

void CBufferedFileReader::OrderReadIn()
{
    if (m_hThread) {
        if(m_hThreadEvent) {
          bool wait = false ;
          if(!m_fRead) {
            m_fRead = true ;
            if(m_hReadInEvent) {
              ::ResetEvent(m_hReadInEvent);
              wait = true ;
            }
          }
          ::SetEvent(m_hThreadEvent);
          if(wait) {
            ::WaitForSingleObject(m_hReadInEvent,3000);
          }
        }
    }
}

unsigned int __stdcall CBufferedFileReader::ReadThread(void *pParam)
{
    CBufferedFileReader &this_ = *static_cast<CBufferedFileReader*>(pParam);
    bool lts = false ;
    for (;;) {
        HANDLE handles[2];
        handles[0]=this_.m_hThreadEvent;
        handles[1]=this_.m_hFileSizeEvent;
        DWORD r = ::WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        if (this_.m_fStop) {
            break;
        }
        switch(r) {
        case WAIT_OBJECT_0: // file reading to buffer
            while(!this_.m_fStop) {
                lock_recursive_mutex rdlocker(this_.m_lockRead); // 読込処理のﾛｯｸ
                lock_recursive_mutex locker(this_.m_lock); // 排他処理のﾛｯｸ
                if (this_.m_fRead && std::next(this_.m_tail, 1) != this_.m_queue.end()) {
                    this_.m_tail->resize(this_.m_bufPreSize + this_.m_bufSize);
                    // 読込処理中は、排他処理のﾛｯｸを解放
                    // ( ﾛｯｸを解放しない場合、CBufferedFileReader::Readﾒﾝﾊﾞ関数が
                    //   下記の読込処理を完結するまで待機状態になる為、結果的にﾊﾞｯﾌｧ効率の悪化に繋がる )
                    locker.unlock();
                    int numRead = this_.m_file->Read(&(*this_.m_tail)[this_.m_bufPreSize], this_.m_bufSize);
                    if (numRead >= 0) {
                        locker.lock();
                        (this_.m_tail++)->resize(this_.m_bufPreSize + numRead);
                        if (numRead > 0) {
                            //::SetEvent(this_.m_hThreadEvent);
                            if(this_.m_hReadInEvent) ::SetEvent(this_.m_hReadInEvent);
                            continue ;
                        }
                    }
                    lts = numRead < this_.m_bufSize ;
                }
                if(this_.m_hReadInEvent) ::SetEvent(this_.m_hReadInEvent);
                break;
            }
            break;
        case WAIT_OBJECT_0+1: { // file size updation
                lock_recursive_mutex locker(this_.m_lock);
                // Linux Samba 環境で上記の読込処理中にCBufferedFileReader::GetFileSizeﾒﾝﾊﾞ関数が
                // m_file->GetSizeをｺｰﾙすると酷くもたつくことがある為、ｺｺでﾌｧｲﾙｻｲｽﾞの更新を行う
                // ( Debian Jessie + Samba + ASM1153 usb storage 環境の NanoPI NEO 2 ｻｰﾊﾞｰに.tsﾌｧｲﾙを置いた状態で検証済 )
                if(lts||(size_t)std::distance(this_.m_queue.begin(),this_.m_tail)>=this_.m_queue.size()/2) {
                    this_.m_fileSize = this_.m_file->GetSize();
                    lts = false ;
                }
            }
            break;
        }
    }
    return 0;
}
