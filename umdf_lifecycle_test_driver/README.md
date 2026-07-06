# UMDF Lifecycle Test Driver

A **UMDF v2 (UMDF 2.15)** test driver project for exercising a UMDF
reflector/host lifecycle. It is intentionally small and focused on lifecycle
validation rather than hardware. It builds to a user-mode driver DLL hosted by
`WUDFHost.exe` through the `WUDFRd.sys` reflector.

> Ported from the original UMDF **v1** (COM) project. UMDF v1 is deprecated and
> cannot be built with the modern WDK (its headers/libraries were removed after
> WDK 8.1), so this project now uses the UMDF v2 WDF object model — the same
> model as the KMDF drivers in this repo. There is no "UMDF 1.15"; 2.15 is the
> corresponding current-generation framework version.

## What it tests

- INF reflector path via `WUDFRd.sys`
- UMDF v2 WDF binding (`UmdfLibraryVersion = 2.15.0`)
- `DriverEntry` / `EvtDriverUnload` (was `IDriverEntry::OnInitialize` / `OnDeinitialize`)
- `EvtDriverDeviceAdd` (was `IDriverEntry::OnDeviceAdd`)
- `WdfDeviceCreate`
- `WdfDeviceCreateDeviceInterface`
- `WdfIoQueueCreate`
- `EvtDevicePrepareHardware` / `EvtDeviceReleaseHardware`
- `EvtDeviceD0Entry` / `EvtDeviceD0Exit`
- `EvtIoDeviceControl`
- `EvtCleanupCallback` (was `IObjectCleanup::OnCleanup`)

## Device interface

`{7D9D6D35-0E63-4F7F-BF54-89A4D64483D4}`

## IOCTLs

- `IOCTL_UMDFLT_PING`
- `IOCTL_UMDFLT_GET_INFO`
- `IOCTL_UMDFLT_FAIL_REQUEST`

`IOCTL_UMDFLT_GET_INFO` returns callback counters (`UMDFLT_INFO`) so your
reflector/host/runtime can verify lifecycle progress. The struct layout is
preserved from the v1 project.

## Build

Built in CI by `.github/workflows/build-driver.yml` (matrix entry
`UmdfLifecycleTest`) with the WDK + MSBuild, producing
`UmdfLifecycleTest.dll` / `.pdb` / `.inf` / `.cat`.

Locally, from a Visual Studio x64 developer prompt with the WDK installed:

```bat
msbuild UmdfLifecycleTest.vcxproj /p:Configuration=Release /p:Platform=x64
```

Build the user test from a Visual Studio developer prompt:

```bat
scripts\build_user_test_msvc.bat
```

## Notes for userspace-ntos

This validates the `WUDFRd.sys` reflector and UMDF **v2** host path. The
included `driver-bringup-fixture.json` describes the expected synthetic root
devnode and lifecycle.
