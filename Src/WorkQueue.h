#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <windows.h>
#undef max
#undef min

#include <deque>
#include <algorithm>

#include "Globals.h"

/// A queue of T, with thread synchronization
template<typename T>
class WorkQueue
{
public:
    WorkQueue() :
        mPort(NULL)
    {
        mPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        Assert(mPort);
    }
    
    ~WorkQueue()
    {
        SafeCloseHandle(mPort);
    }
    
    void push_back(const T& t)
    {
        Assert(PostQueuedCompletionStatus(mPort, sizeof(t), *reinterpret_cast<const DWORD_PTR*>(&t), NULL));
    }
    
    T pop_front()
    {
        DWORD numberOfBytes = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = NULL;

        StaticAssert(sizeof(T) <= sizeof(completionKey));

        Assert(GetQueuedCompletionStatus(mPort, &numberOfBytes, &completionKey, &overlapped, INFINITE));

        return(*reinterpret_cast<T*>(&completionKey));
    }

    bool try_pop_front(T& t, size_t milliseconds = 0)
    {
        DWORD numberOfBytes = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = NULL;
        
        StaticAssert(sizeof(T) <= sizeof(completionKey));

        if(GetQueuedCompletionStatus(mPort, &numberOfBytes, &completionKey, &overlapped, static_cast<DWORD>(milliseconds)))
        {
            t = *reinterpret_cast<T*>(&completionKey);
            return(true);
        }

        return(false);
    }
    
private:    
    HANDLE mPort;
};

#endif // WORK_QUEUE_H
