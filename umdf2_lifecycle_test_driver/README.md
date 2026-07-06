# UMDF2 Lifecycle Test Driver

This is a minimal **UMDF v2** WDF-style user-mode driver project for testing userspace-ntos driver lifecycle support.

It is intentionally **not UMDF v1/COM**. It uses the WDF C API style:

```text
DriverEntry
  -> WdfDriverCreate
  -> EvtDriverDeviceAdd
  -> WdfDeviceCreate
  -> WdfDeviceCreateDeviceInterface
  -> WdfIoQueueCreate
  -> EvtDevicePrepareHardware
  -> EvtDeviceD0Entry / D0Exit
  -> EvtIoDeviceControl
```

## Target

- UMDF v2
- x64
- Intended baseline: Windows 8.1+ style UMDF v2 semantics
- Useful for userspace-ntos UMDF2 reflector/host bring-up

## Device interface

```text
{5F4D7E3B-8C43-4B9A-9E47-9E7E3DF57D22}
```

## IOCTLs

Defined in `driver/Umdf2LifecycleTest.h`:

```text
IOCTL_UMDF2LT_PING
IOCTL_UMDF2LT_GET_INFO
IOCTL_UMDF2LT_ECHO
IOCTL_UMDF2LT_FAIL_REQUEST
```

`IOCTL_UMDF2LT_GET_INFO` returns counters showing which lifecycle callbacks ran.

## Build

Open a WDK Developer Command Prompt and run:

```bat
scripts\build_wdk10_umdf2_x64.bat
```

or open `Umdf2LifecycleTest.vcxproj` in Visual Studio with the WDK installed.

## userspace-ntos fixture

`driver-bringup-fixture.json` describes a synthetic root-enumerated devnode and service entry for testing.

## Expected runtime lifecycle

```text
load UMDF2 driver DLL
call DriverEntry
WdfDriverCreate succeeds
PnP binds devnode
EvtDriverDeviceAdd fires
WdfDeviceCreate succeeds
interface is registered
queue is created
START_DEVICE / PrepareHardware fires
D0Entry fires
IOCTLs work
D0Exit / ReleaseHardware / Cleanup fire on remove
```

