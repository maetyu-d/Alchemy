@echo off
setlocal
cd /d "%~dp0\.."
if not exist build-logs mkdir build-logs
echo Starting vcvars64 > build-logs\configure-nmake.log
call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >> build-logs\configure-nmake-vcvars.log 2>&1
if errorlevel 1 exit /b %errorlevel%
echo Starting cmake >> build-logs\configure-nmake.log
"C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S . ^
  -B build-win-nmake ^
  -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DJUCE_PATH="C:\Users\matda\Desktop\juce-8.0.12-windows\JUCE" >> build-logs\configure-nmake-cmake.log 2>&1
echo CMake exited with %errorlevel% >> build-logs\configure-nmake.log
exit /b %errorlevel%
