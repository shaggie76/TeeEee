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

#include <vlc/vlc.h>

/*
    TODO: should be able to fade to black now
    - disable agc for shush?

    SSE microphone i16->fp

    FFTW_MEASURE 
*/

#pragma comment(lib, "Dinput8.lib")
#pragma comment(lib, "dxguid.lib")

#pragma comment(lib, "libvlccore.lib")
#pragma comment(lib, "libvlc.lib")

#define RECYCLE_PLAYER_INSTANCE

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
    
const UINT SENSITIVITY_STEPS = 10;

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

static bool sMute = false;
static float sVolume = 0.f; 
static float sMicrophoneSensitivity = 0.f;

const UINT_PTR ON_SIZE_TIMER = 1;
const UINT ON_SIZE_DELAY = 500; // 1/2 second

const UINT_PTR JOYSTICK_INPUT_TIMER = 2;
const UINT JOYSTICK_INPUT_DELAY = 1000 / 60; // 60Hz

const UINT_PTR SET_VOLUME_TIMER = 3;
const UINT SET_VOLUME_DELAY = 10 * 1000; // 10-seconds

const UINT_PTR HIDE_CURSOR_TIMER = 4;
const UINT HIDE_CURSOR_DELAY = 10 * 1000; // 10 sec

static time_t sPreviousShushTimes[16] = {0};
static size_t sPreviousShushIndex = 0;
static size_t sTimeoutIndex = 0;

static libvlc_instance_t* sVlc = NULL;
static libvlc_media_player_t* sVlcPlayer = NULL;
static libvlc_time_t sPauseTime = -1;

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
    PM_SHUSH,
    PM_TIMEOUT
};

static PlayingState gPlayingState = PM_IDLE;

enum ShushType
{
    ST_SHUSH,
    ST_TIMEOUT
};

static ShushType gShushType = ST_SHUSH; // Active for current shush

static IDirectInput8* sDirectInput = NULL;
static IDirectInputDevice8* sJoystick = NULL;
static DIJOYSTATE2 sPrevJoyState = {0};

const size_t SLEEP_INTERVALS = 4;
static time_t sSleepTime = 0; // If non-zero will sleep after this.

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
            TCHAR logBuffer[512];
            _sntprintf(logBuffer, ARRAY_COUNT(logBuffer), TEXT("Failed to load %s\n"), movie->coverPath);
            OutputDebugString(logBuffer);
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
        
        TCHAR logBuffer[512];
        _sntprintf(logBuffer, ARRAY_COUNT(logBuffer), TEXT("Loaded %s\n"), movie->name);
        OutputDebugString(logBuffer);
        
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


const TCHAR* SENSITIVITY_REG_KEY = TEXT("SOFTWARE\\TeeEee");

static void LoadSensitivity()
{
    HKEY key = NULL;
    
    if(RegOpenKeyEx(HKEY_CURRENT_USER, SENSITIVITY_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        sMicrophoneSensitivity = 1.f;
        return;
    }

    DWORD valueSize = sizeof(sMicrophoneSensitivity);
    DWORD valueType = REG_DWORD;

    bool gotValue = (RegQueryValueEx(key, TEXT("Sensitivity"), 0, &valueType, reinterpret_cast<LPBYTE>(&sMicrophoneSensitivity), &valueSize) == ERROR_SUCCESS);
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
    
    if(!gotValue || (sMicrophoneSensitivity < 0.f) || (sMicrophoneSensitivity > 1.f))
    {
        sMicrophoneSensitivity = 1.f;
        return;
    }
}

static void SaveSensitivity()
{
    HKEY key = NULL;
    
    if(RegCreateKeyEx
    (
        HKEY_CURRENT_USER,
        SENSITIVITY_REG_KEY, 
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
        reinterpret_cast<const BYTE*>(&sMicrophoneSensitivity),
        static_cast<DWORD>(sizeof(sMicrophoneSensitivity))
    ) != ERROR_SUCCESS)
    {
        Assert(!"Could not set value.");
    }
    
    Assert(RegCloseKey(key) == ERROR_SUCCESS);
}

const TCHAR* CHANNEL_POSITION_REG_KEY = TEXT("SOFTWARE\\TeeEee\\Channels");

struct NameSort
{
    bool operator()(const Movie& a, const Movie& b) const
    {
        return(_tcscmp(a.path, b.path) <= 0);
    }
};

static void BuildChannels()
{
    Assert(!gMovies.empty());
    
    // Separate movies into one channel per directory  
      
    std::sort(gMovies.begin(), gMovies.end(), NameSort());
    std::sort(gDial.begin(), gDial.end(), NameSort()); /* TODO: fix these */
    
    TCHAR prevDir[MAX_PATH] = {0};
    sChannels.reserve(32);
    
    for(Movies::iterator i = gMovies.begin(), end = gMovies.end(); i != end; ++i)
    {
        Movie& movie = *i;
        
        const TCHAR* baseName = FindBaseName(movie.coverPath);
        size_t dirLen = static_cast<size_t>(baseName - movie.coverPath);
        
        TCHAR dir[MAX_PATH];
        _tcsncpy(dir, movie.coverPath, dirLen);
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

    Movie& movie = sSleepTime ? *sBedTimeMovie : *sCurrentChannel->movies.front();
        
    gPlayingState = PM_IDLE;

    bool stopped = false;

    if(sVlcPlayer && libvlc_media_player_is_playing(sVlcPlayer))
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
        OutputDebugString(TEXT("Stopped "));
        OutputDebugString(movie.name);
        OutputDebugString(TEXT("\n"));
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

// Converts float (dB) to VLC volume (0 = mute, 100 = nominal / 0dB)
static int GetVlcVolume()
{
    if(sMute)
    {
        return(0);
    }

    float volume = sVolume + GAIN_HEADROOM;
    
    if(sSleepTime)
    {
        time_t now;
        time(&now);

        double secondsUntilSleep = std::max(difftime(sSleepTime, now), 0.0);
        double maxTimeUntilSleep = 90.0 * 60.0; // 1.5 hours
        
        // As we approach the cut-off, turn the volume down.
        if(secondsUntilSleep < maxTimeUntilSleep)
        {
            volume -= 30.f * (1.f - static_cast<float>(secondsUntilSleep / maxTimeUntilSleep));
        }
        else if(sSleepTime < now)
        {
            return(0);
        }
    }

    //  has 0-100 linear range
    int vlcVolume = Round(100.f * DecibelsToVolume(volume));
    return(Clamp(vlcVolume, 0, 100));
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
    OutputDebugString(TEXT("MovieCompleted\n"));
    PostMessage(sWindowHandle, WM_MOVIE_COMPLETED, 0, 0);
}

static void PlayerErrorCB(const libvlc_event_t*, void*)
{
    OutputDebugString(TEXT("PlayerErrorCB\n"));
    PostMessage(sWindowHandle, WM_MOVIE_COMPLETED, 0, 0);
}

static void LengthChangedCB(const libvlc_event_t*, void*)
{
    // Must be deferred to main thread:
    OutputDebugString(TEXT("LengthChangedCB\n"));
    PostMessage(sWindowHandle, WM_LENGTH_CHANGED, 0, 0);
}

static void PlayMovieEx(Movie& movie, PlayingState newState)
{
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
        TCHAR logBuffer[512];
        _sntprintf(logBuffer, ARRAY_COUNT(logBuffer), TEXT("Could not load media %s\n"), movie.path);
        OutputDebugString(logBuffer);
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
            TCHAR logBuffer[512];
            _sntprintf(logBuffer, ARRAY_COUNT(logBuffer), TEXT("Could not create player for %s\n"), movie.path);
            OutputDebugString(logBuffer);
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

    libvlc_audio_set_volume(sVlcPlayer, GetVlcVolume());
    libvlc_media_player_play(sVlcPlayer);

    Assert(SetTimer(sWindowHandle, SET_VOLUME_TIMER, SET_VOLUME_DELAY, NULL));

    /* Assert(InvalidateRect(sWindowHandle, NULL, TRUE));*/
    
    OutputDebugString(TEXT("Started "));
    OutputDebugString(movie.name);
    OutputDebugString(TEXT("\n"));
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

        OutputDebugString(TEXT("Resumed "));
        OutputDebugString(movie.name);
        OutputDebugString(TEXT("\n"));
        
        return;
    }

    if((gPlayingState == PM_SHUSH) || (gPlayingState == PM_TIMEOUT))
    {
        sTimeoutIndex = gDial.size(); // INVALID
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
    
    OutputDebugString(TEXT("Paused "));
    OutputDebugString(movie.name);
    OutputDebugString(TEXT("\n"));
}

static void OnMovieComplete()
{
    const PlayingState prevState = gPlayingState;

    StopMovie();

    if(prevState == PM_SHUSH)
    {
        if(gShushType == ST_TIMEOUT)
        {
            PlayMovieEx(gDial[sTimeoutIndex], PM_TIMEOUT);
            TEMicrophone::SetSensitivity(1.f);
            return;
        }
    }
    else if(prevState == PM_TIMEOUT)
    {
        sTimeoutIndex = gDial.size(); // INVALID
    }
    else if(prevState != PM_TIMEOUT)
    {
        AdvanceCurrentChannel();
    }
        
    TEMicrophone::SetSensitivity(sMicrophoneSensitivity);
    PlayMovie();
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

static void OnShush()
{
    OutputDebugString(TEXT("Shush!\n"));
    
    if(gShush.empty())
    {
        return;
    }

    if((gPlayingState == PM_IDLE) || (gPlayingState == PM_PAUSED) || (gPlayingState == PM_SHUSH))
    {
        return;
    }

    time_t now;
    time(&now);
    sPreviousShushTimes[sPreviousShushIndex] = now;
    sPreviousShushIndex = (sPreviousShushIndex + 1) % ARRAY_COUNT(sPreviousShushTimes);
    
    if((CountRecentShushes(60.0) >= 2) || (sTimeoutIndex < gDial.size()))
    {
        gShushType = ST_TIMEOUT;
        
        if(sTimeoutIndex >= gDial.size()) // INVALID
        {
            sTimeoutIndex = 0;
        }
        else
        {
            if(sTimeoutIndex + 1 < gDial.size())
            {
               ++sTimeoutIndex; 
            }
        }
    }
    else
    {
        gShushType = ST_SHUSH;
    }
    
    Movies& movies = sSleepTime ? gGoodnight : (gShushType == ST_TIMEOUT) ? gTimeout : gShush;
    PlayMovieEx(movies[rand() % movies.size()], PM_SHUSH);
}

static void OnTimeout()
{
    OutputDebugString(TEXT("Timeout!\n"));
    
    if(gShush.empty())
    {
        return;
    }

    if((gPlayingState == PM_IDLE) || (gPlayingState == PM_PAUSED) || (gPlayingState == PM_SHUSH))
    {
        return;
    }

    time_t now;
    time(&now);
    
    for(size_t i = 0; i < ARRAY_COUNT(sPreviousShushTimes); ++i)
    {
        sPreviousShushTimes[i] = now;
    }
    
    gShushType = ST_TIMEOUT;
    Movies& movies = sSleepTime ? gGoodnight : (gShushType == ST_TIMEOUT) ? gTimeout : gShush;
    PlayMovieEx(movies[rand() % movies.size()], PM_SHUSH);
}

static void NextChannel()
{
    if(gMovies.empty() || sSleepTime)
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
        PlayMovie();
    }
    else
    {
        LoadVolumeForMovie();
        Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
    }
}

static void PrevChannel()
{
    if(gMovies.empty() || sSleepTime)
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
        PlayMovie();
    }
    else
    {
        LoadVolumeForMovie();
        Assert(InvalidateRect(sWindowHandle, NULL, TRUE));
    }
}    

static void HandleRemoteButton(size_t buttonId)
{
    if(sSleepTime || (gPlayingState > PM_PLAYING))
    {
        return;
    }

#if 1
    if(buttonId & 1)
    {
        NextChannel();
    }
    else
    {
        PrevChannel();
    }
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

struct MatchNonCharacters
{
    const bool operator()(const Movie& m)
    {
        return(_istalpha(m.name[0]) == 0);
    }
    
    void CalcMenuName(TCHAR* buffer, size_t bufferSize) const
    {
        _sntprintf(buffer, bufferSize, TEXT("Movies 0-9"));
    }
};

struct MatchCharacter
{
    explicit MatchCharacter(TCHAR c) :
        mMatchChar(_totupper(c))
    {
    }
    
    const bool operator()(const Movie& m)
    {
        return(_totupper(m.name[0]) == mMatchChar);
    }
        
    void CalcMenuName(TCHAR* buffer, size_t bufferSize) const
    {
        _sntprintf(buffer, bufferSize, TEXT("Movies %c"), mMatchChar);
    }
    
private:
    MatchCharacter();
    TCHAR mMatchChar;
};

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
                if(sVlcPlayer)
                {
                    libvlc_audio_set_volume(sVlcPlayer, GetVlcVolume());
                }

                if(sSleepTime)
                {
                    time_t now;
                    time(&now);
                    
                    if(now > sSleepTime)
                    {
                        OutputDebugString(TEXT("Sleep timer done"));
                        sSleepTime = 0;

                        StopMovie();
                    }
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

                    TCHAR* coverPath = sChannels[i].movies.front()->coverPath;
                    TCHAR* baseName = FindBaseName(coverPath);
                    size_t dirLen = static_cast<size_t>(baseName - coverPath);
                    
                    TCHAR dir[MAX_PATH];
                    _tcsncpy(dir, coverPath, dirLen);
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

                Assert(AppendMenu(menuHandle, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(subMenu), TEXT("Bed-Time Movie")));

                MakeIndexSubMenu(subMenu, COMMAND_BED_TIME_MOVIE_INDEX, MatchNonCharacters(), gMovies, sBedTimeMovie);
                
                for(size_t i = 'A'; i <= 'Z'; ++i)
                {
                    MakeIndexSubMenu(subMenu, COMMAND_BED_TIME_MOVIE_INDEX, MatchCharacter(static_cast<TCHAR>(i)), gMovies, sBedTimeMovie);
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

                UINT index = static_cast<UINT>((1.f - sMicrophoneSensitivity) * static_cast<float>(SENSITIVITY_STEPS));
                
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
                        Assert(SetWindowPos(windowHandle, HWND_NOTOPMOST, x, y, dx, dy, SWP_DRAWFRAME | SWP_SHOWWINDOW | SWP_NOREDRAW));
                        Assert(InvalidateRect(NULL, NULL, TRUE));
                    }
                    else
                    {
                        CalcFullscreenLayout(x, y, dx, dy);
                        Assert(SetWindowPos(windowHandle, HWND_TOPMOST, x, y, dx, dy, SWP_DRAWFRAME | SWP_SHOWWINDOW | SWP_NOREDRAW));
                    }
                    
                    Assert(InvalidateRect(windowHandle, NULL, TRUE));
                    return(0);
                }
                
                case COMMAND_SLEEP_MODE_DISABLED:
                {
                    sSleepTime = 0;
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
                
                if(sVlcPlayer)
                {
                    libvlc_audio_set_volume(sVlcPlayer, GetVlcVolume());
                }

                SaveVolumeForMovie();
                return(0);
            }
            
            if((LOWORD(wParam) >= COMMAND_MICROPHONE_SENSITIVITY) && (LOWORD(wParam) < COMMAND_MICROPHONE_SENSITIVITY + SENSITIVITY_STEPS))
            {
                UINT index = LOWORD(wParam) - COMMAND_MICROPHONE_SENSITIVITY;
                sMicrophoneSensitivity = 1.f - (static_cast<float>(index) / static_cast<float>(SENSITIVITY_STEPS - 1));
                TEMicrophone::SetSensitivity(sMicrophoneSensitivity);
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
            
                sBedTimeMovie = &gMovies[static_cast<size_t>(LOWORD(wParam) - COMMAND_BED_TIME_MOVIE_INDEX)];
                Assert(InvalidateRect(windowHandle, NULL, TRUE));
                SaveBedTimeMovie();

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
            
                time(&sSleepTime);
                UINT hours = (LOWORD(wParam) - COMMAND_SLEEP_MODE) + 1;
                sSleepTime += hours * 60 * 60;

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
            
            if(wParam == 'T')
            {
                OnTimeout();
                return(0);
            }

            if(wParam == VK_MEDIA_PLAY_PAUSE)
            {
                if(gPlayingState == PM_PLAYING)
                {
                    PauseMovie();
                }
                else
                {
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

#ifdef _DEBUG
    windowStyle |= WS_DLGFRAME;
#endif
    
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
        OutputDebugString(TEXT("Could not find joystick"));
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

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    srand(static_cast<unsigned>(time(NULL)));
    Assert(!FAILED(CoInitialize(NULL)));

    const char* args[] =
    {
        "--no-osd"
    };

    sVlc = libvlc_new(ARRAY_COUNT(args), args);

    if(!sVlc)
    {
        return(-1);
    }

    FindMovies();
    BuildChannels();
    LoadBedTimeMovie();
    LoadSensitivity();

    InitWindow();
    InitJoystick();
    InitRemote();
    StartThreads();
    
    TEMicrophone::Initialize(sWindowHandle);
    TEMicrophone::SetSensitivity(sMicrophoneSensitivity);
    
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

    CoUninitialize();
    
    return(0);
}
