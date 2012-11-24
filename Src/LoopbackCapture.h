#ifndef LOOPBACK_CAPTURE_H
#define LOOPBACK_CAPTURE_H

// call CreateThread on this function
// feed it the address of a LoopbackCaptureThreadFunctionArguments
// it will capture via loopback from the IMMDevice
// and dump output to the HMMIO
// until the stop event is set
// any failures will be propagated back via hr

#include <vector>

typedef void (*PacketCallback)(std::vector<float>& packet);

struct LoopbackCaptureThreadFunctionArguments
{
    HANDLE hStartedEvent;
    HANDLE hStopEvent;
    UINT32 nFrames;
    HRESULT hr;
    PacketCallback callback;
};

DWORD WINAPI LoopbackCaptureThreadFunction(LPVOID pContext);

#endif // LOOPBACK_CAPTURE_H
