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

inline void SafeDestroyWindow(HWND& p)
{
    if(p)
    {
        DestroyWindow(p);
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

#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter

template<typename T>
class SIMDAllocator
{
public:
    SIMDAllocator()
    {
    }

    template <class Other>
    SIMDAllocator(const SIMDAllocator<Other>&)
    {
    }

    typedef T          value_type;
    typedef size_t     size_type;
    typedef ptrdiff_t  difference_type;

    typedef T*         pointer;
    typedef const T*   const_pointer;

    typedef T&         reference;
    typedef const T&   const_reference;

    pointer allocate(size_type actualCount, const void* = 0)
    {
        void* m = _aligned_malloc(actualCount * sizeof(T), 16);
        return(reinterpret_cast<T*>(m));
    }

    void deallocate(pointer pMemory, size_type)
    {
        _aligned_free(pMemory);
    }

    void construct(pointer p, const T& val)
    {
        new (p) T(val);
    }

    void destroy(pointer p)
    {
        p->~T();
    }
    
    size_type max_size() const
    {
        return(ULONG_MAX / sizeof(T));
    }

    template <class O>
    struct rebind
    {
        typedef SIMDAllocator<O> other ;
    };
};

#pragma warning(pop)

extern void OpenLog();
extern void CloseLog();
extern void Log(const char* format);
extern void Log(const TCHAR* format);
extern void LogF(const TCHAR* format, ...);

#endif // GLOBALS_H
