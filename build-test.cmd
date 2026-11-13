@echo off
rem Builds version.dll (Gitmo) with Visual Studio Build Tools 2022.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\testobj mkdir build\testobj
if not exist build\test mkdir build\test
ml64 /nologo /c /Fobuild\testobj\proxy.obj src\proxy.asm || exit /b 1

rc /nologo /fo build\testobj\gitmo.res res\gitmo.rc || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /LD /utf-8 /Ithird_party\imgui /Ithird_party\openxr /DGITMO_TEST /DIMGUI_USE_WCHAR32 /DIMGUI_DISABLE_DEMO_WINDOWS /DIMGUI_DISABLE_DEBUG_TOOLS ^
   src\gitmo.cpp src\overlay.cpp src\money.cpp src\ui.cpp src\input.cpp src\game.cpp src\watchdog.cpp src\vr.cpp src\transfer.cpp src\chat.cpp src\emoji.cpp src\photo.cpp src\radio.cpp src\net.cpp ^
   third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp ^
   third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp ^
   build\testobj\proxy.obj build\testobj\gitmo.res /Fobuild\testobj\ /Febuild\test\version.dll ^
   /link /DEF:src\version.def /IMPLIB:build\testobj\version_proxy.lib user32.lib kernel32.lib advapi32.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib dxguid.lib winmm.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib shlwapi.lib oleaut32.lib winhttp.lib ws2_32.lib mfplat.lib mfuuid.lib

