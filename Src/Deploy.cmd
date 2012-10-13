@echo off

FOR %%F IN (FFTW\x86\libfftw3f-3.dll,VLC\libvlc.dll,VLC\libvlccore.dll) DO (
    IF NOT EXIST "%1%%~nF%%~xF" (
        MKLINK /H "%1%%~nF%%~xF" "%2%%F"
    )
)

FOR %%F IN (VLC\plugins) DO (
    IF NOT EXIST "%1%%~nF%%~xF" (
        MKLINK /D "%1%%~nF%%~xF" "%2%%F"
    )
)
