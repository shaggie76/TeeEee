#include "TeeEeePch.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

#include "LoopbackCapture.h"

static HRESULT get_default_device(IMMDevice **ppMMDevice) {
    HRESULT hr = S_OK;
    IMMDeviceEnumerator *pMMDeviceEnumerator;

    // activate a device enumerator
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
        __uuidof(IMMDeviceEnumerator),
        (void**)&pMMDeviceEnumerator
    );
    if (FAILED(hr)) {
        printf("CoCreateInstance(IMMDeviceEnumerator) failed: hr = 0x%08x\n", hr);
        return hr;
    }

    // get the default render endpoint
    hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, ppMMDevice);
    pMMDeviceEnumerator->Release();
    if (FAILED(hr)) {
        printf("IMMDeviceEnumerator::GetDefaultAudioEndpoint failed: hr = 0x%08x\n", hr);
        return hr;
    }

    return S_OK;
}

HRESULT LoopbackCapture(
    HANDLE hStartedEvent,
    HANDLE hStopEvent,
    PUINT32 pnFrames,
    PacketCallback callback
) {
    HRESULT hr;
    IMMDevice* pMMDevice = NULL;
    Assert(SUCCEEDED(get_default_device(&pMMDevice)));

    // activate an IAudioClient
    IAudioClient *pAudioClient;
    hr = pMMDevice->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL, NULL,
        (void**)&pAudioClient
    );
    if (FAILED(hr)) {
        printf("IMMDevice::Activate(IAudioClient) failed: hr = 0x%08x", hr);
        return hr;
    }
    
    // get the default device periodicity
    REFERENCE_TIME hnsDefaultDevicePeriod;
    hr = pAudioClient->GetDevicePeriod(&hnsDefaultDevicePeriod, NULL);
    if (FAILED(hr)) {
        printf("IAudioClient::GetDevicePeriod failed: hr = 0x%08x\n", hr);
        pAudioClient->Release();
        return hr;
    }

    // get the default device format
    WAVEFORMATEX *pwfx;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        printf("IAudioClient::GetMixFormat failed: hr = 0x%08x\n", hr);
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        return hr;
    }

#if 0
    if(pwfx->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
    {
        Assert(pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE);
        PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
        Assert(IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat));
    }
#endif

    {
        // coerce to mono-float/48000
        // can do this in-place since we're not changing the size of the format
        // also, the engine will auto-convert from float to int for us
        switch (pwfx->wFormatTag) {
            case WAVE_FORMAT_PCM:
                pwfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                pwfx->nSamplesPerSec = 48000;
                pwfx->wBitsPerSample = 32;
                pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
                pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
                break;

            case WAVE_FORMAT_EXTENSIBLE:
                {
                    // naked scope for case-local variable
                    PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
                    if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_PCM, pEx->SubFormat)) {
                        pEx->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                        pEx->Samples.wValidBitsPerSample = 32;
                        pwfx->wBitsPerSample = 32;
                        pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
                        pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
                    } else if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat)) {
                        pwfx->nSamplesPerSec = 48000;
                        pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
                        pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
                    } else {
                        printf("Don't know how to coerce mix format to int-16\n");
                        CoTaskMemFree(pwfx);
                        pAudioClient->Release();
                        return E_UNEXPECTED;
                    }
                }
                break;

            default:
                printf("Don't know how to coerce WAVEFORMATEX with wFormatTag = 0x%08x to int-16\n", pwfx->wFormatTag);
                CoTaskMemFree(pwfx);
                pAudioClient->Release();
                return E_UNEXPECTED;
        }
    }

    const DWORD nBlockAlign = pwfx->nBlockAlign;

    // create a periodic waitable timer
    HANDLE hWakeUp = CreateWaitableTimer(NULL, FALSE, NULL);
    if (NULL == hWakeUp) {
        DWORD dwErr = GetLastError();
        printf("CreateWaitableTimer failed: last error = %u\n", dwErr);
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        return HRESULT_FROM_WIN32(dwErr);
    }

    *pnFrames = 0;
    
    // call IAudioClient::Initialize
    // note that AUDCLNT_STREAMFLAGS_LOOPBACK and AUDCLNT_STREAMFLAGS_EVENTCALLBACK
    // do not work together...
    // the "data ready" event never gets set
    // so we're going to do a timer-driven loop
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        0, 0, pwfx, 0
    );
    if (FAILED(hr)) { 
        printf("IAudioClient::Initialize failed: hr = 0x%08x\n", hr);
        CloseHandle(hWakeUp);
        pAudioClient->Release();
        return hr;
    }
    CoTaskMemFree(pwfx);

    // activate an IAudioCaptureClient
    IAudioCaptureClient *pAudioCaptureClient;
    hr = pAudioClient->GetService(
        __uuidof(IAudioCaptureClient),
        (void**)&pAudioCaptureClient
    );
    if (FAILED(hr)) {
        printf("IAudioClient::GetService(IAudioCaptureClient) failed: hr 0x%08x\n", hr);
        CloseHandle(hWakeUp);
        pAudioClient->Release();
        return hr;
    }
    
    // register with MMCSS
    DWORD nTaskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Capture", &nTaskIndex);
    if (NULL == hTask) {
        DWORD dwErr = GetLastError();
        printf("AvSetMmThreadCharacteristics failed: last error = %u\n", dwErr);
        pAudioCaptureClient->Release();
        CloseHandle(hWakeUp);
        pAudioClient->Release();
        return HRESULT_FROM_WIN32(dwErr);
    }    

    // set the waitable timer
    LARGE_INTEGER liFirstFire;
    liFirstFire.QuadPart = -hnsDefaultDevicePeriod / 2; // negative means relative time
    LONG lTimeBetweenFires = (LONG)hnsDefaultDevicePeriod / 2 / (10 * 1000); // convert to milliseconds
    BOOL bOK = SetWaitableTimer(
        hWakeUp,
        &liFirstFire,
        lTimeBetweenFires,
        NULL, NULL, FALSE
    );
    if (!bOK) {
        DWORD dwErr = GetLastError();
        printf("SetWaitableTimer failed: last error = %u\n", dwErr);
        AvRevertMmThreadCharacteristics(hTask);
        pAudioCaptureClient->Release();
        CloseHandle(hWakeUp);
        pAudioClient->Release();
        return HRESULT_FROM_WIN32(dwErr);
    }
    
    // call IAudioClient::Start
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        printf("IAudioClient::Start failed: hr = 0x%08x\n", hr);
        AvRevertMmThreadCharacteristics(hTask);
        pAudioCaptureClient->Release();
        CloseHandle(hWakeUp);
        pAudioClient->Release();
        return hr;
    }
    SetEvent(hStartedEvent);
    
    std::vector<float> timeDomain;

    // loopback capture loop
    HANDLE waitArray[2] = { hStopEvent, hWakeUp };
    DWORD dwWaitResult;

    bool bDone = false;
    bool bFirstPacket = true;
    for (UINT32 nPasses = 0; !bDone; nPasses++) {
        dwWaitResult = WaitForMultipleObjects(
            ARRAYSIZE(waitArray), waitArray,
            FALSE, INFINITE
        );

        if (WAIT_OBJECT_0 == dwWaitResult) {
            printf("Received stop event after %u passes and %u frames\n", nPasses, *pnFrames);
            bDone = true;
            continue; // exits loop
        }

        if (WAIT_OBJECT_0 + 1 != dwWaitResult) {
            printf("Unexpected WaitForMultipleObjects return value %u on pass %u after %u frames\n", dwWaitResult, nPasses, *pnFrames);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();
            return E_UNEXPECTED;
        }

        // got a "wake up" event - see if there's data
        UINT32 nNextPacketSize;
        hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize);
        if (FAILED(hr)) {
            printf("IAudioCaptureClient::GetNextPacketSize failed on pass %u after %u frames: hr = 0x%08x\n", nPasses, *pnFrames, hr);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return hr;
        }

        if (0 == nNextPacketSize) {
            // no data yet
            continue;
        }

        // get the captured data
        BYTE *pData;
        UINT32 nNumFramesToRead;
        DWORD dwFlags;

        hr = pAudioCaptureClient->GetBuffer(
            &pData,
            &nNumFramesToRead,
            &dwFlags,
            NULL,
            NULL
        );
        if (FAILED(hr)) {
            printf("IAudioCaptureClient::GetBuffer failed on pass %u after %u frames: hr = 0x%08x\n", nPasses, *pnFrames, hr);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return hr;            
        }

        if (dwFlags && (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY != dwFlags)) {
            printf("IAudioCaptureClient::GetBuffer set flags to 0x%08x on pass %u after %u frames\n", dwFlags, nPasses, *pnFrames);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return E_UNEXPECTED;
        }

        if (0 == nNumFramesToRead) {
            printf("IAudioCaptureClient::GetBuffer said to read 0 frames on pass %u after %u frames\n", nPasses, *pnFrames);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return E_UNEXPECTED;            
        }

        {
            if(!timeDomain.capacity())
            {
                timeDomain.reserve(48000);
            }

            Assert(nBlockAlign % sizeof(float) == 0);

            const float* s = reinterpret_cast<float*>(pData);
            size_t samplesLeft = nNumFramesToRead;
            const size_t step = nBlockAlign / sizeof(float);
            
            while(samplesLeft)
            {
                size_t offset = timeDomain.size();
                size_t capacity = timeDomain.capacity() - offset;
                Assert(capacity);
                size_t packetSamples = min(samplesLeft, capacity);
                samplesLeft -= packetSamples;

                timeDomain.resize(offset + packetSamples);

                float* out = &timeDomain[offset];

                while(packetSamples)
                {
                    *out++ = *s;
                    s += step;
                    --packetSamples;
                }

                if(timeDomain.size() == timeDomain.capacity())
                {
                    callback(timeDomain);
                    timeDomain.resize(0);
                }
                else
                {
                    Assert(!samplesLeft);
                    break;
                }
            }
        }
        /*
        LONG lBytesToWrite = nNumFramesToRead * nBlockAlign;
        LONG lBytesWritten = mmioWrite(hFile, reinterpret_cast<PCHAR>(pData), lBytesToWrite);
        if (lBytesToWrite != lBytesWritten) {
            printf("mmioWrite wrote %u bytes on pass %u after %u frames: expected %u bytes\n", lBytesWritten, nPasses, *pnFrames, lBytesToWrite);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return E_UNEXPECTED;
        }
        */
        *pnFrames += nNumFramesToRead;
        
        hr = pAudioCaptureClient->ReleaseBuffer(nNumFramesToRead);
        if (FAILED(hr)) {
            printf("IAudioCaptureClient::ReleaseBuffer failed on pass %u after %u frames: hr = 0x%08x\n", nPasses, *pnFrames, hr);
            pAudioClient->Stop();
            CancelWaitableTimer(hWakeUp);
            AvRevertMmThreadCharacteristics(hTask);
            pAudioCaptureClient->Release();
            CloseHandle(hWakeUp);
            pAudioClient->Release();            
            return hr;            
        }
        
        bFirstPacket = false;
    } // capture loop
    
    pAudioClient->Stop();
    CancelWaitableTimer(hWakeUp);
    AvRevertMmThreadCharacteristics(hTask);
    pAudioCaptureClient->Release();
    CloseHandle(hWakeUp);
    pAudioClient->Release();

    return hr;
}

DWORD WINAPI LoopbackCaptureThreadFunction(LPVOID pContext) {
    LoopbackCaptureThreadFunctionArguments *pArgs =
        (LoopbackCaptureThreadFunctionArguments*)pContext;

    pArgs->hr = CoInitialize(NULL);
    if (FAILED(pArgs->hr)) {
        printf("CoInitialize failed: hr = 0x%08x\n", pArgs->hr);
        return 0;
    }

    pArgs->hr = LoopbackCapture(
        pArgs->hStartedEvent,
        pArgs->hStopEvent,
        &pArgs->nFrames,
        pArgs->callback
    );

    CoUninitialize();
    return 0;
}
