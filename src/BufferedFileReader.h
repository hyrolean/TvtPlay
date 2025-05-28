#ifndef INCLUDE_BUFFERED_FILE_READER_H
#define INCLUDE_BUFFERED_FILE_READER_H

#include "ReadOnlyFile.h"
#include "Util.h"
#include <list>

class CBufferedFileReader
{
public:
    static const int BUF_NUM_MAX = 128;
    CBufferedFileReader();
    ~CBufferedFileReader();
    void SetFile(IReadOnlyFile *file);
    bool SetupBuffer(int bufSize, int bufPreSize, int bufNum);
    void Flush(__int64 seekPos=-1);
    int Read(BYTE **ppBuf);
    int SyncRead(BYTE **ppBuf);
    __int64 GetFilePosition(__int64 *pSzPreserve=nullptr) const;
    __int64 GetFileSize() const;
    int GetBufferSize() const { return m_hThread ? static_cast<int>(m_queue.size() - 1) * m_bufSize : 0; }
    __int64 Seek(__int64 seekPos, IReadOnlyFile::MOVE_METHOD moveMethod);
    void SuspendReadIn() ;
    void ResumeReadIn() ;
    bool ReadyReadIn() const { return m_fRead; }
    void OrderReadIn() ;
private:
    static unsigned int __stdcall ReadThread(void *pParam);
    IReadOnlyFile *m_file;
    HANDLE m_hThread;
    HANDLE m_hThreadEvent;
    HANDLE m_hFileSizeEvent;
    HANDLE m_hReadInEvent;
    bool m_fStop;
    bool m_fRead;
    std::list<std::vector<BYTE>> m_queue;
    std::list<std::vector<BYTE>>::iterator m_tail;
    int m_bufSize;
    int m_bufPreSize;
    int m_bufPreSeek;
    __int64 m_fileSize;
    __int64 m_filePos;
    mutable recursive_mutex_ m_lock;
    mutable recursive_mutex_ m_lockRead;
};

#endif // INCLUDE_BUFFERED_FILE_READER_H
