#ifndef GLOBALS_H
#define GLOBALS_H

#include <cstring>
#include <cassert>

namespace Util
{
    __forceinline bool True()
    {
        return(true);
    }

    __forceinline bool False()
    {
        return(false);
    }
    __forceinline bool Debug()
    {
#ifdef _DEBUG
        return(true);
#else
        return(false);
#endif
    }

    __forceinline bool Release()
    {
#ifdef NDEBUG
        return(true);
#else
        return(false);
#endif
    }
    
    __forceinline void Zeroize(void* memory, size_t size)
    {
        memset(memory, 0, size);
    }
    
    inline TCHAR* FindExtension(TCHAR* str)
    {
        str = _tcschr(str, '.');
        
        if(!str)
        {
            return(NULL);
        }
        
        for(;;)
        {
            TCHAR* q = _tcschr(str + 1, '.');
            
            if(q)
            {
                str = q;
            }
            else
            {
                return(str);
            }
        }
    }
}

#include <assert.h>
#ifdef NDEBUG
#define Assert(x) if(x){}
#else
#define Assert(x) assert(x)
#endif

#define ARRAY_COUNT(a)  (sizeof(a) / sizeof(a[0]))

template<typename T>
inline void SafeCloseHandle(T& p)
{
    if(p)
    {
        Assert(CloseHandle(p));
        p = 0;
    }
}

template<typename T>
inline void SafeRelease(T*& p)
{
    if(p)
    {
        p->Release();
        p = 0;
    }
}

const size_t INVALID = static_cast<size_t>(-1);

inline float Sqr(float x)
{
    return(x * x);
}

#define StaticAssert(pred) switch(0){case 0:case pred:;}

#endif // GLOBALS_H
