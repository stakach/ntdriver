# AsyncTest WDM Driver

Tiny legacy WDM `.sys` driver for `userspace-ntos` async dispatcher testing.

It is the follow-up to `SurtTest.sys`.

## What it exercises

Baseline:

- `DriverEntry`
- `DriverUnload`
- `IoCreateDevice`
- `IoCreateSymbolicLink`
- `IoDeleteSymbolicLink`
- `IoDeleteDevice`
- `IRP_MJ_CREATE`
- `IRP_MJ_CLOSE`
- `IRP_MJ_DEVICE_CONTROL`
- `METHOD_BUFFERED`
- `IoCompleteRequest`

Async/runtime primitives:

- `IoMarkIrpPending`
- dispatch returns `STATUS_PENDING`
- `KeInitializeDpc`
- `KeInsertQueueDpc`
- `KeInitializeTimer`
- `KeSetTimer`
- timer DPC callback
- `IoAllocateWorkItem`
- `IoQueueWorkItem`
- `IoFreeWorkItem`
- `ExAllocatePoolWithTag`
- `ExFreePoolWithTag`
- `KeInitializeEvent`
- `KeSetEvent`
- `KeClearEvent`
- `KeWaitForSingleObject`

## Created objects

```text
\Device\AsyncTest
\DosDevices\AsyncTest -> \Device\AsyncTest
```

For `userspace-ntos`, map `\DosDevices` to whatever you currently use for the DOS-device namespace, commonly `\??`.

## IOCTLs

```c
IOCTL_ASYNC_PING
```

Synchronously returns `ULONG 0x434E5953`, ASCII-ish `"SYNC"`.

```c
IOCTL_ASYNC_GET_VERSION
```

Synchronously returns:

```c
typedef struct _ASYNC_TEST_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Patch;
    ULONG Build;
} ASYNC_TEST_VERSION;
```

Expected value:

```text
0.1.0 build 9
```

```c
IOCTL_ASYNC_COMPLETE_VIA_DPC
```

Returns `STATUS_PENDING` from dispatch, queues a `KDPC`, and later completes with:

```text
IoStatus.Status      = STATUS_SUCCESS
IoStatus.Information = 0x44504321
```

```c
IOCTL_ASYNC_COMPLETE_VIA_TIMER
```

Returns `STATUS_PENDING` from dispatch, starts a `KTIMER` with a DPC, and later completes with:

```text
IoStatus.Status      = STATUS_SUCCESS
IoStatus.Information = 0x544D5221
```

```c
IOCTL_ASYNC_COMPLETE_VIA_WORKITEM
```

Returns `STATUS_PENDING` from dispatch, queues an `IO_WORKITEM`, and later completes with:

```text
IoStatus.Status      = STATUS_SUCCESS
IoStatus.Information = 0x574B4921
```

## Build on Windows with WDK

Recommended:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add:
   - `driver/Driver.c`
   - `driver/AsyncTest.h`
4. Build x64 Release.
5. The output will be `AsyncTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Load on Windows for sanity testing

From an elevated Developer Command Prompt:

```bat
sc create AsyncTest type= kernel binPath= C:\full\path\to\AsyncTest.sys
sc start AsyncTest
```

Run optional user-mode test:

```bat
cl /W4 /Fe:AsyncUserTest.exe user\UserTest.c
AsyncUserTest.exe
```

Unload:

```bat
sc stop AsyncTest
sc delete AsyncTest
```

## Driver Host milestone expectation

For `userspace-ntos`, a successful async milestone is:

```text
load AsyncTest.sys
call DriverEntry
resolve imports
create \Device\AsyncTest
create \DosDevices\AsyncTest symlink

open device through I/O Manager
send IRP_MJ_CREATE

send IOCTL_ASYNC_COMPLETE_VIA_DPC
driver marks pending
dispatch returns STATUS_PENDING
runtime later invokes DPC callback
driver calls IoCompleteRequest
I/O Manager observes completion

repeat for:
  IOCTL_ASYNC_COMPLETE_VIA_TIMER
  IOCTL_ASYNC_COMPLETE_VIA_WORKITEM

send IRP_MJ_CLOSE
call DriverUnload
wait for outstanding async operations
delete symlink and device
```

## Expected imports

Depending on compiler/WDK optimisation, expect imports close to:

```text
ntoskrnl.exe:
  ExAllocatePoolWithTag
  ExFreePoolWithTag
  IoAllocateWorkItem
  IoCreateDevice
  IoCreateSymbolicLink
  IoDeleteDevice
  IoDeleteSymbolicLink
  IoFreeWorkItem
  IofCompleteRequest
  IoQueueWorkItem
  KeClearEvent
  KeInitializeDpc
  KeInitializeEvent
  KeInitializeTimer
  KeInsertQueueDpc
  KeSetEvent
  KeSetTimer
  KeWaitForSingleObject
  RtlInitUnicodeString
  RtlZeroMemory / memset-like import depending on compiler
```

`IoGetCurrentIrpStackLocation` and `IoMarkIrpPending` are usually macros, so they may not import as functions. They directly touch fields inside `IRP`, which is intentional for your projected IRP layout.
