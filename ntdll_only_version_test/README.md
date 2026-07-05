# ntdll-only version test

This is a tiny Windows x64 PE executable intended to test end-to-end loading of a Windows application when only `ntdll.dll` is imported.

It has:

- no CRT startup
- no `kernel32.dll`
- no `kernelbase.dll`
- no `advapi32.dll`
- no custom user-mode runtime
- a custom PE entry point: `NtProcessStartup`
- imports from `ntdll.dll` only:
  - `RtlGetVersion`
  - `NtTerminateProcess`

The program calls `RtlGetVersion` and exits via `NtTerminateProcess`.

On success, the exit code is encoded as:

```text
(major & 0xff) << 24 | (minor & 0xff) << 16 | (build & 0xffff)
```

Examples:

```text
Windows 10.0.22631 -> 0x0A005867
Windows 11.0.26100 -> 0x0B0065F4
```

If `RtlGetVersion` fails, the program exits with:

```text
0xE0000000 | (status & 0x0FFFFFFF)
```

## Build with MSVC

Open an **x64 Native Tools Command Prompt for VS** and run:

```bat
build_msvc_x64.bat
```

Output:

```text
build\ntdll_only_version_test.exe
```

The build script creates a tiny import library from `ntdll_min.def`, so it does not require the WDK `ntdll.lib`.

## Optional clang-cl build

From an environment with `clang-cl`, `lld-link`, and `lib.exe`:

```bat
build_clang_cl_x64.bat
```

## Verify imports

Using Visual Studio tools:

```bat
dumpbin /imports build\ntdll_only_version_test.exe
```

Expected imported DLL:

```text
ntdll.dll
```

Expected imported functions:

```text
RtlGetVersion
NtTerminateProcess
```

No `kernel32.dll`, `kernelbase.dll`, or CRT DLL should appear.

## Run on Windows

```bat
build\ntdll_only_version_test.exe
echo %ERRORLEVEL%
```

For hex display in PowerShell:

```powershell
$p = Start-Process .\build\ntdll_only_version_test.exe -PassThru -Wait
'0x{0:X8}' -f $p.ExitCode
```

## Why this shape?

This tests the minimal official-Windows-userland path:

```text
PE loader
  -> import ntdll.dll
  -> resolve ntdll imports
  -> jump to executable entry point
  -> executable calls ntdll export
  -> process exits through ntdll/native path
```

It avoids `ExitProcess` because that lives in `kernel32/kernelbase`, not `ntdll`.
