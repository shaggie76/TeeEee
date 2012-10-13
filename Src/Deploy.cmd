@echo off

FOR %%F IN (FFTW\x86\libfftw3f-3.dll,VLC\libvlc.dll,VLC\libvlccore.dll) DO (
    IF NOT EXIST "%1%%~nF%%~xF" (
        REM MKLINK /H "%1%%~nF%%~xF" "%2%%F"
        COPY "%2%%F" "%1%%~nF%%~xF" 
    )
)

IF NOT EXIST "%1\plugins" (
    XCOPY /S /D /I "%2\VLC\plugins" "%1\plugins"
)
