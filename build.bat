@echo off
rem Needs: python -m pip install ziglang
rem   build.bat        -> d3d9.dll without logging
rem   build.bat -log   -> d3d9.dll that writes d3d9proxy.log next to Fable.exe
cd /d "%~dp0"
set FLAGS=
set KIND=no logging
if /i "%~1"=="-log" (
    set FLAGS=-DPROXY_LOG
    set KIND=with logging to d3d9proxy.log
)
python -m ziglang cc -target x86-windows-gnu -O2 -shared %FLAGS% -Wno-dll-attribute-on-redeclaration -o d3d9.dll d3d9.c d3d9.def -luser32 -lkernel32
if errorlevel 1 (
    echo Build FAILED.
    exit /b 1
)
echo Built d3d9.dll (%KIND%).
