@echo off
setlocal
cd /d %~dp0\..\user
if not exist ..\build mkdir ..\build
cl /nologo /W4 /EHsc /DUNICODE /D_UNICODE UserTest.c /Fe:..\build\umdf2_user_test.exe
endlocal
