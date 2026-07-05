@echo off
setlocal enabledelayedexpansion

rem Optional build path if clang-cl is available in a VS/LLVM environment.
rem Still uses lib.exe to generate the ntdll import library from the .def file.

set OUTDIR=%~dp0build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

lib /nologo /def:"%~dp0ntdll_min.def" /machine:x64 /out:"%OUTDIR%\ntdll_min.lib"
if errorlevel 1 exit /b 1

clang-cl /nologo /TC /W4 /WX /GS- /O1 /Zl /c "%~dp0src\ntdll_only_version_test.c" /Fo"%OUTDIR%\ntdll_only_version_test.obj"
if errorlevel 1 exit /b 1

lld-link /nologo /machine:x64 /subsystem:console /entry:NtProcessStartup /nodefaultlib /dynamicbase /nxcompat ^
  /out:"%OUTDIR%\ntdll_only_version_test.exe" ^
  "%OUTDIR%\ntdll_only_version_test.obj" "%OUTDIR%\ntdll_min.lib"
if errorlevel 1 exit /b 1

echo Built: %OUTDIR%\ntdll_only_version_test.exe
