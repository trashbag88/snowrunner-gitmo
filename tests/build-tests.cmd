@echo off
rem Builds and runs Gitmo's protocol tests (Visual Studio Build Tools 2022):
rem   txtest   - money transfers: scenarios + thousands of random lossy rounds (money is conserved)
rem   chattest - SMS: ordering, delivery/read receipts, photos in pieces over a lossy link
rem The other .cpp files here are development tools (the UI harness and hook-chaining tests).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build\tests mkdir ..\build\tests
cl /nologo /std:c++17 /O2 /EHsc /W3 /I..\src txtest.cpp ..\src\transfer.cpp /Fo..\build\tests\ /Fe..\build\tests\txtest.exe || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 /I..\src chattest.cpp ..\src\chat.cpp /Fo..\build\tests\ /Fe..\build\tests\chattest.exe || exit /b 1
..\build\tests\txtest.exe || exit /b 1
..\build\tests\chattest.exe || exit /b 1
