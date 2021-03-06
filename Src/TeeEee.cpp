#include "TeeEeePch.h"

#include "Movies.h"
#include "WorkQueue.h"
#include "Microphone.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <process.h>
#include <shellapi.h>
#include <Uxtheme.h>

#include <vlc/vlc.h>

#pragma warning(push)
#pragma warning(disable: 4324) // structure was padded due to __declspec(align())
#include <bson.h>
#include <mongoc.h>
#pragma warning(pop)

/*
    SSE microphone i16->fp
*/

#pragma comment(lib, "Dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Ws2_32.lib")

#pragma comment(lib, "libvlccore.lib")
#pragma comment(lib, "libvlc.lib")

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Uxtheme.lib")

#pragma comment(lib, "bson-static-1.0.lib")
#pragma comment(lib, "mongoc-static-1.0.lib")

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

// This causes memory corruption after I upgraded to VS2012 and VLC 2.2.1
// #define RECYCLE_PLAYER_INSTANCE

typedef std::deque<Movie*> ChannelMovies;

struct Channel
{
    Channel() :
        startedPlaying(0)
    {
    }

    ChannelMovies movies;
    __int64 startedPlaying;
};

static std::vector<Channel> sChannels;
    
const UINT SENSITIVITY_STEPS = 30;

const UINT TIMEOUT_STEPS = 6;
const UINT TIMEOUT_STEP_SIZE = 5;
static UINT sMinTimeout = TIMEOUT_STEP_SIZE;
const UINT MAX_TIMEOUT_SECONDS = 30;

// These are the inter-thread communication queues:
// A special request for NULL will cause the threads involved to shutdown.
static WorkQueue<Movie*> sLoadWorkQueue;

const UINT WM_LOADED_MOVIE = (WM_USER + 1); // LPARAM is Movie*
const UINT WM_MOVIE_COMPLETED = (WM_USER + 2);
const UINT WM_REMOTE_BUTTON = (WM_USER + 3); // WPARAM is index
const UINT WM_LENGTH_CHANGED = (WM_USER + 4);

const unsigned THREAD_STACK = 16 * 1024;

static HANDLE sLoadThread = NULL;

const TCHAR* CLASS_NAME = TEXT("TeeEeeMain");

static WNDCLASS sWindowClass;
static HWND sWindowHandle = NULL;
static HACCEL sAcceleratorTable = NULL;

static HANDLE sRemoteHandle = NULL;
static HANDLE sRemoteSem = NULL;
static HANDLE sRemoteThread = NULL;

static HFONT sLabelFont = NULL;

static int sMaxCoverDx = 0;
static int sMaxCoverDy = 0;

static Channel* sCurrentChannel = NULL;
static Movie* sBedTimeMovie = NULL;

const time_t PRELOAD_DECAY = 1;

const float VOLUME_QUANTUM = 1.f;

const float GAIN_HEADROOM = (-3.f * VOLUME_QUANTUM);

const float MIN_VOLUME = -3.f * VOLUME_QUANTUM;
const float MAX_VOLUME = 3.f * VOLUME_QUANTUM;

static float sVolume = 0.f; 

static float sMicrophoneSensitivityNormal = 0.f;
static float sMicrophoneSensitivityBedtime = 0.f;

const UINT_PTR ON_SIZE_TIMER = 1;
const UINT ON_SIZE_DELAY = 500; // 1/2 second

const UINT_PTR JOYSTICK_INPUT_TIMER = 2;
const UINT JOYSTICK_INPUT_DELAY = 1000 / 60; // 60Hz

const UINT_PTR SET_VOLUME_TIMER = 3;

#ifndef _DEBUG
const UINT SET_VOLUME_DELAY = 1000; // 1-second
#else
const UINT SET_VOLUME_DELAY = 16 * 1000; // 16-seconds
#endif

const UINT_PTR HIDE_CURSOR_TIMER = 4;
const UINT HIDE_CURSOR_DELAY = 10 * 1000; // 10 sec

const UINT_PTR TIMEOUT_TICK = 5;
const UINT TIMEOUT_TICK_MS = 16;

static time_t sPreviousShushTimes[16] = {0};
static size_t sPreviousShushIndex = 0;
static int sTimeoutIndex = -1;
static bool sLoading = false;

static libvlc_instance_t* sVlc = NULL;
static libvlc_media_player_t* sVlcPlayer = NULL;
static libvlc_time_t sPauseTime = -1;

static HWND sProgressBar = NULL;

// ms / 100-nanonseconds
// 1E-3 / 100E-9
// 1E-3 / 1E-7
// 1/E-4
// 1E4
const __int64 VLCTIME_TO_FILE_TIME = 10000;

enum PlayingState
{
    PM_IDLE,
    PM_PAUSED,
    PM_PLAYING,
    PM_LOADING,
    PM_SHUSH,
    PM_TIMEOUT
};

static PlayingState gPlayingState = PM_IDLE;

static IDirectInput8* sDirectInput = NULL;
static IDirectInputDevice8* sJoystick = NULL;
static DIJOYSTATE2 sPrevJoyState = {0};

const size_t SLEEP_INTERVALS = 4;
static time_t sSleepTime = 0; // If non-zero will sleep after this.
static time_t sSleepTimeStarted = 0;

static POINT sHideCursorPoint = {0};

static int Round(double f)
{
    return(static_cast<int>(floor(f + 0.5)));
}

static int Clamp(int a, int tMin, int tMax)
{
    int t = std::max(a, tMin);
    return(std::min(t, tMax));
}

static size_t CountRecentShushes(double interval)
{
    time_t now;
    time(&now);

    size_t shushCount = 0;
    
    for(size_t i = 0; i < ARRAY_COUNT(sPreviousShushTimes); ++i)
    {
        if(difftime(now, sPreviousShushTimes[i]) < interval)
        {
            ++shushCount;        
        }
    }
    
    Assert(shushCount > 0);
    return(shushCount);
}

static DWORD WINAPI LoadThread(void *)
{
    for(;;)
    {
        Movie* movie = sLoadWorkQueue.pop_front();
        
        if(!movie || !movie->coverPath[0])
        {
            break;
        }
            
        Assert(!movie->cover);

        HRESULT hr;

        IPicture* picture = NULL;
        hr = OleLoadPicturePath(movie->coverPath, NULL, 0, CLR_NONE, IID_IPicture, reinterpret_cast<LPVOID*>(&picture));
        if(FAILED(hr))
        {
            LogF(TEXT("Failed to load %s\n"), movie->coverPath);
            movie->state = Movie::MS_DORMANT;
            continue;
        }

        HBITMAP bitmap = NULL;
        hr = picture->get_Handle(reinterpret_cast<OLE_HANDLE*>(&bitmap));
        Assert(!FAILED(hr));

        BITMAP bitmapInfo = {0}; 
        Assert(GetObjectA(bitmap, sizeof(bitmapInfo), &bitmapInfo)); 
        long dx = bitmapInfo.bmWidth;
        long dy = bitmapInfo.bmHeight;
        float scale = 1.f;
        
        if(dx > sMaxCoverDx)
        {
            scale = std::min(scale, static_cast<float>(sMaxCoverDx) / static_cast<float>(dx));
        }
        
        if(dy > sMaxCoverDy)
        {
            scale = std::min(scale, static_cast<float>(sMaxCoverDy) / static_cast<float>(dy));
        }
        
        dx = static_cast<long>(static_cast<float>(dx) * scale);
        dy = static_cast<long>(static_cast<float>(dy) * scale);

        bitmap = reinterpret_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, dx, dy, LR_COPYRETURNORG));
        Assert(bitmap);
        
        Assert(!FAILED(picture->Release()));
    
        movie->cover = bitmap;
        
        LogF(TEXT("Loaded %s\n"), movie->name);
        
        movie->state = Movie::MS_LOADED;
        
        PostMessage(sWindowHandle, WM_LOADED_MOVIE, 0, reinterpret_cast<LPARAM>(movie));
    }
    
    return(0);
}

static void LoadChannelCovers()
{
    std::vector<Movie*> neededSet;
    neededSet.reserve(sChannels.size() + 1);

    for(size_t i = 0; i < sChannels.size(); ++i)
    {
        if(!sChannels[i].movies.empty())
        {
            neededSet.push_back(sChannels[i].movies.front());
        }
    }

    neededSet.push_back(sBedTimeMovie);
    
    for(Movies::iterator i = gMovies.begin(), end = gMovies.end(); i != end; ++i)
    {
        Movie& movie = *i;
        
        if(movie.state != Movie::MS_LOADED)
        {
            continue;
        }
        
        if(std::find(neededSet.begin(), neededSet.end(), &movie) == neededSet.end())
        {
            UnloadMovie(movie);
        }
    }
    
    for(std::vector<Movie*>::iterator i = neededSet.begin(), end = neededSet.end(); i != end; ++i)
    {
        Movie& movie = **i;
    
        if(movie.state == Movie::MS_DORMANT)
        {
            movie.state = Movie::MS_LOADING;
            sLoadWorkQueue.push_back(&movie);
        }
    }
}

static void StartThreads()
{
    Assert(!sLoadThread);

    sLoadThread = CreateThread(NULL, THREAD_STACK, &LoadThread, NULL, 0, NULL);
    Assert(sLoadThread != INVALID_HANDLE_VALUE);

    if(gMovies.empty())
    {
        return;
    }

    LoadChannelCovers();  
}

static bool StopThreads()
{
    if(!sLoadThread)
    {
        return(false);
    }

    sLoadWorkQueue.push_back(NULL);

    Assert(WaitForSingleObject(sLoadThread, INFINITE) == WAIT_OBJECT_0);
    sLoadThread = NULL;
 
    return(true);
}

static void OnPaintCover(HWND windowHandle, PAINTSTRUCT& paintStruct)
{
    RECT clientRect = {0};
    Assert(GetClientRect(windowHandle, &clientRect));

    if(gMovies.empty())
    {
        Assert(FillRect(paintStruct.hdc, &paintStruct.rcPaint, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))));
    
        HGDIOBJ prevObj = SelectObject(paintStruct.hdc, sLabelFont);

        RECT textRect = clientRect;
        textRect.top = clientRect.bottom / 2;
        textRect.bottom = clientRect.bottom;

        SetTextColor(paintStruct.hdc, RGB(192, 192, 192));
        SetBkColor(paintStruct.hdc, RGB(0, 0, 0));

        const TCHAR* message = TEXT("No movies found");

        DrawText(paintStruct.hdc, message, static_cast<int>(_tcslen(message)), &textRect, DT_CENTER | DT_WORDBREAK);

        SelectObject(paintStruct.hdc, prevObj);
        return;
    }

    Movie* movie = sSleepTime ? sBedTimeMovie : sCurrentChannel->movies.front();
    
    if(movie->state != Movie::MS_LOADED)
    {
        Assert(FillRect(paintStruct.hdc, &paintStruct.rcPaint, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))));
    
        HGDIOBJ prevObj = SelectObject(paintStruct.hdc, sLabelFont);

        RECT textRect = clientRect;
        textRect.top = clientRect.bottom / 2;
        textRect.bottom = clientRect.bottom;

        SetTextColor(paintStruct.hdc, RGB(192, 192, 192));
        SetBkColor(paintStruct.hdc, RGB(0, 0, 0));

        DrawText(paintStruct.hdc, movie->name, static_cast<int>(_tcslen(movie->name)), &textRect, DT_CENTER | DT_WORDBREAK);

        SelectObject(paintStruct.hdc, prevObj);
    }
    else
    {
        BITMAP bitmapInfo = {0}; 
        Assert(GetObjectA(movie->cover, sizeof(bitmapInfo), &bitmapInfo)); 
        int bitmapDx = bitmapInfo.bmWidth;
        int bitmapDy = bitmapInfo.bmHeight;

        RECT bitmapRect = clientRect;
        bitmapRect.left = clientRect.left + ((clientRect.right - clientRect.left) - bitmapDx) / 2;
        bitmapRect.right = bitmapRect.left + bitmapDx;
        bitmapRect.top = clientRect.top + ((clientRect.bottom - clientRect.top) - bitmapDy) / 2;
        bitmapRect.bottom = bitmapRect.top + bitmapDy;

        HDC coverDc = CreateCompatibleDC(paintStruct.hdc);
        HGDIOBJ prevObj = SelectObject(coverDc, movie->cover);

        Assert(BitBlt
        (
            paintStruct.hdc,
            bitmapRect.left,
            bitmapRect.top, 
            bitmapRect.right - bitmapRect.left,
            bitmapRect.bottom - bitmapRect.top,
            coverDc,
            0,
            0,
            SRCCOPY
        ));

        SelectObject(coverDc, prevObj);
        Assert(DeleteDC(coverDc));

        HRGN tileRgn = CreateRectRgnIndirect(&clientRect);
        HRGN coverRgn = CreateRectRgnIndirect(&bitmapRect);
        HRGN drawRegion = CreateRectRgnIndirect(&clientRect);

        if(CombineRgn(drawRegion, tileRgn, coverRgn, RGN_DIFF) != NULLREGION)
        {
            Assert(FillRgn(paintStruct.hdc, drawRegion, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))));
        }

        DeleteObject(tileRgn);
        DeleteObject(coverRgn);
        DeleteObject(drawRegion);
    }
}

static void CalcWindowedLayout(int& x, int& y, int& dx, int& dy, DWORD windowStyle)
{
    // const long WINDOW_DX = 1920 - 160;
    // const long WINDOW_DY = 1200 - 90;

    const long WINDOW_DX = 720;
    const long WINDOW_DY = 480;

    RECT windowRect = { 0, 0, WINDOW_DX, WINDOW_DY };
    Assert(AdjustWindowRect(&windowRect, windowStyle, FALSE));

    RECT parentRect = { 0, 0, 0, 0 };
    
    Assert(SystemParametersInfo(SPI_GETWORKAREA, 0, &parentRect, 0));

    dx = windowRect.right - windowRect.left;
    dy = windowRect.bottom - windowRect.top;

    x = parentRect.left + ((parentRect.right - parentRect.left) - dx) / 2;
    y = parentRect.top + ((parentRect.bottom - parentRect.top) - dy) / 2;
}

static void CalcFullscreenLayout(int& x, int& y, int& dx, int& dy)
{
    RECT windowRect = {0};
    GetWindowRect(GetDesktopWindow(), &windowRect);
    
    dx = windowRect.right - windowRect.left;
    dy = windowRect.bottom - windowRect.top;

    x = windowRect.left;
    y = windowRect.top;
}

const TCHAR* VOLUME_REG_KEY = TEXT("SOFTWARE\\TeeEee\\Volume");

static void GetVolumeKeyName(TCHAR* keyName, const Movie& movie)
{
    const TCHAR* baseName = FindBaseName(const_cast<TCHAR*>(movie.coverPath));
    
    if(!baseName || !baseName[0])
    {
        _tcscpy(keyName, movie.name);
        return;
    }

    _tcscpy(keyName, baseName + 1);
    TCHAR* ext = FindExtension(keyName);

    if(ext)
    {
        *ext = '\0';
    }

    TCHAR* disc = _tcsstr(keyName, TEXT("Disc "));

    if(disc)
    {
        disc[5] = '*';
    }
    
    TCHAR* part = _tcsstr(keyName, TEXT("Part "));

    if(part)
    {
        part[5] = '*';
    }

    TCHAR* season = _tcsstr(keyName, TEXT("Season "));

    if(season)
    {
        season[7] = '*';
    }
}

void LoadVolumeForMovie()
{
    const Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();

    HKEY key = NULL;
    
    if(RegOpenKeyEx(HKEY_CURRENT_USER, VOLUME_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        sVolume = 0.f;
        return;
    }

    DWORD volumeSize = sizeof(sVolume);
    DWORD valueType = REG_DWORD;

    TCHAR keyName[MAX_PATH];
    GetVolumeKeyName(keyName, movie);

    bool gotValue = (RegQueryValueEx(key, keyName, 0, &valueType, reinterpret_cast<LPBYTE>(&sVolume), &volumeSize) == ERROR_SUCCESS);
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
    
    sVolume /= 4.0; // Marshall from old format

    if(!gotValue || (sVolume < MIN_VOLUME) || (sVolume > MAX_VOLUME))
    {
        sVolume = 0.f;
    }
}

void SaveVolumeForMovie()
{
    const Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();

    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        VOLUME_REG_KEY, 
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        NULL,
        &key,
        NULL
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not create key value.");
        return;
    }

    float volume = sVolume * 4.f; // Marshall to old format

    TCHAR keyName[MAX_PATH];
    GetVolumeKeyName(keyName, movie);

    if(RegSetValueEx
    (
        key,
        keyName,
        NULL,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&volume),
        static_cast<DWORD>(sizeof(volume))
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }
    
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

const TCHAR* TEE_EEE_REG_KEY = TEXT("SOFTWARE\\TeeEee");
const TCHAR* BED_TIME_MOVIE_REG_KEY = TEXT("BedTime Movie");

static void LoadBedTimeMovie()
{
    sBedTimeMovie = &gMovies.front();

    HKEY key = NULL;
    
    if(RegOpenKeyEx(HKEY_CURRENT_USER, TEE_EEE_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        sVolume = 0.f;
        return;
    }

    TCHAR movieName[MAX_PATH];
    DWORD movieNameSize = sizeof(movieName);
    DWORD movieNameType = REG_SZ;

    bool gotValue = (RegQueryValueEx(key, BED_TIME_MOVIE_REG_KEY, 0, &movieNameType, reinterpret_cast<LPBYTE>(movieName), &movieNameSize) == ERROR_SUCCESS);
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
    
    if(!gotValue)
    {
        return;
    }
    
    movieName[movieNameSize] = '\0';
    
    for(size_t i = 0; i < gMovies.size(); ++i)
    {
        Movie& movie = gMovies[i];
        
        if(!_tcscmp(movie.name, movieName))
        {
            sBedTimeMovie = &movie;
            break;
        }
    }
}

static void SaveBedTimeMovie()
{
    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        TEE_EEE_REG_KEY, 
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        NULL,
        &key,
        NULL
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not create key value.");
        return;
    }

    Assert(sBedTimeMovie);

    if(RegSetValueEx
    (
        key,
        BED_TIME_MOVIE_REG_KEY,
        NULL,
        REG_SZ,
        reinterpret_cast<const BYTE*>(sBedTimeMovie->name),
        static_cast<DWORD>(_tcslen(sBedTimeMovie->name) + 1) * sizeof(TCHAR)
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }
    
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

static void LoadSensitivity()
{
    HKEY key = NULL;
    
    if(RegOpenKeyEx(HKEY_CURRENT_USER, TEE_EEE_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        sMicrophoneSensitivityNormal = 0.5f;
        sMicrophoneSensitivityBedtime = 0.5f;
        sMinTimeout = TIMEOUT_STEP_SIZE;
        return;
    }

    DWORD valueSize = sizeof(sMicrophoneSensitivityNormal);
    DWORD valueType = REG_DWORD;
    bool gotValue;

    gotValue = (RegQueryValueEx(key, TEXT("Sensitivity"), 0, &valueType, reinterpret_cast<LPBYTE>(&sMicrophoneSensitivityNormal), &valueSize) == ERROR_SUCCESS);
    if(!gotValue || (sMicrophoneSensitivityNormal < 0.f) || (sMicrophoneSensitivityNormal > 1.f))
    {
        sMicrophoneSensitivityNormal = 0.5f;
    }

    gotValue = (RegQueryValueEx(key, TEXT("BedtimeSensitivity"), 0, &valueType, reinterpret_cast<LPBYTE>(&sMicrophoneSensitivityBedtime), &valueSize) == ERROR_SUCCESS);
    if(!gotValue || (sMicrophoneSensitivityBedtime < 0.f) || (sMicrophoneSensitivityBedtime > 1.f))
    {
        sMicrophoneSensitivityBedtime = 0.5f;
    }

    StaticAssert(sizeof(sMinTimeout) == sizeof(DWORD));

    gotValue = (RegQueryValueEx(key, TEXT("MinTimeout"), 0, &valueType, reinterpret_cast<LPBYTE>(&sMinTimeout), &valueSize) == ERROR_SUCCESS);
    if(!gotValue || (sMinTimeout >= (TIMEOUT_STEP_SIZE * TIMEOUT_STEPS)))
    {
        sMinTimeout = TIMEOUT_STEP_SIZE;
    }

    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

static void SaveSensitivity()
{
    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        TEE_EEE_REG_KEY, 
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        NULL,
        &key,
        NULL
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not create key value.");
        return;
    }

    if(RegSetValueEx
    (
        key,
        TEXT("Sensitivity"),
        NULL,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&sMicrophoneSensitivityNormal),
        static_cast<DWORD>(sizeof(sMicrophoneSensitivityNormal))
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }

    if(RegSetValueEx
    (
        key,
        TEXT("BedtimeSensitivity"),
        NULL,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&sMicrophoneSensitivityBedtime),
        static_cast<DWORD>(sizeof(sMicrophoneSensitivityBedtime))
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }

    if(RegSetValueEx
    (
        key,
        TEXT("MinTimeout"),
        NULL,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&sMinTimeout),
        static_cast<DWORD>(sizeof(sMinTimeout))
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }
    
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

const TCHAR* CHANNEL_POSITION_REG_KEY = TEXT("SOFTWARE\\TeeEee\\Channels");

struct NameSort
{
    bool operator()(const Movie& am, const Movie& bm) const
    {
        const TCHAR* a = am.path;
        const TCHAR* b = bm.path;

        while(*a && *b)
        {
            const int aDigit = isdigit(*a);
            const int bDigit = isdigit(*b);

            if(aDigit && bDigit)
            {
                TCHAR* aEnd;
                const unsigned long aNum = _tcstoul(a, &aEnd, 0);
                Assert(aEnd && (aEnd != a));
                a = aEnd;

                TCHAR* bEnd;
                const unsigned long bNum = _tcstoul(b, &bEnd, 0);
                Assert(aEnd && (bEnd != b));
                b = bEnd;

                const long d = static_cast<long>(aNum - bNum);

                if(d)
                {
                    return(d < 0);
                }
            
                continue;
            }
            else if(aDigit)
            {
                return(true);
            }
            else if(bDigit)
            {
                return(false);
            }
            
            const int d = *a - *b;

            if(d)
            {
                return(d < 0);
            }

            ++a;
            ++b;
        }

        return(*a != 0);
    }
};

static void BuildChannels()
{
    Assert(!gMovies.empty());
    
    // Separate movies into one channel per directory  
      
    std::sort(gMovies.begin(), gMovies.end(), NameSort());
    std::sort(gLoading.begin(), gLoading.end(), NameSort()); /* TODO: fix these */
    
    TCHAR prevDir[MAX_PATH] = {0};
    sChannels.reserve(32);
    
    for(Movies::iterator i = gMovies.begin(), end = gMovies.end(); i != end; ++i)
    {
        Movie& movie = *i;
        
        TCHAR* path = movie.coverPath[0] ? movie.coverPath : movie.path;
        const TCHAR* baseName = FindBaseName(path);

        size_t dirLen = static_cast<size_t>(baseName - path);
        
        TCHAR dir[MAX_PATH];
        _tcsncpy(dir, path, dirLen);
        dir[dirLen] = '\0';

        if(_tcscmp(prevDir, dir))
        {
            _tcscpy(prevDir, dir);
            sChannels.push_back(Channel());
        }
        
        sChannels.back().movies.push_back(&movie);
    }
    
    sCurrentChannel = &sChannels[rand() % sChannels.size()];

    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        CHANNEL_POSITION_REG_KEY, 
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        NULL,
        &key,
        NULL
    ) != ERROR_SUCCESS)
    {
        key = NULL;
    }
    
    for(size_t i = 0; i < sChannels.size(); ++i)
    {
        TCHAR channelNum[8];
        _sntprintf(channelNum, ARRAY_COUNT(channelNum), TEXT("%d\n"), i);

        TCHAR movieName[MAX_PATH];
        DWORD movieNameSize = sizeof(movieName);
        DWORD movieNameType = REG_SZ;

        if((RegQueryValueEx(key, channelNum, 0, &movieNameType, reinterpret_cast<LPBYTE>(movieName), &movieNameSize) != ERROR_SUCCESS) || (movieNameType != REG_SZ))
        {
            RegDeleteValue(key, channelNum);
            continue;
        }
        
        movieName[movieNameSize] = '\0';
        
        for(size_t j = 0, n = sChannels[i].movies.size(); j < n; ++j)
        {
            Movie* m = sChannels[i].movies.front();
            
            if(!_tcscmp(m->name, movieName))
            {
                break;
            }
            
            sChannels[i].movies.pop_front();
            sChannels[i].movies.push_back(m);
        }

        if(sChannels[i].movies.front() == sBedTimeMovie)
        {
            Movie* m = sChannels[i].movies.front();
            sChannels[i].movies.pop_front();
            sChannels[i].movies.push_back(m);
        }            
    }

    if(key)
    {
        Assert(RegCloseKey(key) == ERROR_SUCCESS);
    }

    for(size_t i = 0; i < sChannels.size(); ++i)
    {    
        GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sChannels[i].startedPlaying));
    }

    LoadVolumeForMovie();
}

static void SaveChannelPositions()
{
    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        CHANNEL_POSITION_REG_KEY, 
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        NULL,
        &key,
        NULL
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not create key value.");
        return;
    }
    
    for(size_t i = 0; i < sChannels.size(); ++i)
    {
        const Movie& movie = *sChannels[i].movies.front();
        
        TCHAR channelNum[8];
        _sntprintf(channelNum, ARRAY_COUNT(channelNum), TEXT("%d\n"), i);

        if(RegSetValueEx
        (
            key,
            channelNum,
            NULL,
            REG_SZ,
            reinterpret_cast<const BYTE*>(movie.name),
            static_cast<DWORD>(_tcslen(movie.name) + 1) * sizeof(TCHAR)
        ) != ERROR_SUCCESS)
        {
            Assert(!"Could not set value.");
        }
    }

    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

static void StopMovie()
{
    KillTimer(sWindowHandle, SET_VOLUME_TIMER); // not checking return value
    KillTimer(sWindowHandle, TIMEOUT_TICK); // not checking return value
    SafeDestroyWindow(sProgressBar);

    Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();
        
    gPlayingState = PM_IDLE;

    bool stopped = false;

    if(sVlcPlayer)
    {
        libvlc_media_player_stop(sVlcPlayer);
#ifndef RECYCLE_PLAYER_INSTANCE
        libvlc_media_player_release(sVlcPlayer);
        sVlcPlayer = NULL;
#endif
        stopped = true;
    }

    if(stopped)
    {
        LogF(TEXT("Stopped %s\n"), movie.name);
    }
    
    Assert(InvalidateRect(NULL, NULL, TRUE));
}

static inline float DecibelsToVolume(float decibels)
{
    return(powf(10.0, decibels / 20.f));
}

static inline float VolumeToDecibels(float volume)
{
    return(20.f * log10f(volume));
}

static IMMDevice* sDevice = NULL;
static IAudioSessionManager* sIAudioSessionManager = NULL;
static ISimpleAudioVolume* sSimpleAudioVolume = NULL;

static void InitVolume()
{
    IMMDeviceEnumerator* deviceEnumerator = NULL;
    Assert(!FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&deviceEnumerator)));
    Assert(!FAILED(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &sDevice)));
    SafeRelease(deviceEnumerator);

    Assert(!FAILED(sDevice->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, NULL, (void**)&sIAudioSessionManager)));
    
    Assert(!FAILED(sIAudioSessionManager->GetSimpleAudioVolume(NULL, FALSE, &sSimpleAudioVolume)));
}

static void ShutdownVolume()
{
    SafeRelease(sSimpleAudioVolume);
    SafeRelease(sIAudioSessionManager);
    SafeRelease(sDevice);
}

static void SetVolume()
{
    if(!sVlcPlayer)
    {
        return;
    }

    // has 0-100 linear range
    int vlcVolume = Round(100.f * DecibelsToVolume(sVolume + GAIN_HEADROOM));
    vlcVolume = Clamp(vlcVolume, 0, 100);
    libvlc_audio_set_volume(sVlcPlayer, vlcVolume);

    float masterVolume = 1.f;

    if(sSleepTime)
    {
        time_t now;
        time(&now);

        double secondsUntilSleep = std::max(difftime(sSleepTime, now), 0.0);
        double maxTimeUntilSleep = difftime(sSleepTime, sSleepTimeStarted);
        
        // As we approach the cut-off, turn the volume down.
        if((maxTimeUntilSleep > 1.f) && (secondsUntilSleep < maxTimeUntilSleep))
        {
            masterVolume = static_cast<float>(secondsUntilSleep / maxTimeUntilSleep);
        }
        else if(sSleepTime < now)
        {
            masterVolume = 0;
        }
    }
    
    masterVolume = std::max(masterVolume, 0.2f); // Min 20%

    libvlc_video_set_adjust_int(sVlcPlayer, libvlc_adjust_Enable, 1);
    libvlc_video_set_adjust_float(sVlcPlayer, libvlc_adjust_Brightness, sqrt(masterVolume + FLT_EPSILON));

    Assert(!FAILED(sSimpleAudioVolume->SetMasterVolume(masterVolume, NULL)));
}

static void AdvanceCurrentChannel()
{
    for(;;)
    {
        Movie* m = sCurrentChannel->movies.front();
        sCurrentChannel->movies.pop_front();
        sCurrentChannel->movies.push_back(m);
     
        m = sCurrentChannel->movies.front();
        
        GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sCurrentChannel->startedPlaying));

        if(m == sBedTimeMovie)
        {
            continue;
        }
        
        break;
    }

    LoadChannelCovers();
}

static void MovieCompletedCB(const libvlc_event_t*, void*)
{
    Log(TEXT("MovieCompleted\n"));
    PostMessage(sWindowHandle, WM_MOVIE_COMPLETED, 0, 0);
}

static void PlayerErrorCB(const libvlc_event_t*, void*)
{
    Log(TEXT("PlayerErrorCB\n"));
    PostMessage(sWindowHandle, WM_MOVIE_COMPLETED, 0, 0);
}

static void LengthChangedCB(const libvlc_event_t*, void*)
{
    // Must be deferred to main thread:
    Log(TEXT("LengthChangedCB\n"));
    PostMessage(sWindowHandle, WM_LENGTH_CHANGED, 0, 0);
}

static void PlayMovieEx(Movie& movie, PlayingState newState)
{
    KillTimer(sWindowHandle, TIMEOUT_TICK); // not checking return value
    SafeDestroyWindow(sProgressBar);

#ifndef RECYCLE_PLAYER_INSTANCE
    if(sVlcPlayer)
    {
        StopMovie();
    }
#endif // RECYCLE_PLAYER_INSTANCE

    if(newState == PM_PLAYING)
    {
        LoadVolumeForMovie();
    }
    else if(newState == PM_SHUSH)
    {
        sVolume = -GAIN_HEADROOM;
    }
    else
    {
        sVolume = 0.f;
    }
    
    char moviePath[MAX_PATH]; // VLC doesn't do wide chars it seems
    libvlc_media_t* media = NULL;

    if(WideCharToMultiByte
    (
        CP_ACP,
        0,
        movie.path,
        -1,
        moviePath,
        ARRAY_COUNT(moviePath),
        NULL,
        NULL
    ))
    {
        media = libvlc_media_new_path(sVlc, moviePath);
    }

    if(!media)
    {
        LogF(TEXT("Could not load media %s\n"), movie.path);
        return;
    }

#ifdef RECYCLE_PLAYER_INSTANCE
    if(sVlcPlayer)
    {
       libvlc_media_player_set_media(sVlcPlayer, media);
    }
    else
#endif // RECYCLE_PLAYER_INSTANCE
    {
        sVlcPlayer = libvlc_media_player_new_from_media(media);

        if(!sVlcPlayer)
        {
            LogF(TEXT("Could not create player for %s\n"), movie.path);
            return;
        }

        libvlc_event_manager_t* eventManager = libvlc_media_player_event_manager(sVlcPlayer);
        Assert(eventManager);

        libvlc_media_player_set_hwnd(sVlcPlayer, sWindowHandle);
    
        libvlc_video_set_key_input(sVlcPlayer, false);
        libvlc_video_set_mouse_input(sVlcPlayer, false);
        
        Assert(!libvlc_event_attach(eventManager, libvlc_MediaPlayerEndReached, &MovieCompletedCB, NULL));
        Assert(!libvlc_event_attach(eventManager, libvlc_MediaPlayerEncounteredError, &PlayerErrorCB, NULL));
        Assert(!libvlc_event_attach(eventManager, libvlc_MediaPlayerLengthChanged, &LengthChangedCB, NULL));
    }

    libvlc_media_release(media);
    media = NULL;

    gPlayingState = newState;
    
    SetVolume();

    libvlc_media_player_play(sVlcPlayer);

    Assert(SetTimer(sWindowHandle, SET_VOLUME_TIMER, SET_VOLUME_DELAY, NULL));

    LogF(TEXT("Started %s\n"), movie.name);
}

static void PlayMovie()
{
    Assert(sWindowHandle);
        
    if(gMovies.empty())
    {
        return;
    }
    
    Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();
    
    if(gPlayingState == PM_PAUSED)
    {
        libvlc_media_player_play(sVlcPlayer);

        if(sPauseTime > 0)
        {
            libvlc_media_player_set_time(sVlcPlayer, sPauseTime);

            __int64 systemTime = {0}; // 100-nanosecond intervals since January 1, 1601 
            GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&systemTime));
            Assert(sCurrentChannel->startedPlaying && (sCurrentChannel->startedPlaying <= systemTime));

            sCurrentChannel->startedPlaying = systemTime - (sPauseTime * VLCTIME_TO_FILE_TIME);
        }

        Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
        
        gPlayingState = PM_PLAYING;

        LogF(TEXT("Resumeed %s\n"), movie.name);
        
        return;
    }

    if((gPlayingState == PM_SHUSH) || (gPlayingState == PM_TIMEOUT))
    {
        sTimeoutIndex = -1;
        StopMovie();
        return;
    }
    
    if((gPlayingState == PM_IDLE) || (gPlayingState == PM_PLAYING))
    {
        PlayMovieEx(movie, PM_PLAYING);
    }
}

static void PauseMovie()
{
    Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();

    gPlayingState = PM_PAUSED;

    sPauseTime = libvlc_media_player_get_time(sVlcPlayer);
    libvlc_media_player_stop(sVlcPlayer);

    Assert(InvalidateRect(sWindowHandle, NULL, TRUE));

    LogF(TEXT("Paused %s\n"), movie.name);
}

static void PlayLoading()
{
    if(gLoading.empty())
    {
        PlayMovie();
    }
    else
    {
        sLoading = true;
        PlayMovieEx(gLoading.front(), PM_LOADING);
    }
}

static void StartTimeoutBar()
{
    SafeDestroyWindow(sProgressBar);

    gPlayingState = PM_TIMEOUT;

    RECT clientRect = {0};
    Assert(GetClientRect(sWindowHandle, &clientRect));

    LONG height = clientRect.bottom / 8;
    LONG top = (clientRect.bottom - height) / 2;

    Assert(!sProgressBar);
    sProgressBar = CreateWindowEx
    (
        0,
        PROGRESS_CLASS,
        NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0,
        top,
        clientRect.right,
        height,
        sWindowHandle,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );

    // Cannot customize color if themes are enabled
    if(FAILED(SetWindowTheme(sProgressBar, TEXT(" "), TEXT(" "))))
    {
        Log(TEXT("Failed to disable theme on progress-bar\n"));
    }

    SendMessage(sProgressBar, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(RGB(128,128,128)));
    SendMessage(sProgressBar, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(0));
                
    const UINT timeout = (sTimeoutIndex > 0) ? sTimeoutIndex - 1 : 0;
    const UINT seconds = std::min(MAX_TIMEOUT_SECONDS, sMinTimeout << static_cast<UINT>(timeout));

    LogF(TEXT("Starting %d second timeout\n"), seconds);

    SendMessage(sProgressBar, PBM_SETRANGE32, 0, static_cast<LPARAM>((seconds * 1000) / TIMEOUT_TICK_MS));

    Assert(SetTimer(sWindowHandle, TIMEOUT_TICK, TIMEOUT_TICK_MS, NULL));
    TEMicrophone::SetSensitivity(sMicrophoneSensitivityBedtime); // assuming bedtime more sensative than normal
}

static void OnMovieComplete()
{
    const PlayingState prevState = gPlayingState;

    StopMovie();

    if(prevState == PM_SHUSH)
    {
        if(!sLoading && (sTimeoutIndex >= 0))
        {
            StartTimeoutBar();
            return;
        }
    }
    else if(prevState == PM_TIMEOUT)
    {
        sTimeoutIndex = -1;
    }
    else if(prevState == PM_PLAYING)
    {
        AdvanceCurrentChannel();
    }
    else if(prevState == PM_LOADING)
    {
        Assert(sLoading);
        sLoading = false;
    }

    if(sLoading)
    {
        TEMicrophone::SetSensitivity(sMicrophoneSensitivityBedtime);
        PlayLoading();
    }
    else
    {
        TEMicrophone::SetSensitivity(sSleepTime ? sMicrophoneSensitivityBedtime : sMicrophoneSensitivityNormal);
        PlayMovie();
    }
}

static void OnLengthChanged()
{
    if(gPlayingState != PM_PLAYING)
    {
        return;
    }

    __int64 systemTime = {0}; // 100-nanosecond intervals since January 1, 1601 
    GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&systemTime));
    Assert(sCurrentChannel->startedPlaying && (sCurrentChannel->startedPlaying <= systemTime));

    libvlc_time_t length = libvlc_media_player_get_length(sVlcPlayer);
    libvlc_time_t offset = (systemTime - sCurrentChannel->startedPlaying) / VLCTIME_TO_FILE_TIME;

    if(offset + (20 * 1000) > length)
    {
        OnMovieComplete();
    }
    else if(offset > (5 * 1000))
    {
        libvlc_media_player_set_time(sVlcPlayer, offset);
    }
}

class Database
{
public:
    Database() :
        mMongo(NULL),
        mCollection(NULL)
    {
        mongoc_init();
    }

    ~Database()
    {
        Disconnect();
        mongoc_cleanup();
    }

    void RecordEvent(const char* type)
    {
        if(!Connect())
        {
            return;
        }

        bson_t* doc = bson_new();
        BSON_APPEND_UTF8(doc, "t", type);

        bson_error_t error;
        const bool inserted = mongoc_collection_insert(mCollection, MONGOC_INSERT_NONE, doc, NULL, &error);

        bson_destroy(doc);
     
        if(!inserted)   
        {
            char logBuffer[512];
            _snprintf(logBuffer, ARRAY_COUNT(logBuffer), "RecordEvent: %s\n", error.message);
            OutputDebugStringA(logBuffer);
     
            Disconnect();
        }
    }

private:

    bool Connect()
    {
        if(mMongo)
        {
            return(true);
        }

        mMongo = mongoc_client_new("mongodb://localhost:27017/");
        Assert(mMongo);

        if(!mMongo)
        {
            return(false);
        }
        
        mCollection = mongoc_client_get_collection(mMongo, "te", "events");
        Assert(mCollection);

        return(true);
    }

    void Disconnect()
    {
        if(mCollection)
        {
            mongoc_collection_destroy(mCollection);
            mCollection = NULL;
        }

        if(mMongo)
        {
            mongoc_client_destroy(mMongo);
            mMongo = NULL;
        }
    }
    
    mongoc_client_t* mMongo;
    mongoc_collection_t* mCollection;
};

static Database sDatabase;

static void OnShush()
{
    Log(TEXT("Shush!\n"));
    
    if(gShush.empty())
    {
        return;
    }

    if((gPlayingState == PM_IDLE) || (gPlayingState == PM_PAUSED))
    {
        return;
    }

    time_t now;
    time(&now);
    sPreviousShushTimes[sPreviousShushIndex] = now;
    sPreviousShushIndex = (sPreviousShushIndex + 1) % ARRAY_COUNT(sPreviousShushTimes);
    
    if(sTimeoutIndex >= 0)
    {
        ++sTimeoutIndex;
    }
    else
    {
        size_t shushes = CountRecentShushes(60.0);

        if(shushes > 1)
        {
            sTimeoutIndex = shushes - 1;
        }
    }

    if(gPlayingState == PM_SHUSH)
    {
        return;
    }

#if 0
    if(sSleepTime)
    {
        StopMovie();
        StartTimeoutBar();
        return;
    }
#endif

    const char* type = sSleepTime ? "goodnight" : (sTimeoutIndex >= 0) ? "timeout" : "shush";
    sDatabase.RecordEvent(type);
    
    Movies& movies = sSleepTime ? gGoodnight : (sTimeoutIndex >= 0) ? gTimeout : gShush;
    PlayMovieEx(movies[rand() % movies.size()], PM_SHUSH);
}

static void NextChannel()
{
    if(gMovies.empty() || sSleepTime || (gPlayingState > PM_PLAYING))
    {
        return;
    }

    bool wasPlaying = (gPlayingState == PM_PLAYING);
    StopMovie();
    
    if(sCurrentChannel == &sChannels.back())
    {
        sCurrentChannel = &sChannels[0];
    }
    else
    {
        ++sCurrentChannel;
    }
    
    if(wasPlaying)
    {
        PlayLoading();
    }
    else
    {
        LoadVolumeForMovie();
        Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
    }
}

static void PrevChannel()
{
    if(gMovies.empty() || sSleepTime || (gPlayingState == PM_LOADING))
    {
        return;
    }

    bool wasPlaying = (gPlayingState == PM_PLAYING);
    StopMovie();
    
    if(sCurrentChannel == &sChannels.front())
    {
        sCurrentChannel = &sChannels.back();
    }
    else
    {
        --sCurrentChannel;
    }
    
    if(wasPlaying)
    {
        PlayLoading();
    }
    else
    {
        LoadVolumeForMovie();
        Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
    }
}    

static void HandleRemoteButton(size_t)
{
    if(sSleepTime || (gPlayingState > PM_PLAYING))
    {
        return;
    }

#if 1
    NextChannel();
#else
    // Button 2 is the big red one:
    //
    //if(buttonId == 2)
    //{
    //    SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_PLAY_PAUSE, 0);
    //    return;
    //}

    Channel& channel = sChannels[buttonId % sChannels.size()];

    if(&channel == sCurrentChannel)
    {
        // SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_PLAY_PAUSE, 0);
        return;
    }

    StopMovie();
    sCurrentChannel = &channel;

    PlayMovie();
#endif
}

enum PovHatState
{
    PHS_CENTERED,
    PHS_UP,
    PHS_DOWN,
    PHS_LEFT,
    PHS_RIGHT
};

static PovHatState GetPovHatState(DWORD value)
{
    if(LOWORD(value) == 0xFFFF)
    {
        return(PHS_CENTERED);
    }

    if((value >= 315 * DI_DEGREES) || (value <= 45 * DI_DEGREES))
    {
        return(PHS_UP);
    }

    if((value >= 45 * DI_DEGREES) && (value <= 135 * DI_DEGREES))
    {
        return(PHS_RIGHT);
    }

    if((value >= 135 * DI_DEGREES) && (value <= 225 * DI_DEGREES))
    {
        return(PHS_DOWN);
    }

    if((value >= 225 * DI_DEGREES) && (value <= 315 * DI_DEGREES))
    {
        return(PHS_LEFT);
    }
 
    return(PHS_CENTERED);
}

static void UpdateJoystick()
{
    if(!sJoystick)
    {
        return;
    }
    
    DIJOYSTATE2 joyState = {0};

    if(FAILED(sJoystick->Poll()))  
    {
        if(FAILED(sJoystick->Acquire()))
        {
            return;
        }
    }

    if(FAILED(sJoystick->GetDeviceState(sizeof(DIJOYSTATE2), &joyState)))
    {
        return;
    }
    
    if(gPlayingState > PM_PLAYING)
    {
        memcpy(&sPrevJoyState, &joyState, sizeof(joyState));
        Assert(SetTimer(sWindowHandle, JOYSTICK_INPUT_TIMER, JOYSTICK_INPUT_DELAY, NULL));
        return;
    }
    
    for(size_t i = 0; i < ARRAY_COUNT(joyState.rgbButtons); ++i)
    {
        if((joyState.rgbButtons[i] & 0x80) && !(sPrevJoyState.rgbButtons[i] & 0x80))
        {
            if(i < 4)
            {
                SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_NEXT_TRACK, 0);
            }
            // else if((i == 4) || (i == 5)) // SmartJoy SNES
            else if((i == 8) || (i == 9)) // SmartJoy PS2
            {
                SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_PLAY_PAUSE, 0);
            }
        }
    }

    for(size_t i = 0; i < ARRAY_COUNT(joyState.rgdwPOV); ++i)
    {
        PovHatState prevState = GetPovHatState(sPrevJoyState.rgdwPOV[i]);
        PovHatState newState = GetPovHatState(joyState.rgdwPOV[i]);
        
        if(prevState != newState)
        {
            switch(newState)
            {
                case PHS_CENTERED:
                    break;
                    
                case PHS_UP:
                    SendMessage(sWindowHandle, WM_KEYDOWN, VK_VOLUME_UP, 0);
                    break;
                    
                case PHS_DOWN:
                    SendMessage(sWindowHandle, WM_KEYDOWN, VK_VOLUME_DOWN, 0);
                    break;
                    
                case PHS_LEFT:
                    SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_PREV_TRACK, 0);
                    break;
                    
                case PHS_RIGHT:
                    SendMessage(sWindowHandle, WM_KEYDOWN, VK_MEDIA_NEXT_TRACK, 0);
                    break;
            }
        }
    }
    
    memcpy(&sPrevJoyState, &joyState, sizeof(joyState));

    Assert(SetTimer(sWindowHandle, JOYSTICK_INPUT_TIMER, JOYSTICK_INPUT_DELAY, NULL));
}

static void CleanMenuName(TCHAR* out, const TCHAR* in)
{
    while(*in)
    {
        *out = *in;

        ++in;
        ++out;       
 
        if(*in == '&')
        {
            *out = *in;
            ++out;
        }
    }
    
    *out = '\0';
}

static void EndSleep()
{
    if(!sSleepTime)
    {
        return;
    }

    sSleepTime = 0;
    
    StopMovie();
    sLoading = false;
    sDatabase.RecordEvent("stop");

    for(size_t i = 0; i < sChannels.size(); ++i)
    {    
        GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sChannels[i].startedPlaying));
    }

    Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
}

template<typename MatchFunctor>
static void MakeIndexSubMenu(HMENU menuHandle, UINT_PTR commandBase, MatchFunctor f, const Movies& movies, const Movie* selected)
{
    HMENU subMenu = NULL;
    TCHAR nameBuf[2 * MAX_PATH];

    for(Movies::const_iterator i = movies.begin(); i != movies.end(); ++i)
    {
        const Movie& movie = *i;
        
        if(!f(movie))
        {
            continue;
        }

        if(!subMenu)
        {
            subMenu = CreatePopupMenu();
            Assert(subMenu);
            
            f.CalcMenuName(nameBuf, ARRAY_COUNT(nameBuf));
            
            Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), nameBuf));
        }
        
        CleanMenuName(nameBuf, movie.name);
        
        size_t index = std::distance<Movies::const_iterator>(movies.begin(), i);
        UINT id = static_cast<UINT>(commandBase + index);
        Assert(AppendMenu(subMenu, MF_STRING | MF_ENABLED, id, nameBuf));
        
        if(&movie == selected)
        {
            Assert(CheckMenuRadioItem(subMenu, id, id, id, MF_BYCOMMAND));
        }
    }
}

static void MakeChannelSubMenu(HMENU menuHandle, UINT_PTR commandBase, const ChannelMovies& movies)
{
    TCHAR nameBuf[2 * MAX_PATH];

    for(ChannelMovies::const_iterator i = movies.begin(), end = movies.end(); i != end; ++i)
    {
        CleanMenuName(nameBuf, (*i)->name);
        
        size_t index = std::distance<ChannelMovies::const_iterator>(movies.begin(), i);
        UINT_PTR id = commandBase + index;
        Assert(AppendMenu(menuHandle, MF_STRING | MF_ENABLED, id, nameBuf));
    }
}

static LRESULT CALLBACK WindowProc(HWND windowHandle, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
        case WM_LOADED_MOVIE:
        {
            Movie* movie = reinterpret_cast<Movie*>(lParam);
            Assert(movie);

            const Movie* currentMovie = sSleepTime ? sBedTimeMovie : sCurrentChannel->movies.front();

            if(movie == currentMovie)
            {
                Assert(InvalidateRect(windowHandle, NULL, TRUE));
            }
            return(0);   
        }
        
        case WM_MOVIE_COMPLETED:
        {
            OnMovieComplete();
            return(0);
        };

        case WM_LENGTH_CHANGED:
        {
            OnLengthChanged();
            return(0);
        }

        case WM_REMOTE_BUTTON:
        {
            HandleRemoteButton(static_cast<size_t>(wParam));
            return(0);
        }
        
        case WM_SIZE:
        {        
            sMaxCoverDx = LOWORD(lParam);
            sMaxCoverDy = HIWORD(lParam);

            if(wParam)
            {
                Assert(SetTimer(windowHandle, ON_SIZE_TIMER, ON_SIZE_DELAY, NULL));
                Assert(InvalidateRect(windowHandle, NULL, TRUE));
            }
            else
            {
                SendMessage(windowHandle, WM_TIMER, ON_SIZE_TIMER, 0);
            }                
            break;
        }

        case WM_TIMER:
        {
            if(wParam == JOYSTICK_INPUT_TIMER)
            {
                UpdateJoystick();
            }
            else if(wParam == ON_SIZE_TIMER)
            {
                KillTimer(windowHandle, ON_SIZE_TIMER);
            
                bool stopped = StopThreads();
                
                for(Movies::iterator i = gMovies.begin(); i != gMovies.end(); ++i)
                {
                    UnloadMovie(*i);
                }
                
                if(stopped)
                {
                    StartThreads();
                }
                
                return(0);
            }
            else if(wParam == SET_VOLUME_TIMER)
            {
                if(sSleepTime)
                {
                    SetVolume();

#if 0
                    time_t now;
                    time(&now);
                    
                    if(now > sSleepTime)
                    {
                        Log(TEXT("Sleep timer done\n"));
                        EndSleep();
                    }
#endif
                }
                else
                {
                    SYSTEMTIME systemTime = {0};
                    GetLocalTime(&systemTime);
                    
                    // if(systemTime.wHour >= 8 + 12)
                    if((systemTime.wHour > 7 + 12) || ((systemTime.wHour == 7 + 12) && (systemTime.wMinute > 30)))
                    {
                        SendMessage(windowHandle, WM_COMMAND, MAKEWPARAM((COMMAND_SLEEP_MODE + (SLEEP_INTERVALS - 1)), 0), 0);
                    }
                }

                return(0);
            }
            else if(wParam == HIDE_CURSOR_TIMER)
            {
                CURSORINFO cursorInfo = {0};
                cursorInfo.cbSize = sizeof(cursorInfo);
     
                Assert(GetCursorInfo(&cursorInfo));
                
                if(cursorInfo.flags & CURSOR_SHOWING)
                {
                    ShowCursor(FALSE);
                    sHideCursorPoint = cursorInfo.ptScreenPos;
                }

                Assert(SetTimer(sWindowHandle, HIDE_CURSOR_TIMER, HIDE_CURSOR_DELAY, NULL));
            }
            else if(wParam == TIMEOUT_TICK)
            {
                if(!sProgressBar)
                {
                    KillTimer(sWindowHandle, TIMEOUT_TICK); // not checking return value
                }
                else
                {
                    LPARAM pos = SendMessage(sProgressBar, PBM_GETPOS, 0, 0);
                    LPARAM maxPos = SendMessage(sProgressBar, PBM_GETRANGE, 0, 0);

                    if(pos < maxPos)
                    {
                        SendMessage(sProgressBar, PBM_SETPOS, static_cast<WPARAM>(pos + 1), 0);
                    }
                    else
                    {
                        KillTimer(sWindowHandle, TIMEOUT_TICK); // not checking return value
                        SafeDestroyWindow(sProgressBar);

                        sTimeoutIndex = -1;

                        if(sLoading)
                        {
                            TEMicrophone::SetSensitivity(sMicrophoneSensitivityBedtime);
                            PlayLoading();
                        }
                        else
                        {
                            TEMicrophone::SetSensitivity(sSleepTime ? sMicrophoneSensitivityBedtime : sMicrophoneSensitivityNormal);
                            StopMovie();
                            PlayMovie();
                        }
                    }
                }
            }
            else
            {
                break;
            }
        }
        
        case WM_MOUSEMOVE:
        {
            CURSORINFO cursorInfo = {0};
            cursorInfo.cbSize = sizeof(cursorInfo);
 
            Assert(GetCursorInfo(&cursorInfo));
            
            if(!(cursorInfo.flags & CURSOR_SHOWING))
            {
                float distanceSqr = Sqr(static_cast<float>(sHideCursorPoint.x - cursorInfo.ptScreenPos.x)) +
                    Sqr(static_cast<float>(sHideCursorPoint.y - cursorInfo.ptScreenPos.y));
            
                if(distanceSqr > 100)
                {
                    ShowCursor(TRUE);
                }
            }
            
            Assert(SetTimer(sWindowHandle, HIDE_CURSOR_TIMER, HIDE_CURSOR_DELAY, NULL));
            break;
        }
                
        case WM_PAINT:
        {
            PAINTSTRUCT paintStruct;
            HDC deviceContext = BeginPaint(windowHandle, &paintStruct);
            Assert(deviceContext);

            if((gPlayingState == PM_PAUSED) || (gPlayingState == PM_IDLE))
            {
                OnPaintCover(windowHandle, paintStruct);
            }
#if 1
            else if(gPlayingState == PM_TIMEOUT)
            {
                Assert(FillRect(paintStruct.hdc, &paintStruct.rcPaint, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))));
            }
#endif

            Assert(EndPaint(windowHandle, &paintStruct));
        
            return(0);
        }

        case WM_SYSCOMMAND:
        {
            if((gPlayingState == PM_PAUSED) || (gPlayingState == PM_IDLE))
            {
                break;
            }
        
            if(wParam == SC_SCREENSAVE)
            {
                return(0);
            }
            
            break;
        }
        
        case WM_CONTEXTMENU:
        {
            POINT p = { 0, 0 };

            if(lParam != static_cast<LPARAM>(-1))
            {
                p.x = static_cast<SHORT>(LOWORD(lParam));
                p.y = static_cast<SHORT>(HIWORD(lParam));
            }
            else
            {
                RECT windowRect = { 0, 0, 0, 0 };
                Assert(GetWindowRect(windowHandle, &windowRect));
                p.x = (windowRect.right + windowRect.left) / 2;
                p.y = (windowRect.bottom + windowRect.top) / 2;
            }
            
            HMENU menuHandle = CreatePopupMenu();
            Assert(menuHandle);
            
            {
                HMENU channelMenu = CreatePopupMenu();
                Assert(channelMenu);
                
                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(channelMenu), TEXT("Channels")));

                UINT_PTR playMovieId = COMMAND_PLAY_MOVIE_INDEX;

                for(size_t i = 0; i < sChannels.size(); ++i)
                {
                    HMENU subMenu = CreatePopupMenu();
                    Assert(subMenu);

                    Movie& movie = *sChannels[i].movies.front();
                    TCHAR* path = movie.coverPath[0] ? movie.coverPath : movie.path;
                    TCHAR* baseName = FindBaseName(path);
                    size_t dirLen = static_cast<size_t>(baseName - path);
                    
                    TCHAR dir[MAX_PATH];
                    _tcsncpy(dir, path, dirLen);
                    dir[dirLen] = '\0';
                    
                    const TCHAR* label = FindBaseName(dir) + 1;
                    
                    Assert(AppendMenu(channelMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), label));

                    MakeChannelSubMenu(subMenu, playMovieId, sChannels[i].movies);
                    playMovieId += sChannels[i].movies.size();
                }
            }
            
            {
                HMENU channelMenu = CreatePopupMenu();
                Assert(channelMenu);

                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(channelMenu), TEXT("Bed-Time Movie")));

                UINT_PTR playMovieId = COMMAND_BED_TIME_MOVIE_INDEX;

                for(size_t i = 0; i < sChannels.size(); ++i)
                {
                    HMENU subMenu = CreatePopupMenu();
                    Assert(subMenu);

                    Movie& movie = *sChannels[i].movies.front();
                    TCHAR* path = movie.coverPath[0] ? movie.coverPath : movie.path;
                    TCHAR* baseName = FindBaseName(path);
                    size_t dirLen = static_cast<size_t>(baseName - path);
                    
                    TCHAR dir[MAX_PATH];
                    _tcsncpy(dir, path, dirLen);
                    dir[dirLen] = '\0';
                    
                    const TCHAR* label = FindBaseName(dir) + 1;
                    
                    Assert(AppendMenu(channelMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), label));

                    MakeChannelSubMenu(subMenu, playMovieId, sChannels[i].movies);
                    playMovieId += sChannels[i].movies.size();
                }
            }
            
            {
                HMENU subMenu = CreatePopupMenu();
                Assert(subMenu);
                
                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), TEXT("Microphone Sensitivity")));
               
                for(UINT i = 0; i < SENSITIVITY_STEPS; ++i)
                {
                    TCHAR buffer[64];
                    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("%u"), (SENSITIVITY_STEPS - i));
                
                    Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_MICROPHONE_SENSITIVITY + i, buffer));
                }

                float& sensitivity = sSleepTime ? sMicrophoneSensitivityBedtime : sMicrophoneSensitivityNormal;
                UINT index = static_cast<UINT>((1.f - sensitivity) * static_cast<float>(SENSITIVITY_STEPS));
                
                if(index >= SENSITIVITY_STEPS)
                {
                    index = SENSITIVITY_STEPS - 1;
                }

                UINT command = COMMAND_MICROPHONE_SENSITIVITY + index;
                Assert(CheckMenuRadioItem(subMenu, COMMAND_MICROPHONE_SENSITIVITY, COMMAND_MICROPHONE_SENSITIVITY + SENSITIVITY_STEPS, command, MF_BYCOMMAND));
            }

            {
                HMENU subMenu = CreatePopupMenu();
                Assert(subMenu);
                
                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), TEXT("Timeout")));
               
                for(UINT i = 0; i < TIMEOUT_STEPS; ++i)
                {
                    TCHAR buffer[64];
                    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("%u"), (i + 1) * TIMEOUT_STEP_SIZE);
                
                    Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_MIN_TIMEOUT + i, buffer));
                }

                UINT index = static_cast<UINT>((sMinTimeout / TIMEOUT_STEP_SIZE) - 1);
                
                if(index >= TIMEOUT_STEPS)
                {
                    index = TIMEOUT_STEPS - 1;
                }

                UINT command = COMMAND_MIN_TIMEOUT + index;
                Assert(CheckMenuRadioItem(subMenu, COMMAND_MIN_TIMEOUT, COMMAND_MIN_TIMEOUT + TIMEOUT_STEPS, command, MF_BYCOMMAND));
            }

            {
                HMENU subMenu = CreatePopupMenu();
                Assert(subMenu);
                
                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), TEXT("Volume Adjustment")));

                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_UP_3, TEXT("Up 3")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_UP_2, TEXT("Up 2")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_UP_1, TEXT("Up 1")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_NONE, TEXT("None")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_DOWN_1, TEXT("Down 1")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_DOWN_2, TEXT("Down 2")));
                Assert(AppendMenu(subMenu, MF_POPUP | MF_STRING, COMMAND_VOLUME_DOWN_3, TEXT("Down 3")));
             
                UINT command = COMMAND_VOLUME_NONE - Round(sVolume / VOLUME_QUANTUM);
             
                Assert(CheckMenuRadioItem(subMenu, COMMAND_VOLUME_UP_3, COMMAND_VOLUME_DOWN_3, command, MF_BYCOMMAND));
            }
            
            {
                HMENU subMenu = CreatePopupMenu();
                Assert(subMenu);
                
                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), TEXT("Sleep Mode")));

                Assert(AppendMenu(subMenu, MF_STRING | MF_ENABLED | (sSleepTime ? 0 : MF_CHECKED), COMMAND_SLEEP_MODE_DISABLED, TEXT("Disabled")));

                UINT index = COMMAND_SLEEP_MODE;

                size_t hoursUntilSleep = INVALID;
                
                if(sSleepTime)
                {
                    time_t now;
                    time(&now);
                    hoursUntilSleep = static_cast<size_t>(Round(std::max(difftime(sSleepTime, now), 0.0) / (60.f * 60.f)));
                }

                for(size_t i = 0; i < SLEEP_INTERVALS; ++i)
                {
                    size_t hours = i + 1;
                
                    TCHAR buffer[64];
                    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("%d:00"), hours);
                    
                    UINT flags = MF_STRING | MF_ENABLED;
                    
                    if(hours == hoursUntilSleep)
                    {
                        flags |= MF_CHECKED;
                    }
                    
                    Assert(AppendMenu(subMenu, flags, index, buffer));
                    ++index;
                }
            }
            
            LONG windowStyle = GetWindowLong(sWindowHandle, GWL_STYLE);

            if(windowStyle & WS_DLGFRAME)
            {
                Assert(AppendMenu(menuHandle, MF_STRING | MF_ENABLED, COMMAND_FULL_SCREEN, TEXT("Full-Screen\tAlt+Enter")));
            }
            else
            {
                Assert(AppendMenu(menuHandle, MF_CHECKED | MF_STRING | MF_ENABLED, COMMAND_FULL_SCREEN, TEXT("Full-Screen\tAlt+Enter")));
            }

            Assert(AppendMenu(menuHandle, MF_STRING | MF_ENABLED, COMMAND_RESTART_MOVIE, TEXT("Restart Movie")));

            Assert(AppendMenu(menuHandle, MF_STRING | MF_ENABLED, COMMAND_QUIT, TEXT("Quit\tAlt+F4")));

            TrackPopupMenu(menuHandle, TPM_TOPALIGN | TPM_LEFTALIGN, p.x, p.y, 0, windowHandle, NULL);
            Assert(DestroyMenu(menuHandle));
        }
        
        case WM_COMMAND:
        {
            switch(LOWORD(wParam))
            {
                case COMMAND_QUIT:
                {
                    DestroyWindow(windowHandle);
                    return(0);
                }
                
                case COMMAND_FULL_SCREEN:
                {
                    LONG windowStyle = GetWindowLong(windowHandle, GWL_STYLE);
                    windowStyle = windowStyle ^ WS_DLGFRAME;
                    SetWindowLong(windowHandle, GWL_STYLE, windowStyle);

                    int x = 0;
                    int y = 0;
                    int dx = 0;
                    int dy = 0;
                   
                    if(windowStyle & WS_DLGFRAME)
                    {
                        CalcWindowedLayout(x, y, dx, dy, windowStyle);
                        Assert(SetWindowPos(windowHandle, HWND_NOTOPMOST, x, y, dx, dy, SWP_DRAWFRAME | SWP_SHOWWINDOW));
                    }
                    else
                    {
                        CalcFullscreenLayout(x, y, dx, dy);
                        Assert(SetWindowPos(windowHandle, HWND_TOPMOST, x, y, dx, dy, SWP_DRAWFRAME | SWP_SHOWWINDOW));
                    }
                    
                    return(0);
                }
                
                case COMMAND_SLEEP_MODE_DISABLED:
                {
                    EndSleep();
                    return(0);
                }
                
                case COMMAND_RESTART_MOVIE:
                {
                    if(sVlcPlayer && (gPlayingState <= PM_PLAYING))
                    {
                        libvlc_media_player_set_position(sVlcPlayer, 0.f);
                    }

                    GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sCurrentChannel->startedPlaying));
                    return(0);
                }
            }

            if((LOWORD(wParam) >= COMMAND_VOLUME_UP_3) && (LOWORD(wParam) <= COMMAND_VOLUME_DOWN_3))
            {
                sVolume = VOLUME_QUANTUM * static_cast<float>(COMMAND_VOLUME_NONE - LOWORD(wParam));
                SetVolume();
                SaveVolumeForMovie();
                return(0);
            }
            
            if((LOWORD(wParam) >= COMMAND_MICROPHONE_SENSITIVITY) && (LOWORD(wParam) < COMMAND_MICROPHONE_SENSITIVITY + SENSITIVITY_STEPS))
            {
                UINT index = LOWORD(wParam) - COMMAND_MICROPHONE_SENSITIVITY;
                float& sensitivity = sSleepTime ? sMicrophoneSensitivityBedtime : sMicrophoneSensitivityNormal;
                sensitivity = 1.f - (static_cast<float>(index) / static_cast<float>(SENSITIVITY_STEPS - 1));
                TEMicrophone::SetSensitivity((sSleepTime || sLoading) ? sMicrophoneSensitivityBedtime : sMicrophoneSensitivityNormal);
                return(0);
            }

            if((LOWORD(wParam) >= COMMAND_MIN_TIMEOUT) && (LOWORD(wParam) < COMMAND_MIN_TIMEOUT + TIMEOUT_STEPS))
            {
                UINT index = LOWORD(wParam) - COMMAND_MIN_TIMEOUT;
                sMinTimeout = (index + 1) * TIMEOUT_STEP_SIZE;
                return(0);
            }
            
            if((LOWORD(wParam) >= COMMAND_PLAY_MOVIE_INDEX) && (LOWORD(wParam) < (COMMAND_PLAY_MOVIE_INDEX + gMovies.size())))
            {
                bool wasPlaying = (gPlayingState == PM_PLAYING);
            
                if(gPlayingState != PM_IDLE)
                {
                    StopMovie();    
                }
            
                size_t index = static_cast<size_t>(LOWORD(wParam) - COMMAND_PLAY_MOVIE_INDEX);
                
                for(size_t c = 0; c < sChannels.size(); ++c)
                {
                    if(index >= sChannels[c].movies.size())
                    {
                        index -= sChannels[c].movies.size();
                        continue;
                    }
                    
                    sCurrentChannel = &sChannels[c];
                    
                    while(index)
                    {
                        Movie* movie = sCurrentChannel->movies.front();
                        sCurrentChannel->movies.pop_front();
                        sCurrentChannel->movies.push_back(movie);
                        --index;
                    }
                            
                    LoadChannelCovers();
                    
                    GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sCurrentChannel->startedPlaying));
                    
                    if(wasPlaying)
                    {
                        PlayMovie();
                    }
                    else
                    {
                        LoadVolumeForMovie();
                        Assert(InvalidateRect(windowHandle, NULL, TRUE));
                    }
                    
                    return(0);
                }
                
                assert(!"Programmer too stupid");
                return(0);
            }
            
            
            if((LOWORD(wParam) >= COMMAND_BED_TIME_MOVIE_INDEX) && (LOWORD(wParam) < (COMMAND_BED_TIME_MOVIE_INDEX + gMovies.size())))
            {
                bool wasPlaying = sSleepTime && (gPlayingState == PM_PLAYING);
            
                if(sSleepTime && (gPlayingState != PM_IDLE))
                {
                    StopMovie();    
                }

                size_t index = static_cast<size_t>(LOWORD(wParam) - COMMAND_BED_TIME_MOVIE_INDEX);
                
                for(size_t c = 0; c < sChannels.size(); ++c)
                {
                    if(index >= sChannels[c].movies.size())
                    {
                        index -= sChannels[c].movies.size();
                        continue;
                    }

                    sBedTimeMovie = sChannels[c].movies[index];
                    SaveBedTimeMovie();
                    break;
                }

                if(wasPlaying)
                {
                    PlayMovie();    
                }
                
                return(0);
            }
            
            if((LOWORD(wParam) >= COMMAND_SLEEP_MODE) && (LOWORD(wParam) < (COMMAND_SLEEP_MODE + SLEEP_INTERVALS)))
            {
                if(!sSleepTime && ((gPlayingState == PM_PLAYING) || (gPlayingState == PM_PAUSED)))
                {
                    StopMovie();    
                }
            
                if(!sSleepTime)
                {
                    time(&sSleepTimeStarted);
                }

                time(&sSleepTime);
                UINT hours = (LOWORD(wParam) - COMMAND_SLEEP_MODE) + 1;

#ifdef _DEBUG
                sSleepTime += hours * 60;
#else
                sSleepTime += hours * 60 * 60;
#endif

                Assert(InvalidateRect(windowHandle, NULL, TRUE));
                
                GetSystemTimeAsFileTime(reinterpret_cast<LPFILETIME>(&sCurrentChannel->startedPlaying));

                if(sSleepTime && (gPlayingState == PM_IDLE))
                {
                    PlayMovie();
                }

                return(0);
            }

            break;
        }
       
        case WM_KEYDOWN:
        {
            //  VK_0 - VK_9 are the same as ASCII '0' - '9' (0x30 - 0x39)

            if((wParam >= '0') && (wParam <= '9'))
            {
                HandleRemoteButton(wParam - '0');
                return(0);
            }
        
            // Remapping:
            if(wParam == VK_SPACE)
            {
                SendMessage(windowHandle, msg, VK_MEDIA_PLAY_PAUSE, lParam);
                return(0);
            }        

            if(wParam == VK_MEDIA_STOP)
            {
                SendMessage(windowHandle, msg, VK_MEDIA_PLAY_PAUSE, lParam);
                return(0);
            }        
            
            if(wParam == VK_RIGHT)
            {
                SendMessage(windowHandle, msg, VK_MEDIA_NEXT_TRACK, lParam);
                return(0);
            }        

            if(wParam == VK_LEFT)
            {
                SendMessage(windowHandle, msg, VK_MEDIA_PREV_TRACK, lParam);
                return(0);
            }     

            if(wParam == VK_DOWN)
            {
                SendMessage(windowHandle, msg, VK_VOLUME_DOWN, lParam);
                return(0);
            }     

            if(wParam == VK_UP)
            {
                SendMessage(windowHandle, msg, VK_VOLUME_UP, lParam);
                return(0);
            }     

            if(wParam == 'M')
            {
                SendMessage(windowHandle, msg, VK_VOLUME_MUTE, lParam);
                return(0);
            }     
            
            if(wParam == 'S')
            {
                OnShush();
                return(0);
            }

            if(wParam == VK_MEDIA_PLAY_PAUSE)
            {
                if(sSleepTime)
                {
                    SYSTEMTIME systemTime = {0};
                    GetLocalTime(&systemTime);
                    
                    if((systemTime.wHour > 6) && (systemTime.wHour < 12))
                    {
                        EndSleep();
                        return(0);
                    }
                }

                if(gPlayingState == PM_PLAYING)
                {
                    PauseMovie();
                    sDatabase.RecordEvent("stop");
                }
                else if(gPlayingState == PM_LOADING)
                {
                    StopMovie();
                    sLoading = false;
                    sDatabase.RecordEvent("stop");
                }
                else
                {
                    sDatabase.RecordEvent("start");
                    PlayMovie();
                }
                return(0);
            }                    
            else
            {
                if(wParam == VK_MEDIA_PLAY_PAUSE)
                {
                    return(0);
                }
            }

            if(wParam == VK_END)
            {
                bool wasPlaying = (gPlayingState == PM_PLAYING);
                StopMovie();    
                AdvanceCurrentChannel();
                    
                if(wasPlaying)
                {
                    PlayMovie();
                }
                else
                {
                    Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
                }
                return(0);
            }
                            
            if(wParam == VK_MEDIA_NEXT_TRACK)
            {
                NextChannel();
                return(0);
            }
            else if(wParam == VK_MEDIA_PREV_TRACK)
            {
                PrevChannel();
                return(0);
            }
            
            break;
        }
    }

    return(DefWindowProc(windowHandle, msg, wParam, lParam));
}

static void InitWindow()
{
    Util::Zeroize(&sWindowClass, sizeof(sWindowClass));
    sWindowClass.lpfnWndProc = WindowProc;
    sWindowClass.hInstance = GetModuleHandle(NULL);
    sWindowClass.lpszClassName = CLASS_NAME;
    sWindowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    Assert(RegisterClass(&sWindowClass));
    
    DWORD windowStyle = WS_POPUP;

    if(IsDebuggerPresent())
    {
        windowStyle |= WS_DLGFRAME;
    }

    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    
    if(windowStyle & WS_DLGFRAME)
    {
        CalcWindowedLayout(x, y, dx, dy, windowStyle);
    }
    else
    {
        CalcFullscreenLayout(x, y, dx, dy);
    }
                    
    sWindowHandle = CreateWindowEx
    (
        0,
        CLASS_NAME,
        TEXT("Tee Eee"),
        windowStyle,
        x, y, dx, dy,
        HWND_DESKTOP,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );

    HDC deviceContext = GetDC(sWindowHandle);
    int height = -MulDiv(12, GetDeviceCaps(deviceContext, LOGPIXELSY), 72);            
    ReleaseDC(sWindowHandle, deviceContext);

    sLabelFont = CreateFont(height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, TEXT("Lucida Console"));
    
    ShowWindow(sWindowHandle, SW_SHOW);
    SetForegroundWindow(sWindowHandle);
    BringWindowToTop(sWindowHandle);
    
    sAcceleratorTable = LoadAccelerators(GetModuleHandle(NULL), MAKEINTRESOURCE(ACCELERATOR_MAIN));
    Assert(sAcceleratorTable);

    Assert(SetTimer(sWindowHandle, HIDE_CURSOR_TIMER, HIDE_CURSOR_DELAY, NULL));
    
    SetCursorPos(x + dx, y + dy);
}

BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCE* deviceInstance, void*)
{
    if(FAILED(sDirectInput->CreateDevice(deviceInstance->guidInstance, &sJoystick, NULL)))
    {
        return(DIENUM_CONTINUE);
    }
    else
    {
        return(DIENUM_STOP);
    }
}

static void InitJoystick()
{
    Assert(sWindowHandle);

    Assert(!FAILED(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, reinterpret_cast<void**>(&sDirectInput), NULL)));

    if(FAILED(sDirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoysticksCallback, NULL, DIEDFL_ATTACHEDONLY)) || !sJoystick)
    {
        Log(TEXT("Could not find joystick\n"));
        SafeRelease(sDirectInput);
        return;
    }

    Assert(!FAILED(sJoystick->SetDataFormat(&c_dfDIJoystick2)));
    Assert(!FAILED(sJoystick->SetCooperativeLevel(sWindowHandle, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND)));

    Assert(SetTimer(sWindowHandle, JOYSTICK_INPUT_TIMER, JOYSTICK_INPUT_DELAY, NULL));
}

static void HandleRemoteCommand(const char* command)
{
    // OutputDebugStringA(command);
    
    // N6E9102:
    // N - NEC protocol
    // NXXYYZZ
    // XXYY system code
    // ZZ command-code
    
    if((command[0] != 'N') || (strlen(command) != 7))
    {
        OutputDebugStringA("Bad remote command\n");
        return;
    }
    
    const char* hexBegin = command + 5;
    char* hexEnd = NULL;
    
    long buttonId = strtol(hexBegin, &hexEnd, 16);
    
    if(hexEnd != (hexBegin + 2))
    {
        OutputDebugStringA("Bad remote command\n");
        return;
    }
    
    static long prevButton = -1;
    static DWORD prevTicks = 0;

    DWORD curTicks = timeGetTime();

    if(buttonId == prevButton)
    {
        if((curTicks - prevTicks) < 750) // ms
        {
            return;
        }
    }

    prevButton = buttonId;
    prevTicks = curTicks;

    Assert(PostMessage(sWindowHandle, WM_REMOTE_BUTTON, static_cast<WPARAM>(buttonId), 0));
}

static unsigned __stdcall RemoteProc(void *)
{
    char readBuffer[1024] = "";
    DWORD buffered = 0;
    
    HANDLE eventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);

    for(;;)
    {
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = eventHandle;
        
        DWORD bytesToRead = static_cast<DWORD>(ARRAY_COUNT(readBuffer)) - (buffered + 1);
        DWORD bytesRead = 0;
                
        Assert(!ReadFile(sRemoteHandle, readBuffer + buffered, bytesToRead, &bytesRead, &overlapped));
    
        HANDLE waitHandles[] =
        {
            sRemoteSem,
            eventHandle
        };

        DWORD waitRc = WaitForMultipleObjects(ARRAY_COUNT(waitHandles), waitHandles, FALSE, INFINITE);
        
        if(waitRc == WAIT_OBJECT_0)
        {
            break;
        }
        else if(waitRc == WAIT_OBJECT_0 + 1)
        {
            bytesRead = 0;
            Assert(GetOverlappedResult(sRemoteHandle, &overlapped, &bytesRead, TRUE));
            
            buffered += bytesRead;
            readBuffer[buffered] = '\0';
            
            while(buffered)
            {
                char* eol = (char*)memchr(readBuffer, '\r', buffered);
                
                if(!eol)
                {
                    break;
                }
                
                *eol = '\0';
                HandleRemoteCommand(readBuffer);
                
                ++eol;
                
                while(eol < &readBuffer[buffered] && isspace(*eol))
                {
                    ++eol;
                }
                
                const DWORD remainder = static_cast<DWORD>(&readBuffer[buffered] - eol);
                memmove(readBuffer, eol, remainder + 1);
                buffered = remainder;
            }
        }
        else
        {
            Assert(!"Crash");
        }
    }
    
    SafeCloseHandle(eventHandle);

    return(0);
}

static void InitRemote()
{
    int numArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &numArgs);
    
    WCHAR comPort[5] = {0};

    for(int i = 1; i < numArgs; ++i)
    {
        if
        (
            (towupper(argv[i][0]) == 'C') && 
            (towupper(argv[i][1]) == 'O') && 
            (towupper(argv[i][2]) == 'M') && 
            isdigit(argv[i][3])  && 
            !argv[i][4]
        )
        {
            for(size_t c = 0; c < 4; ++c)
            {
                comPort[c] = towupper(argv[i][c]);
            }
            
            break;
        }
    }
    
    if(!comPort[0])
    {
        return;
    }

    sRemoteHandle = CreateFile
    (
        comPort,
        GENERIC_READ | GENERIC_WRITE,
        0,
        0,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL,
        0
    );
    
    if(sRemoteHandle == INVALID_HANDLE_VALUE)
    {
        MessageBox(NULL, TEXT("Could not open to the given COM port"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        return;
    }
    
    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    
    if(!GetCommState(sRemoteHandle, &dcbSerialParams))
    {
        MessageBox(NULL, TEXT("Could not read to the given COM port state"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        SafeCloseHandle(sRemoteHandle);
        return;
    }
    
    dcbSerialParams.BaudRate = CBR_9600;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    
    if(!SetCommState(sRemoteHandle, &dcbSerialParams))
    {
        MessageBox(NULL, TEXT("Could not write to the given COM port state"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        SafeCloseHandle(sRemoteHandle);
        return;
    }
    
    COMMTIMEOUTS timeouts={0};
      
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    
    if(!SetCommTimeouts(sRemoteHandle, &timeouts))
    {
        MessageBox(NULL, TEXT("Could not set timeous on the given COM port state"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        SafeCloseHandle(sRemoteHandle);
        return;
    }

    sRemoteSem = CreateSemaphore(NULL, 0, 1, NULL);
    Assert(sRemoteSem);

    unsigned int id = 0;
    sRemoteThread = reinterpret_cast<HANDLE>(_beginthreadex
    (
        NULL, 
        0,
        &RemoteProc,
        NULL,
        0,
        &id
    ));
    
    Assert(sRemoteThread);
}

static void ShutdownRemote()
{
    if(sRemoteSem)
    {
        Assert(ReleaseSemaphore(sRemoteSem, 1, NULL));
    }
    
    if(sRemoteThread)
    {
        Assert(WaitForSingleObject(sRemoteThread, INFINITE) == WAIT_OBJECT_0);
        SafeCloseHandle(sRemoteThread);
    }
    
    SafeCloseHandle(sRemoteSem);
    SafeCloseHandle(sRemoteHandle);
}

static void ShutownWindow()
{
    Assert(DestroyAcceleratorTable(sAcceleratorTable));
    sAcceleratorTable = NULL;

    sWindowHandle = NULL;
    Assert(UnregisterClass(CLASS_NAME, GetModuleHandle(NULL)));
    DeleteObject(sLabelFont);
    sLabelFont = NULL;
}

static void ShutdownMovies()
{
    for(Movies::iterator i = gMovies.begin(); i != gMovies.end(); ++i)
    {
        UnloadMovie(*i);
    }
}

static void ShutdownJoystick()
{
    SafeRelease(sJoystick);
    SafeRelease(sDirectInput);
}

void VlcLogCB(void*, int level, const libvlc_log_t*, const char* format, va_list args)
{
    if(level != LIBVLC_ERROR)
    {
        return;
    }

    char buffer[1024];
    const size_t bufferSize = ARRAY_COUNT(buffer);

    int rc = vsnprintf(buffer, bufferSize - 1, format, args);

    if(rc < 0)
    {
        return;
    }
    
    if((rc + 1) < bufferSize)
    {
        strcpy(buffer + rc, "\n");
    }

    Log(buffer);
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    OpenLog();

    srand(static_cast<unsigned>(time(NULL)));
    Assert(!FAILED(CoInitialize(NULL)));

    InitCommonControls();

    InitVolume();

    const char* args[] =
    {
        "--no-osd"
    };

    sVlc = libvlc_new(ARRAY_COUNT(args), args);

    if(!sVlc)
    {
        CloseLog();
        return(-1);
    }

    libvlc_log_set(sVlc, VlcLogCB, NULL);

    FindMovies();
    BuildChannels();
    LoadBedTimeMovie();
    LoadSensitivity();

    InitWindow();
    InitJoystick();
    InitRemote();
    StartThreads();
    
    TEMicrophone::Initialize(sWindowHandle);
    TEMicrophone::SetSensitivity(sMicrophoneSensitivityNormal);
    
    sDatabase.RecordEvent("start");
    PlayMovie();

    for(;;)
    { 
        MSG msg;
        BOOL gotMessage = GetMessage(&msg, sWindowHandle, 0, 0);
        
        if(static_cast<int>(gotMessage) <= 0)
        {
            break;
        }

        if(sAcceleratorTable && TranslateAccelerator(sWindowHandle, sAcceleratorTable, &msg))
        {
            continue;
        }

        TranslateMessage(&msg); 
        DispatchMessage(&msg);
    } 

    if(gPlayingState == PM_PLAYING)
    {
        StopMovie();
        sDatabase.RecordEvent("stop");
    }
    else if(gPlayingState == PM_LOADING)
    {
        StopMovie();
        sLoading = false;
        sDatabase.RecordEvent("stop");
    }

    if(sVlcPlayer)
    {
       libvlc_media_player_stop(sVlcPlayer);
       libvlc_media_player_release(sVlcPlayer);
       sVlcPlayer = NULL;
    }

    TEMicrophone::Shutdown();
    
    Assert(StopThreads());
    ShutdownRemote();
    ShutdownJoystick();
    StopMovie();
    SaveChannelPositions();
    ShutownWindow();
    ShutdownMovies();
    SaveSensitivity();
    
    if(sVlc)
    {
        libvlc_release(sVlc);
        sVlc = NULL;
    }
    
    ShutdownVolume();

    CoUninitialize();
    CloseLog();
    return(0);
}
