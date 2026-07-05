# KmdfLoaderCompatTest

A small Windows 7-targeted KMDF driver project for testing the userspace-ntos
`NT_DRIVER_LOADING_KMDF_BINDING_COMPAT_SPEC.md` path.

The driver is intentionally ordinary KMDF, not a custom harness driver. It should bind through
`wdfldr.sys` and exercise the WDF function table path used by real KMDF drivers.

## What it tests

Driver loading / binding:

- `.sys` PE64 native driver image loading
- `ntoskrnl.exe`, `hal.dll`, and `wdfldr.sys` import resolution
- `WdfVersionBind` / WDF function table setup
- `DriverEntry`
- `WdfDriverCreate`
- `RegistryPath` correctness

KMDF / PnP:

- `EvtDriverDeviceAdd`
- `WdfDeviceCreate`
- `WdfDeviceCreateSymbolicLink`
- `WdfDeviceCreateDeviceInterface`
- `WdfIoQueueCreate`
- `EvtDevicePrepareHardware`
- `EvtDeviceReleaseHardware`
- `EvtDeviceD0Entry` / `EvtDeviceD0Exit`
- `EvtDeviceSelfManagedIoInit` / `EvtDeviceSelfManagedIoCleanup`

Registry:

- `WdfDriverOpenParametersRegistryKey`
- `WdfRegistryQueryULong("Answer")`
- `WdfRegistryAssignULong("SeenByDriver", 1)`
- `WdfRegistryQueryString("Greeting")`

Dispatcher objects:

- `WdfWorkItemCreate` / `WdfWorkItemEnqueue`
- `WdfTimerCreate` / `WdfTimerStart`

I/O:

- Default sequential queue
- `WdfRequestRetrieveInputBuffer`
- `WdfRequestRetrieveOutputBuffer`
- `WdfRequestCompleteWithInformation`

## Device names

Legacy symbolic link:

```text
\\.\KmdfLoaderCompatTest
```

NT device name:

```text
\Device\KmdfLoaderCompatTest
```

Device interface GUID:

```text
{C4BC7D8F-780E-4D27-8D81-0F973B7E0611}
```

## Registry fixture expected by userspace-ntos

The compatibility harness should create a service key like:

```text
\Registry\Machine\System\CurrentControlSet\Services\KmdfLoaderCompatTest
  Type         REG_DWORD 1
  Start        REG_DWORD 3
  ErrorControl REG_DWORD 1
  ImagePath    REG_EXPAND_SZ \SystemRoot\System32\drivers\KmdfLoaderCompatTest.sys

\Registry\Machine\System\CurrentControlSet\Services\KmdfLoaderCompatTest\Parameters
  Answer       REG_DWORD 42
  Greeting     REG_SZ "hello from registry"
```

The driver writes:

```text
SeenByDriver REG_DWORD 1
```

## PnP fixture suggested

A simple root-enumerated devnode is enough:

```text
ROOT\KMDFLOADERCOMPATTEST\0000
```

Resources are optional. If supplied, `EvtDevicePrepareHardware` counts memory and interrupt resources.
A useful test fixture is:

```json
{
  "memory": [{ "base": "0x10000000", "length": "0x1000" }],
  "interrupts": [{ "vector": 5, "irql": 5, "mode": "LevelSensitive" }]
}
```

## IOCTLs

- `IOCTL_KLCT_PING`
- `IOCTL_KLCT_ECHO`
- `IOCTL_KLCT_GET_INFO`
- `IOCTL_KLCT_GET_GREETING`
- `IOCTL_KLCT_TOUCH_WORKITEM`
- `IOCTL_KLCT_TOUCH_TIMER`

`IOCTL_KLCT_GET_INFO` returns a `KLCT_INFO` structure with callback counts, registry values,
resource counts, and version fields.

## Building with modern WDK

Open a Visual Studio x64 Native Tools prompt with WDK installed and run:

```bat
scripts\build_wdk10_x64.bat
```

The project sets:

```text
NTDDI_VERSION=0x06010000
_WIN32_WINNT=0x0601
TargetVersion=Windows7
KMDF library version: 1.11
```

## Building with Windows 7 WDK build.exe

From a Windows 7 WDK x64 build environment:

```bat
scripts\build_wdk7600_build_env.bat
```

The `driver/Sources` file is included for the classic WDK build system.

## Expected userspace-ntos bring-up phases

```text
ResolveService
OpenImage
MapImage
ResolveImports
WdfVersionBind
CreateDriverObject
CallDriverEntry
WdfDriverCreate
CallAddDevice / EvtDriverDeviceAdd
StartDevice / EvtDevicePrepareHardware / EvtDeviceD0Entry
Open legacy link or interface path
DeviceIoControl(IOCTL_KLCT_GET_INFO)
RemoveDevice / ReleaseHardware / D0Exit / SelfManagedIoCleanup
Unload / WdfVersionUnbind
```

## Notes

This driver deliberately avoids actual MMIO access or interrupt connection. It is for validating
loader/KMDF/PnP/registry/interface/queue integration before bringing in real hardware-class drivers.
