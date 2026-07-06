@echo off
setlocal
cd /d %~dp0\..
if not exist build mkdir build
msbuild Umdf2LifecycleTest.vcxproj /p:Configuration=Release /p:Platform=x64
endlocal
