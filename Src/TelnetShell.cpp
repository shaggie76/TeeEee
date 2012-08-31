#include "TeeEeePch.h"
#include "TelnetShell.h"

// ENABLE_TELNET_SHELL

#ifndef ENABLE_TELNET_SHELL
void TelnetShell::Initialize(HWND) {}
void TelnetShell::Shutdown() {}
#else

#include <process.h>

static HWND sWindowHandle = NULL;
static SOCKET sListenSocket = INVALID_SOCKET;
static HANDLE sListenThread = NULL;

static void HandleTelnetCommand(char c)
{
    c = static_cast<char>(toupper(c));
    
    switch(c)
    {
        case 'S':
        case 'T':
            PostMessage(sWindowHandle, WM_KEYDOWN, c, 0);
            break;
    }
}

const BYTE TELNET_IAC = 255;

const BYTE TELNET_WILL = 251;
const BYTE TELNET_WONT = 252;
const BYTE TELNET_DO = 253;
const BYTE TELNET_DONT = 254;

const BYTE TELNET_ECHO = 1;
const BYTE TELNET_SGA = 3;
const BYTE TELNET_LINEMODE = 34;

static unsigned __stdcall ListenProc(void *)
{
    for(;;)
    {
        SOCKET clientSocket = accept(sListenSocket, NULL, NULL);
        
        if(clientSocket == INVALID_SOCKET)
        {
            break;
        }

        BYTE readBuffer[1024] = "";
        int buffered = 0;
        
        for(;;)
        {
            fd_set readfds = {0};
            
            readfds.fd_count = 2;
            readfds.fd_array[0] = sListenSocket;
            readfds.fd_array[1] = clientSocket;

            fd_set exceptfds = readfds;
            
            if(select(2, &readfds, NULL, &exceptfds, NULL) == SOCKET_ERROR)
            {
                break;
            }

            if(exceptfds.fd_count > 0)
            {
                break;
            }
            
            for(u_int i = 0; i < readfds.fd_count; ++i)
            {
                SOCKET other = readfds.fd_array[i];
                
                if(other == sListenSocket)
                {
                    // Reject extra connections
                    SOCKET clientSocket2 = accept(sListenSocket, NULL, NULL);
                
                    if(clientSocket2 != INVALID_SOCKET)
                    {
                        shutdown(clientSocket2, SD_SEND);
                        closesocket(clientSocket2);
                    }
                }
                else if(other == clientSocket)
                {
                    int bytesToRead = static_cast<int>(ARRAY_COUNT(readBuffer)) - (buffered + 1);
                    
                    int received = recv(clientSocket, reinterpret_cast<char*>(readBuffer + buffered), bytesToRead, 0);
                    
                    if(received <= 0)
                    {
                        goto disconnectClient;
                    }

                    buffered += received;
                    
                    bool clearBuffer = true;
                    
                    for(int i = 0; i < buffered; ++i)
                    {
                        if(readBuffer[i] == TELNET_IAC)
                        {
                            if((i + 2) > buffered)
                            {
                                const DWORD remainder = static_cast<DWORD>(buffered - i);
                                memmove(readBuffer, &readBuffer[i], remainder + 1);
                                buffered = remainder;
                                clearBuffer = false;
                                break;
                            }
                         
                            const BYTE verb = readBuffer[i + 1];
                            
                            if((verb >= TELNET_WILL) && (verb <= TELNET_DONT))
                            {
                                // reply to all commands with "WONT", unless it is SGA (suppres go ahead)
                                char command = readBuffer[i + 2];

                                BYTE reply;

                                if((command == TELNET_SGA) || (command == TELNET_ECHO))
                                {
                                     reply = (verb == TELNET_DO) ? TELNET_WILL : TELNET_DO;
                                }
                                else
                                {
                                     reply = (verb == TELNET_DO) ? TELNET_WONT : TELNET_DONT;
                                }
                                
                                BYTE replyPacket[] = { TELNET_IAC, reply, command };
                                send(clientSocket, reinterpret_cast<const char*>(replyPacket), sizeof(replyPacket), 0);
                            }
                        
                            i += 2;
                            continue;
                        }
                        
                        HandleTelnetCommand(readBuffer[i]);
                    } 
                    
                    if(clearBuffer)
                    {
                        buffered = 0;
                    }
                }
            }
        }
        
disconnectClient:        
        
        shutdown(clientSocket, SD_SEND);
        closesocket(clientSocket);
    }

    return(0);
}

void TelnetShell::Initialize(HWND windowHandle)
{
    sWindowHandle = windowHandle;
    
    WSADATA wsaData = {0};
    Assert(!WSAStartup(MAKEWORD(2,2), &wsaData));
    
    sListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in service = {0};
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = htonl(INADDR_ANY);
    service.sin_port = htons(23);

    Assert(bind(sListenSocket, reinterpret_cast<sockaddr*>(&service), sizeof(service)) != SOCKET_ERROR);
    Assert(listen(sListenSocket, SOMAXCONN) != SOCKET_ERROR);

    unsigned int id = 0;
    sListenThread = reinterpret_cast<HANDLE>(_beginthreadex
    (
        NULL, 
        0,
        &ListenProc,
        NULL,
        0,
        &id
    ));
    
    Assert(sListenThread);
}

void TelnetShell::Shutdown()
{
    if(sListenSocket != INVALID_SOCKET)
    {
        closesocket(sListenSocket);
        sListenSocket = INVALID_SOCKET;
    }

    if(sListenThread)
    {
        Assert(WaitForSingleObject(sListenThread, INFINITE) == WAIT_OBJECT_0);
        SafeCloseHandle(sListenThread);
    }
    
    sWindowHandle = NULL;
    WSACleanup();
}

#endif // ENABLE_TELNET_SHELL
