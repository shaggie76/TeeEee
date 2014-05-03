#include "TeeEeePch.h"
#include "Microphone.h"

#include "fftw3.h"

// #define HEADSET_MONITOR

#pragma comment(lib, "libfftw3f-3.lib")
#pragma comment(lib, "Winmm.lib")

const size_t PACKETS = 4;
const size_t SAMPLING_RATE = 48000;
const size_t PACKET_SIZE = 48000;

const size_t MIN_FREQUENCY = 350;
const size_t MAX_FREQUENCY = 1250;

static HWAVEIN sWaveInHandle = NULL;
static WAVEHDR sWaveInHeaders[PACKETS] = {0};
static __declspec(align(16)) short sWaveInPackets[PACKETS][PACKET_SIZE];
static volatile size_t sActiveWaveInPackets = PACKETS;

#ifdef HEADSET_MONITOR
static HWAVEOUT sWaveOutHandle = NULL;
static WAVEHDR sWaveOutHeaders[PACKETS] = {0};
static __declspec(align(16)) short sWaveOutPackets[PACKETS][PACKET_SIZE];
static volatile size_t sActiveWaveOutPackets = 0;
static size_t sNextWaveOutPacket = 0;
#endif HEADSET_MONITOR

struct PacketSummary
{
    float samples;
    float sumOfSquares;
};

static PacketSummary sPacketSummary[30] = {0};
static size_t sPacketSummaryIndex = 0;

static volatile bool sShutdown = false;

float DEFAULT_SENSITIVITY = -20.f;
float sPeakSensitivity = DEFAULT_SENSITIVITY;
const float MODERATE_BIAS = -11.f;

static HWND sWindowHandle = NULL;

typedef SIMDAllocator<float> SIMDAllocatorFloat;
typedef std::vector<float, SIMDAllocatorFloat> FloatArray;

struct FFTComplex
{
    float r, i;
};

typedef SIMDAllocator<FFTComplex> SIMDAllocatorFFTComplex;
typedef std::vector<FFTComplex, SIMDAllocatorFFTComplex> FFTComplexArray;

static bool sHaveLoopback = false;
static FloatArray sLoopbackFreqMag;

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

// TODO: weighting function instead?
static void BandPassFilter(FFTComplexArray& frequencyDomain)
{
    for(size_t freq = 0; freq < MIN_FREQUENCY; ++freq)
    {
        FFTComplex& f = frequencyDomain[freq];
        f.r = 0.f;
        f.i = 0.f;
    }

    for(size_t freq = MAX_FREQUENCY; freq < frequencyDomain.size(); ++freq)
    {
        FFTComplex& f = frequencyDomain[freq];
        f.r = 0.f;
        f.i = 0.f;
    }
}

static void ToFrequencyDomain(FFTComplexArray& frequencyDomain, FloatArray& timeDomain)
{
    StaticAssert(sizeof(FFTComplex) == sizeof(fftwf_complex));

    /*
    NOTE: should be this but you get heap corruption if you do it:
    frequencyDomain.resize((timeDomain.size() / 2) + 1);
    */

    frequencyDomain.resize(timeDomain.size());

    fftwf_plan plan = fftwf_plan_dft_r2c_1d
    (
        static_cast<int>(timeDomain.size()),
        &timeDomain.front(),
        reinterpret_cast<fftwf_complex*>(&frequencyDomain.front()),
        FFTW_ESTIMATE | FFTW_DESTROY_INPUT
    );

    fftwf_execute(plan);
    fftwf_destroy_plan(plan);
}

static void ToTimeDomain(FloatArray& timeDomain, const FFTComplexArray& frequencyDomain)
{
    fftwf_plan plan = fftwf_plan_dft_c2r_1d
    (
        static_cast<int>(timeDomain.size()),
        reinterpret_cast<fftwf_complex*>(const_cast<FFTComplex*>(&frequencyDomain.front())),
        &timeDomain.front(),
        FFTW_ESTIMATE
    );

    fftwf_execute(plan);
    fftwf_destroy_plan(plan);
        
    /* TODO: SSE */
    float scale = 1.f / static_cast<float>(PACKET_SIZE);
        
    for(size_t i = 0; i < PACKET_SIZE; ++i)
    {
        timeDomain[i] *= scale;
    }
}

/* TODO: SSE */
static float SumOfSquares(const FloatArray& timeDomain)
{
    float sumOfSquares = 0.0;
    for(FloatArray::const_iterator i = timeDomain.begin(), end = timeDomain.end(); i != end; ++i)
    {
        float s = *i;
        sumOfSquares += s * s;
    }
    return(sumOfSquares);
}

static void ProcessMicPacket(size_t packetIndex)
{
    const WAVEHDR& waveHeader = sWaveInHeaders[packetIndex];
    const short* dataIn = reinterpret_cast<const short*>(sWaveInPackets[packetIndex]);

    size_t samples = waveHeader.dwBytesRecorded / 2;
    
    FloatArray timeDomain(PACKET_SIZE);
    static const float scale = 1.f / 32768.f;
        
    for(size_t i = 0; i < samples; ++i)
    {
        /* TODO: SSE */
        timeDomain[i] = static_cast<float>(dataIn[i]) * scale;
    }
    
    // Usually not necessary but make sure it's zeroized if we got a partial packet:
    for(size_t i = samples; i < PACKET_SIZE; ++i)
    {
        timeDomain[i] = 0.f;
    }
    
    FFTComplexArray frequencyDomain;
    ToFrequencyDomain(frequencyDomain, timeDomain);

    BandPassFilter(frequencyDomain);

    ToTimeDomain(timeDomain, frequencyDomain);

#ifdef HEADSET_MONITOR
    if(sWaveOutHandle)
    {
        while(sActiveWaveOutPackets >= PACKETS)
        {
            Sleep(1);
        }

        short* dataOut = reinterpret_cast<short*>(sWaveOutPackets[sNextWaveOutPacket]);

        static const float invScale = 32768.f;
        
        /* TODO: SSE */

        for(size_t i = 0; i < samples; ++i)
        {
            float s = timeDomain[i] * invScale;

            if(s < SHRT_MIN)
            {
                s = SHRT_MIN;
            }

            if(s > SHRT_MAX)
            {
                s = SHRT_MAX;
            }
            
            dataOut[i] = static_cast<short>(s);
        }
        ++sActiveWaveOutPackets;
        waveOutWrite(sWaveOutHandle, &sWaveOutHeaders[sNextWaveOutPacket], sizeof(WAVEHDR));
        sNextWaveOutPacket = (sNextWaveOutPacket + 1) % ARRAY_COUNT(sWaveOutHeaders);
        return;
    }
#endif

    float sumOfSquares = SumOfSquares(timeDomain);

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

            Assert(waveHeader >= sWaveInHeaders);
            Assert(waveHeader < sWaveInHeaders + PACKETS);
            
            size_t packetIndex = static_cast<size_t>(waveHeader - sWaveInHeaders);

            if(sShutdown)
            {
                Assert(sActiveWaveInPackets);
                --sActiveWaveInPackets;
                return;
            }
            
            ProcessMicPacket(packetIndex);

            MMRESULT result = waveInAddBuffer(waveInHandle, waveHeader, sizeof(*waveHeader));
            Assert(result == MMSYSERR_NOERROR);
            break;
        }
    }
}

#ifdef HEADSET_MONITOR
static void CALLBACK WaveOutProc(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR, DWORD_PTR)
{
    switch(message)
    {
        case WOM_OPEN:
        {
            break;
        }

        case WOM_CLOSE:
        {
            break;
        }

        case WOM_DONE:
        {
            Assert(sActiveWaveOutPackets);
            --sActiveWaveOutPackets;
            break;
        }
    }
}
#endif // HEADSET_MONITOR

void TEMicrophone::Initialize(HWND windowHandle)
{
    sWindowHandle = windowHandle;
    sShutdown = false;
    sActiveWaveInPackets = PACKETS;

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

    for(size_t i = 0; i < ARRAY_COUNT(sWaveInHeaders); ++i)
    {
        sWaveInHeaders[i].lpData = reinterpret_cast<LPSTR>(sWaveInPackets[i]);
        sWaveInHeaders[i].dwBufferLength = static_cast<DWORD>(sizeof(sWaveInPackets[i]));
        result = waveInPrepareHeader(sWaveInHandle, &sWaveInHeaders[i], sizeof(sWaveInHeaders[i]));
        Assert(result == MMSYSERR_NOERROR);

        result = waveInAddBuffer(sWaveInHandle, &sWaveInHeaders[i], sizeof(sWaveInHeaders[i]));
        Assert(result == MMSYSERR_NOERROR);
    }
    
    result = waveInStart(sWaveInHandle);
    Assert(result == MMSYSERR_NOERROR);

#ifdef HEADSET_MONITOR
    UINT waveOutDevice = WAVE_MAPPER;

    for(UINT dev = 0, numDevs = waveOutGetNumDevs(); dev < numDevs; dev++)
    {
        WAVEOUTCAPS caps = {};
        if(waveOutGetDevCaps(dev, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        {
            continue;
        }
        
        if(_tcsstr(caps.szPname, TEXT("Headset")))
        {
            waveOutDevice = dev;
            break;
        }
    }

    if(waveOutDevice != WAVE_MAPPER)
    {
        result = waveOutOpen
        (
            &sWaveOutHandle,
            waveOutDevice,
            &waveFormat,
            reinterpret_cast<DWORD_PTR>(WaveOutProc),
            0,
            CALLBACK_FUNCTION
        );
    
        if(result != MMSYSERR_NOERROR)
        {
            OutputDebugString(TEXT("Could not open wave out\n"));
            return;
        }

        for(size_t i = 0; i < ARRAY_COUNT(sWaveOutHeaders); ++i)
        {
            sWaveOutHeaders[i].lpData = reinterpret_cast<LPSTR>(sWaveOutPackets[i]);
            sWaveOutHeaders[i].dwBufferLength = static_cast<DWORD>(sizeof(sWaveOutPackets[i]));
            result = waveOutPrepareHeader(sWaveOutHandle, &sWaveOutHeaders[i], sizeof(sWaveOutHeaders[i]));
            Assert(result == MMSYSERR_NOERROR);
        }
    }
#endif // HEADSET_MONITOR
}

void TEMicrophone::Shutdown()
{
    sWindowHandle = NULL;
    sShutdown = true;

    if(sWaveInHandle)
    {
        while(sActiveWaveInPackets)
        {
            Sleep(1);
        }

        for(size_t i = 0; i < ARRAY_COUNT(sWaveInHeaders); ++i)
        {
            MMRESULT result = waveInUnprepareHeader(sWaveInHandle, &sWaveInHeaders[i], sizeof(sWaveInHeaders[i]));
            Assert(result == MMSYSERR_NOERROR);
        }
    
        MMRESULT result;
    
        result = waveInStop(sWaveInHandle);
        Assert(result == MMSYSERR_NOERROR);

        result = waveInClose(sWaveInHandle);
        Assert(result == MMSYSERR_NOERROR);
        sWaveInHandle = NULL;
    }

#ifdef HEADSET_MONITOR
    if(sWaveOutHandle)
    {
        for(size_t i = 0; i < ARRAY_COUNT(sWaveOutHeaders); ++i)
        {
            while(waveOutUnprepareHeader(sWaveOutHandle, &sWaveOutHeaders[i], sizeof(sWaveOutHeaders[i])) == WAVERR_STILLPLAYING)
            {
                Sleep(1);
            }
        }

        MMRESULT result;

        result = waveOutClose(sWaveOutHandle);
        Assert(result == MMSYSERR_NOERROR);
        sWaveOutHandle = NULL;
    }
#endif // HEADSET_MONITOR
}

const float MIN_SENSITIVITY = -8.f;
const float MAX_SENSITIVITY = -38.f;

float TEMicrophone::GetSensitivity()
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

void TEMicrophone::SetSensitivity(float s)
{
    sPeakSensitivity = Lerp(MIN_SENSITIVITY, MAX_SENSITIVITY, s);

    TCHAR buffer[256];
    _sntprintf(buffer, ARRAY_COUNT(buffer), TEXT("Sensitivity: %g\n"), sPeakSensitivity);
    OutputDebugString(buffer);
}
