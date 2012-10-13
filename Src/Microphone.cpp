#include "TeeEeePch.h"
#include "Microphone.h"

#include "fftw3.h"

static HWAVEIN sWaveInHandle = NULL;

const size_t PACKETS = 4;
const size_t SAMPLING_RATE = 44100; // Roughly 1 second
const size_t PACKET_SIZE = 44100;
 
const size_t MIN_FREQUENCY = 350;
const size_t MAX_FREQUENCY = 1250;

static WAVEHDR sWaveHeaders[PACKETS] = {0};
static __declspec(align(16)) char sWavePackets[PACKETS][PACKET_SIZE];

struct PacketSummary
{
    float samples;
    float sumOfSquares;
};

static PacketSummary sPacketSummary[30] = {0};
static size_t sPacketSummaryIndex = 0;

static volatile bool sShutdown = false;
static volatile size_t sActivePackets = PACKETS;

float DEFAULT_SENSITIVITY = -20.f;
float sPeakSensitivity = DEFAULT_SENSITIVITY;
const float MODERATE_BIAS = -11.f;

static HWND sWindowHandle = NULL;

static inline float VolumeToDecibels(float volume)
{
    return(20.f * log10f(volume));
}

static void ClearHistory()
{
    for(size_t i = 0; i < ARRAY_COUNT(sPacketSummary); ++i)
    {
        sPacketSummary[i].sumOfSquares = 0.f;
    }
}

static void ProcessPacket(size_t packetIndex)
{
    const WAVEHDR& waveHeader = sWaveHeaders[packetIndex];

    const short* data = reinterpret_cast<const short*>(waveHeader.lpData);

    size_t samples = waveHeader.dwBytesRecorded / 2;
    float sumOfSquares = 0.f;
    
    static float timeDomain[PACKET_SIZE];
    static const float scale = 1.f / 32768.f;
        
    for(size_t i = 0; i < samples; ++i)
    {
        timeDomain[i] = static_cast<float>(data[i]) * scale;
    }
    
    for(size_t i = samples; i < PACKET_SIZE; ++i)
    {
        timeDomain[i] = 0.f;
    }
    
    static fftwf_complex frequencyDomain[PACKET_SIZE / 2];
    const size_t frequencies = (PACKET_SIZE / 2) + 1;

    // Time -> Frequency
    {
        fftwf_plan plan = fftwf_plan_dft_r2c_1d(static_cast<int>(PACKET_SIZE), timeDomain, frequencyDomain, FFTW_ESTIMATE | FFTW_DESTROY_INPUT);

        fftwf_execute(plan);
        fftwf_destroy_plan(plan);
    }
    
    for(size_t freq = 0; freq < MIN_FREQUENCY; ++freq)
    {
        frequencyDomain[freq][0] = 0.f;
        frequencyDomain[freq][1] = 0.f;
    }

    // TODO: weighting function?

    for(size_t freq = MAX_FREQUENCY; freq < frequencies; ++freq)
    {
        frequencyDomain[freq][0] = 0.f;
        frequencyDomain[freq][1] = 0.f;
    }
    
    // Frequency -> Time
    {
        fftwf_plan plan = fftwf_plan_dft_c2r_1d(static_cast<int>(PACKET_SIZE), frequencyDomain, timeDomain, FFTW_ESTIMATE | FFTW_DESTROY_INPUT);

        fftwf_execute(plan);
        fftwf_destroy_plan(plan);
        
        float scale = 1.f / static_cast<float>(PACKET_SIZE);
        
        for(size_t i = 0; i < PACKET_SIZE; ++i)
        {
            timeDomain[i] *= scale;
            sumOfSquares += timeDomain[i] * timeDomain[i];
        }
    }

    PacketSummary& summary = sPacketSummary[sPacketSummaryIndex];
    sPacketSummaryIndex = (sPacketSummaryIndex + 1) % ARRAY_COUNT(sPacketSummary);
    
    summary.sumOfSquares = sumOfSquares;
    summary.samples = static_cast<float>(samples);
    
    float rmsVolume = sqrtf(summary.sumOfSquares / summary.samples);
    float noiseDb = VolumeToDecibels(rmsVolume);
    
    float totalSumOfSquares = 0.f;
    float totalSamples = 0.f;
    
    for(size_t i = 0; i < ARRAY_COUNT(sPacketSummary); ++i)
    {
        totalSumOfSquares += sPacketSummary[i].sumOfSquares;
        totalSamples += sPacketSummary[i].samples;
    }
    
    if(totalSamples < (PACKET_SIZE * 3))
    {
        return;
    }

    float longTermRmsVolume = sqrtf(totalSumOfSquares / totalSamples);
    float longTermNoiseDb = VolumeToDecibels(longTermRmsVolume);

#if 0
    TCHAR buffer[256];
    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("ST: %g LT: %g\n"), noiseDb, longTermNoiseDb);
    OutputDebugString(buffer);
#endif

    if(noiseDb >= sPeakSensitivity)
    {
        ClearHistory();
        PostMessage(sWindowHandle, WM_KEYDOWN, 'S', 0);
        return;
    }

    if(longTermNoiseDb >= (sPeakSensitivity + MODERATE_BIAS))
    {
        ClearHistory();
        PostMessage(sWindowHandle, WM_KEYDOWN, 'S', 0);
        return;
    }
}

static void CALLBACK WaveInProc(HWAVEIN waveInHandle, UINT message, DWORD_PTR, DWORD_PTR dwParam1, DWORD_PTR)
{
    switch(message)
    {
        case WIM_OPEN:
        {
            break;
        }

        case WIM_CLOSE:
        {
            break;
        }

        case WIM_DATA:
        {
            WAVEHDR* waveHeader = reinterpret_cast<WAVEHDR*>(dwParam1); 
            Assert(waveHeader);

            Assert(waveHeader >= sWaveHeaders);
            Assert(waveHeader < sWaveHeaders + PACKETS);
            
            size_t packetIndex = static_cast<size_t>(waveHeader - sWaveHeaders);

            if(sShutdown)
            {
                Assert(sActivePackets);
                --sActivePackets;
                return;
            }
            
            ProcessPacket(packetIndex);

            MMRESULT result = waveInAddBuffer(waveInHandle, waveHeader, sizeof(*waveHeader));
            Assert(result == MMSYSERR_NOERROR);
            break;
        }
    }
}

void Microphone::Initialize(HWND windowHandle)
{
    sWindowHandle = windowHandle;
    sShutdown = false;
    sActivePackets = PACKETS;

    WAVEFORMATEX waveFormat = {0};
    
    waveFormat.wFormatTag = WAVE_FORMAT_PCM; 
    waveFormat.nChannels = 1;
    waveFormat.nSamplesPerSec = SAMPLING_RATE;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = waveFormat.wBitsPerSample / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign; 
    waveFormat.cbSize = sizeof(waveFormat);

    MMRESULT result;

    result = waveInOpen
    (
        &sWaveInHandle,
        WAVE_MAPPER,
        &waveFormat,
        reinterpret_cast<DWORD_PTR>(WaveInProc),
        0,
        CALLBACK_FUNCTION
    );
    
    if(result != MMSYSERR_NOERROR)
    {
        OutputDebugString(TEXT("Could not open microphone\n"));
        return;
    }

    for(size_t i = 0; i < ARRAY_COUNT(sWaveHeaders); ++i)
    {
        sWaveHeaders[i].lpData = sWavePackets[i];
        sWaveHeaders[i].dwBufferLength = static_cast<DWORD>(PACKET_SIZE);
        result = waveInPrepareHeader(sWaveInHandle, &sWaveHeaders[i], sizeof(sWaveHeaders[i]));
        Assert(result == MMSYSERR_NOERROR);

        result = waveInAddBuffer(sWaveInHandle, &sWaveHeaders[i], sizeof(sWaveHeaders[i]));
        Assert(result == MMSYSERR_NOERROR);
    }
    
    result = waveInStart(sWaveInHandle);
    Assert(result == MMSYSERR_NOERROR);
}

void Microphone::Shutdown()
{
    sWindowHandle = NULL;

    if(!sWaveInHandle)
    {
        return;
    }
    
    sShutdown = true;

    while(sActivePackets)
    {
        Sleep(1);
    }

    for(size_t i = 0; i < ARRAY_COUNT(sWaveHeaders); ++i)
    {
        MMRESULT result = waveInUnprepareHeader(sWaveInHandle, &sWaveHeaders[i], sizeof(sWaveHeaders[i]));
        Assert(result == MMSYSERR_NOERROR);
    }
    
    MMRESULT result;
    
    result = waveInStop(sWaveInHandle);
    Assert(result == MMSYSERR_NOERROR);

    result = waveInClose(sWaveInHandle);
    Assert(result == MMSYSERR_NOERROR);
    sWaveInHandle = NULL;
}

const float MIN_SENSITIVITY = -34.f;
const float MAX_SENSITIVITY = -40.f;

float Microphone::GetSensitivity()
{
    float range = MAX_SENSITIVITY - MIN_SENSITIVITY;
    float s = (sPeakSensitivity - MIN_SENSITIVITY) / range;
    return(s);
}

template<class T, class S>
inline T Lerp(const T& a, const T& b, const S& r)
{
    return(a + r * (b - a));
}

void Microphone::SetSensitivity(float s)
{
    sPeakSensitivity = Lerp(MIN_SENSITIVITY, MAX_SENSITIVITY, s);

    TCHAR buffer[256];
    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("Sensitivity: %g\n"), sPeakSensitivity);
    OutputDebugString(buffer);
}
