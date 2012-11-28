#include "TeeEeePch.h"
#include "Movies.h"

#include <algorithm>
#include <shellapi.h>

Movies gMovies;

Movies gShush;
Movies gTimeout;
Movies gDial;
Movies gGoodnight;

static const TCHAR* SPECIAL_PREFIXES[] = 
{
    TEXT("Shush"),
    TEXT("Timeout"),
    TEXT("Dial"),
    TEXT("Goodnight")
};

static const size_t SPECIAL_PREFIX_LENS[] = 
{
    5,
    7,
    4,
    9
};

static Movies* SPECIAL_MOVIES[] = 
{
    &gShush,
    &gTimeout,
    &gDial,
    &gGoodnight
};

static bool FileExists(const TCHAR* path)
{
    TCHAR buffer[MAX_PATH] = TEXT("");
    DWORD len = GetLongPathName(path, buffer, ARRAY_COUNT(buffer));
    return(len > 0);
}
       
static TCHAR* FindExtension(TCHAR* str)
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

static void ScanDir(const TCHAR* dir)
{
    WIN32_FIND_DATA findData = {0};
    
    TCHAR searchWildcard[MAX_PATH];
    
    _tcscpy(searchWildcard, dir);
    
    size_t len = _tcslen(dir);
    
    if((dir[len - 1] != '\\') && (dir[len - 1] != '/'))
    {
        _tcscat(searchWildcard, TEXT("\\"));
    }
    
    _tcscat(searchWildcard, TEXT("*"));
    
    HANDLE findHandle = FindFirstFile(searchWildcard, &findData);

    if(findHandle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    
    do
    {
        if(findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
        {
            continue;
        }
    
        if(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if((findData.cFileName[0] != '.') && (findData.cFileName[0] != '_'))
            {
                TCHAR subDir[MAX_PATH];
                _tcscpy(subDir, dir);
                _tcscat(subDir, TEXT("\\"));
                _tcscat(subDir, findData.cFileName);
                ScanDir(subDir);
            }
        
            continue;
        }

        Movie movie = {0};
        
        _tcscpy(movie.name, findData.cFileName);
        
        TCHAR* ext = FindExtension(movie.name);
        
        if(!ext || (_tcscmp(ext, TEXT(".avi")) && _tcscmp(ext, TEXT(".mp4"))))
        {
            continue;
        }
        
        if(ext)
        {
            *ext = '\0';
        }

        _tcscpy(movie.path, dir);

        if((dir[len - 1] != '\\') && (dir[len - 1] != '/'))
        {
            _tcscat(movie.path, TEXT("\\"));
        }

        _tcscat(movie.path, findData.cFileName);
        
        bool isSpecial = false;
        
        for(size_t s = 0; s < ARRAY_COUNT(SPECIAL_PREFIXES); ++s)
        {
            if
            (
                (_tcsncmp(findData.cFileName, SPECIAL_PREFIXES[s], SPECIAL_PREFIX_LENS[s]) == 0) &&
                (_istdigit(findData.cFileName[SPECIAL_PREFIX_LENS[s]]))
            )
            {
                SPECIAL_MOVIES[s]->push_back(movie);
                isSpecial = true;
                break;
            }
        }
        
        if(isSpecial)
        {
            continue;
        }

        _tcscpy(movie.coverPath, movie.path);
        ext = FindExtension(movie.coverPath);
    
        if(ext)
        {
            _tcscpy(ext, TEXT(".jpg"));
        } 

        if(!FileExists(movie.coverPath))
        {
            _tcscpy(movie.coverPath, dir);
            _tcscat(movie.coverPath, TEXT(".jpg"));

            if(!FileExists(movie.coverPath))
            {
                TCHAR messageBuf[1024];
                _sntprintf(messageBuf, ARRAY_COUNT(messageBuf), TEXT("No cover found for: %s\n"), movie.name);
                OutputDebugString(messageBuf);
                movie.coverPath[0] = '\0';
                
                /* MessageBox(NULL, messageBuf, TEXT("TeeEee"), MB_OK | MB_ICONERROR); */
            }
        }

        gMovies.push_back(movie);
       
        Util::Zeroize(&findData, sizeof(findData));
    } while(FindNextFile(findHandle, &findData));    
    
    Assert(FindClose(findHandle));    
}

void FindMovies()
{
    int numArgs = 0;
    
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &numArgs);

    if(numArgs < 1)
    {
        MessageBox(NULL, TEXT("You need to specify the movie dirs on the command-line"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        exit(0);
    }
  
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
            continue;
        }
        
        size_t before = gMovies.size();
        ScanDir(argv[i]);
        size_t after = gMovies.size();
        
        if(before == after)
        {
            TCHAR messageBuf[1024];
            _sntprintf(messageBuf, ARRAY_COUNT(messageBuf), TEXT("No movies found in: %s"), argv[i]);
            MessageBox(NULL, messageBuf, TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        }
    }
        
    if(gMovies.empty())
    {
        MessageBox(NULL, TEXT("You need to specify the movie dirs on the command-line"), TEXT("TeeEee"), MB_OK | MB_ICONERROR);
        exit(0);
    }
}

void UnloadMovie(Movie& movie)
{
    if(movie.state == Movie::MS_DORMANT)
    {
        Assert(!movie.cover);
        return;
    }
    
    if(movie.state != Movie::MS_LOADED)
    {
        return;
    }
    
    if(movie.cover)
    {
        Assert(DeleteObject(movie.cover));
        movie.cover = NULL;
    }    

    movie.state = Movie::MS_DORMANT;
    
    TCHAR logBuffer[512];
    _sntprintf(logBuffer, ARRAY_COUNT(logBuffer), TEXT("Unloaded %s\n"), movie.name);
    OutputDebugString(logBuffer);
}
