# SurtTest WDM Driver

Tiny legacy WDM `.sys` driver for `userspace-ntos` Driver Host testing.

## What it exercises

- `DriverEntry`
- `DRIVER_OBJECT.MajorFunction[]`
- `DriverUnload`
- `IoCreateDevice`
- `IoCreateSymbolicLink`
- `IoDeleteSymbolicLink`
- `IoDeleteDevice`
- `RtlInitUnicodeString`
- `IRP_MJ_CREATE`
- `IRP_MJ_CLOSE`
- `IRP_MJ_DEVICE_CONTROL`
- `IoGetCurrentIrpStackLocation`
- `IoCompleteRequest` / normally imported as `IofCompleteRequest`
- `METHOD_BUFFERED` via `Irp->AssociatedIrp.SystemBuffer`

## Created objects

```text
\Device\SurtTest
\DosDevices\SurtTest -> \Device\SurtTest
```

On real Windows, user-mode opens:

```text
\\.\SurtTest
```

For `userspace-ntos`, either support `\DosDevices` or alias/map it to `\??`.

## IOCTLs

- `IOCTL_SURT_PING`: returns `ULONG` value `0x53555254`, ASCII `SURT`.
- `IOCTL_SURT_GET_VERSION`: returns `SURT_TEST_VERSION { 0, 1, 0, 9 }`.
- `IOCTL_SURT_ECHO`: `METHOD_BUFFERED` echo; completion information is `min(input_len, output_len)`.

## Build on Windows with WDK

Simplest path:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add `driver/Driver.c` and `driver/SurtTest.h`.
4. Build x64 Release.
5. The output will be `SurtTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Load on Windows for sanity testing

Elevated Developer Command Prompt:

```bat
sc create SurtTest type= kernel binPath= C:\full\path\to\SurtTest.sys
sc start SurtTest
```

Optional user-mode test:

```bat
cl /W4 /I driver /Fe:SurtUserTest.exe user\UserTest.c
SurtUserTest.exe
```

Unload:

```bat
sc stop SurtTest
sc delete SurtTest
```

## Driver Host milestone expectation

```text
load SurtTest.sys
call DriverEntry
resolve minimal imports
create \Device\SurtTest
create \DosDevices\SurtTest symlink
open device through I/O Manager
send IRP_MJ_CREATE
send IRP_MJ_DEVICE_CONTROL / IOCTL_SURT_PING
receive IoStatus.Status = STATUS_SUCCESS
receive IoStatus.Information = 4
send IRP_MJ_CLOSE
call DriverUnload
delete symlink and device
```

## Expected imports

Depending on compiler/WDK optimisation, expect something close to:

```text
ntoskrnl.exe:
  IoCreateDevice
  IoCreateSymbolicLink
  IoDeleteDevice
  IoDeleteSymbolicLink
  IofCompleteRequest
  RtlInitUnicodeString
```

`IoGetCurrentIrpStackLocation` is normally a macro, so it likely will not appear as an import. It directly accesses fields inside `IRP`, which is useful for testing projected IRP layout.
