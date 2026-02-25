@echo off
setlocal

set PROJ=%~dp0
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set VSDEV="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

echo [1/3] Configuring...
call %VSDEV% -arch=amd64 >nul 2>&1
%CMAKE% -S "%PROJ%" -B "%PROJ%build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :err

echo [2/3] Building (Release)...
%CMAKE% --build "%PROJ%build" --config Release --parallel
if errorlevel 1 goto :err

echo [3/3] Running tests...
"%PROJ%build\Release\lob_tests.exe"
if errorlevel 1 goto :err

echo.
echo === All done! ===
echo Run benchmark: %PROJ%build\Release\lob_bench.exe
echo Run replay:    %PROJ%build\Release\lob_main.exe data\sample_orders.csv output\trades.csv output\tob.csv
goto :eof

:err
echo Build or test failed.
exit /b 1
