@echo off
setlocal
mkdir build 2>nul
cl /nologo /W4 /DUNICODE /D_UNICODE /EHsc user\UserTest.c /link /out:build\UmdfLifecycleUserTest.exe setupapi.lib
endlocal
