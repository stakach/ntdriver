@echo off
setlocal
REM Run from a "x64 Native Tools Command Prompt for VS" with WDK installed.
msbuild KmdfLoaderCompatTest.vcxproj /p:Configuration=Release /p:Platform=x64
endlocal
