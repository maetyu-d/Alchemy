@echo off
setlocal
cd /d "%~dp0\.."
if not exist build-logs mkdir build-logs
echo Starting vcvars64 > build-logs\build-nmake.log
call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >> build-logs\build-nmake-vcvars.log 2>&1
if errorlevel 1 exit /b %errorlevel%
echo Starting build >> build-logs\build-nmake.log
"C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  --build build-win-nmake ^
  --config Release >> build-logs\build-nmake-cmake.log 2>&1
echo Build exited with %errorlevel% >> build-logs\build-nmake.log
exit /b %errorlevel%
