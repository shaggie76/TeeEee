#include "TeeEeePch.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

static HANDLE sLogFile = NULL;

void OpenLog()
{
    Assert(!sLogFile);

    TCHAR fileName[MAX_PATH] = TEXT("");
    Assert(GetModuleFileName(GetModuleHandle(NULL), fileName, ARRAY_COUNT(fileName)));

    TCHAR* dot = _tcsrchr(fileName, '.');
    Assert(dot);

    _tcscpy(dot, TEXT(".log"));

    const DWORD desiredAccess = GENERIC_WRITE | GENERIC_READ;
    const DWORD creationDisposition = CREATE_ALWAYS;
    const DWORD shareMode = FILE_SHARE_READ;

    LPSECURITY_ATTRIBUTES securityAttributes = NULL;
    DWORD flagsAndAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE templateFile = NULL;

    HANDLE h = CreateFile(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);

    if(h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    
    sLogFile = h;
}

void CloseLog()
{
    if(sLogFile)
    {
        Assert(FlushFileBuffers(sLogFile));
        SafeCloseHandle(sLogFile);
    }
}

void Log(const char* buffer)
{
    OutputDebugStringA(buffer);

    if(!sLogFile)
    {
        return;
    }

    char outputBuffer[4096];
    size_t outputPending = 0;
    DWORD numberOfBytesWritten = 0;

    // Translate \n => \r\n when writing files (since we don't use LIBC for this
    // IO layer we have to do this ourselves).

    for(const char* p = buffer; *p; ++p)
    {
        if(*p == '\r')
        {
            continue;
        }

        if(*p == '\n')
        {
            outputBuffer[outputPending++] = '\r';
                
            if(outputPending == ARRAY_COUNT(outputBuffer))
            {
                if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL))
                {
                    OutputDebugString(TEXT("Log write failed\n"));
                    break;
                }
                outputPending = 0;
            }
        }
            
        outputBuffer[outputPending++] = *p;

        if(outputPending == ARRAY_COUNT(outputBuffer))
        {
            if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL))
            {
                OutputDebugString(TEXT("Log write failed\n"));
                break;
            }
            outputPending = 0;
        }
    }

    if(outputPending)
    {
        if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL)) //lint !e772: Symbol outputBuffer conceivably not initialized
        {
            OutputDebugString(TEXT("Log write failed\n"));
        }
    }
}

void Log(const TCHAR* buffer)
{
    OutputDebugString(buffer);

    if(!sLogFile)
    {
        return;
    }

    char outputBuffer[4096];
    size_t outputPending = 0;
    DWORD numberOfBytesWritten = 0;

    // Translate \n => \r\n when writing files (since we don't use LIBC for this
    // IO layer we have to do this ourselves).

    for(const TCHAR* p = buffer; *p; ++p)
    {
        if(*p == '\r')
        {
            continue;
        }

        if(*p == '\n')
        {
            outputBuffer[outputPending++] = '\r';
                
            if(outputPending == ARRAY_COUNT(outputBuffer))
            {
                if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL))
                {
                    OutputDebugString(TEXT("Log write failed\n"));
                    break;
                }
                outputPending = 0;
            }
        }
            
        outputBuffer[outputPending++] = static_cast<char>(*p); // poor-mans wide-to-ansi

        if(outputPending == ARRAY_COUNT(outputBuffer))
        {
            if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL))
            {
                OutputDebugString(TEXT("Log write failed\n"));
                break;
            }
            outputPending = 0;
        }
    }

    if(outputPending)
    {
        if(!WriteFile(sLogFile, outputBuffer, static_cast<DWORD>(outputPending), &numberOfBytesWritten, NULL)) //lint !e772: Symbol outputBuffer conceivably not initialized
        {
            OutputDebugString(TEXT("Log write failed\n"));
        }
    }
}

void LogF(const TCHAR* format, ...)
{
    va_list args = NULL;
    va_start(args, format);

    TCHAR buffer[1024];
    const size_t bufferSize = ARRAY_COUNT(buffer);

    int rc = _vsntprintf(buffer, bufferSize - 1, format, args);

    if(rc < 0)
    {
        Log(format);
    }
    else
    {
        Log(buffer);
    }

    va_end(args);
}

