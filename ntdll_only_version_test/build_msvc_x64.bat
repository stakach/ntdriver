@echo off
setlocal enabledelayedexpansion

rem Build from a "x64 Native Tools Command Prompt for VS".
rem This creates a tiny import library from ntdll_min.def, then links an EXE
rem with /NODEFAULTLIB and a custom entry point so the only DLL import is ntdll.dll.

set OUTDIR=%~dp0build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

lib /nologo /def:"%~dp0ntdll_min.def" /machine:x64 /out:"%OUTDIR%\ntdll_min.lib"
if errorlevel 1 exit /b 1

cl /nologo /TC /W4 /WX /GS- /GR- /O1 /Zl /c "%~dp0src\ntdll_only_version_test.c" /Fo"%OUTDIR%\ntdll_only_version_test.obj"
if errorlevel 1 exit /b 1

link /nologo /machine:x64 /subsystem:console /entry:NtProcessStartup /nodefaultlib /dynamicbase /nxcompat ^
  /out:"%OUTDIR%\ntdll_only_version_test.exe" ^
  "%OUTDIR%\ntdll_only_version_test.obj" "%OUTDIR%\ntdll_min.lib"
if errorlevel 1 exit /b 1

echo.
echo Built: %OUTDIR%\ntdll_only_version_test.exe
echo.
echo Expected import table: ntdll.dll only, with RtlGetVersion and NtTerminateProcess.
where dumpbin >nul 2>nul
if not errorlevel 1 (
  dumpbin /nologo /imports "%OUTDIR%\ntdll_only_version_test.exe"
) else (
  echo dumpbin not found; use Dependencies, dumpbin, llvm-readobj, or objdump to verify imports.
)
