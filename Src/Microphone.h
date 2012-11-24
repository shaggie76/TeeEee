#ifndef MICROPHONE_H
#define MICROPHONE_H

namespace TEMicrophone
{
    extern void Initialize(HWND windowHandle);
    extern void Shutdown();

    // 0.f -> 1.f, 1.f being max
    extern float GetSensitivity();
    extern void SetSensitivity(float s);
}

#endif // MICROPHONE_H
