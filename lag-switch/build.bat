@echo off
setlocal
pushd "%~dp0"

REM ===========================================================================
REM  Build lagswitch.exe from source.
REM
REM  You do NOT need this to USE the tool - a prebuilt lagswitch.exe already
REM  ships in this folder. This is only for compiling it yourself.
REM
REM  Tries MSVC (cl) first, then MinGW (g++). Always pauses at the end.
REM ===========================================================================

echo === lagswitch build ===
echo.

where cl >nul 2>nul && goto :msvc
where g++ >nul 2>nul && goto :mingw
goto :nocompiler

:msvc
echo [*] Found MSVC. Building...
cl /nologo /EHsc /O2 /W3 /utf-8 lagswitch.cpp /Fe:lagswitch.exe /link ole32.lib oleaut32.lib
if errorlevel 1 goto :fail
where mt >nul 2>nul && mt -nologo -manifest app.manifest -outputresource:lagswitch.exe;1
goto :ok

:mingw
echo [*] Found MinGW g++. Building ^(static^)...
echo 1 24 "app.manifest" > manifest.rc
windres manifest.rc -O coff -o manifest.res
g++ -std=c++17 -O2 -static lagswitch.cpp manifest.res -o lagswitch.exe -lole32 -loleaut32 -luuid
if errorlevel 1 goto :fail
del manifest.rc manifest.res >nul 2>nul
goto :ok

:nocompiler
echo No C++ compiler found - and you probably don't need one!
echo.
echo Just use the prebuilt binary in this folder:
echo     lagswitch.exe   ^(right-click -^> Run as administrator^)
echo.
echo If you really want to build from source, install ONE of:
echo   - Visual Studio Build Tools ^(gives you "cl"^), or
echo   - MSYS2 + mingw-w64-ucrt-x86_64-gcc ^(gives you "g++"^)
echo.
pause
exit /b 1

:ok
echo.
echo [*] Built lagswitch.exe
echo.
pause
exit /b 0

:fail
echo.
echo [!] Build failed.
echo.
pause
exit /b 1
