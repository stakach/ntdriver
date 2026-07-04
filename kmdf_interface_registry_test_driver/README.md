# KmdfInterfaceRegistryTest KMDF Driver

Tiny KMDF `.sys` driver for the `userspace-ntos` Device Interface / Registry / PnP Property milestone.

## What it tests

```text
WdfDeviceCreateDeviceInterface
WdfDeviceRetrieveDeviceInterfaceString
WDFSTRING
WdfStringCreate
WdfStringGetUnicodeString
WdfDriverOpenParametersRegistryKey
WdfDeviceOpenRegistryKey
WdfRegistryQueryULong
WdfRegistryAssignULong
WdfRegistryQueryString
WdfRegistryAssignString
WdfRegistryClose
WdfDeviceAssignProperty
WdfDeviceInitAssignName
WdfDeviceCreateSymbolicLink
WdfIoQueueCreate
WdfRequestRetrieveInputBuffer
WdfRequestRetrieveOutputBuffer
WdfRequestCompleteWithInformation
```

## Created objects

```text
\Device\KmdfInterfaceRegistryTest
\DosDevices\KmdfInterfaceRegistryTest -> \Device\KmdfInterfaceRegistryTest
```

The legacy symbolic link is included as a fallback while device-interface enumeration is being implemented.

## Device interface GUID

```text
{9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001}
```

The driver calls:

```c
WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_USERSPACE_NTOS_TEST, NULL);
WdfDeviceRetrieveDeviceInterfaceString(Device, &GUID_DEVINTERFACE_USERSPACE_NTOS_TEST, NULL, InterfaceString);
```

Your PnP/device-interface manager should enable the interface after successful `START_DEVICE` and disable it on `REMOVE_DEVICE`.

## Expected registry fixture

Recommended fixture values:

```text
\Registry\Machine\System\CurrentControlSet\Services\KmdfInterfaceRegistryTest\Parameters\Answer = REG_DWORD 42
\Registry\Machine\System\CurrentControlSet\Services\KmdfInterfaceRegistryTest\Parameters\Greeting = REG_SZ "hello registry"
```

The driver reads those values. If they are missing, it writes defaults so you can bootstrap the registry implementation.

The driver also writes:

```text
Services\KmdfInterfaceRegistryTest\Parameters\SeenByDriver = REG_DWORD 1
Enum\...\DeviceSeenByDriver = REG_DWORD 1
Enum\...\RuntimeValue = REG_DWORD <runtime set value>
```

## PnP properties

The driver calls `WdfDeviceAssignProperty` for:

```text
DEVPKEY_Device_FriendlyName
DEVPKEY_UserspaceNtos_InterfaceTestAnswer
```

The custom DEVPROPKEY uses the same GUID as the interface, PID 2.

## IOCTLs

```c
IOCTL_KMDF_IF_PING
```

Returns a marker `ULONG`.

```c
IOCTL_KMDF_IF_GET_CONFIG
```

Returns:

```c
typedef struct _KMDF_IF_CONFIG {
    ULONG Prepared;
    ULONG Powered;
    ULONG InterfaceCreated;
    ULONG Answer;
    ULONG SeenByDriver;
    ULONG DeviceSeenByDriver;
    ULONG RuntimeValue;
    ULONG CustomPropertyValue;
    ULONG GreetingLengthBytes;
    ULONG InterfaceStringLengthBytes;
    ULONG IoctlCount;
} KMDF_IF_CONFIG;
```

```c
IOCTL_KMDF_IF_GET_INTERFACE_STRING
IOCTL_KMDF_IF_GET_GREETING
```

Return UTF-16 strings:

```c
typedef struct _KMDF_IF_STRING_OUT {
    USHORT LengthBytes;
    USHORT MaximumLengthBytes;
    WCHAR Buffer[260];
} KMDF_IF_STRING_OUT;
```

```c
IOCTL_KMDF_IF_SET_RUNTIME_VALUE
IOCTL_KMDF_IF_GET_RUNTIME_VALUE
```

Write/read a `REG_DWORD` named `RuntimeValue` under the device registry key.

```c
IOCTL_KMDF_IF_ECHO
```

Copies input to output through the KMDF request-buffer path.

## Expected userspace-ntos flow

```text
load KmdfInterfaceRegistryTest.sys

DriverEntry:
  WdfDriverCreate

EvtDeviceAdd:
  WdfDeviceInitAssignName
  WdfDeviceCreate
  WdfDeviceCreateSymbolicLink
  WdfDeviceCreateDeviceInterface
  WdfDeviceRetrieveDeviceInterfaceString
  WdfDriverOpenParametersRegistryKey
  WdfRegistryQueryULong("Answer")
  WdfRegistryAssignULong("SeenByDriver", 1)
  WdfRegistryQueryString("Greeting")
  WdfDeviceOpenRegistryKey(PLUGPLAY_REGKEY_DEVICE)
  WdfRegistryAssignULong("DeviceSeenByDriver", 1)
  WdfDeviceAssignProperty(...)
  WdfIoQueueCreate

START_DEVICE:
  PnP manager enables interface

test harness:
  enumerate {9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001}
  open interface symbolic link
  send IOCTL_KMDF_IF_GET_CONFIG
  verify Answer=42 and SeenByDriver=1

REMOVE_DEVICE:
  PnP manager disables interface
  opening old interface path fails
```

## Build on Windows with WDK

Recommended:

1. Create a new **Kernel Mode Driver, Empty (KMDF)** project.
2. Add:
   - `driver/Driver.c`
   - `driver/KmdfInterfaceRegistryTest.h`
3. Build x64 Release.

A minimal `.vcxproj` is included, but KMDF/MSBuild project files are WDK-version-sensitive. If it does not load, create a fresh KMDF empty project and copy the driver files in.

## Optional user-mode tester

Build:

```bat
cl /W4 /Fe:KmdfInterfaceRegistryUserTest.exe user\UserTest.c
```

Open legacy symbolic link:

```bat
KmdfInterfaceRegistryUserTest.exe
```

Open an enumerated device-interface path:

```bat
KmdfInterfaceRegistryUserTest.exe "\\??\\DEVICEINTERFACE\\{9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001}\\ROOT#KMDF_INTERFACE_TEST#0000"
```

## Expected WDF APIs

Depending on WDK build style, these may be direct imports or `WdfFunctions` table entries through `WdfVersionBind`.

```text
WdfDriverCreate
WdfDeviceCreate
WdfDeviceCreateSymbolicLink
WdfDeviceInitAssignName
WdfDeviceInitSetDeviceType
WdfDeviceInitSetIoType
WdfDeviceInitSetExclusive
WdfDeviceInitSetPnpPowerEventCallbacks
WdfDeviceCreateDeviceInterface
WdfDeviceRetrieveDeviceInterfaceString
WdfDeviceOpenRegistryKey
WdfDeviceAssignProperty
WdfDriverOpenParametersRegistryKey
WdfRegistryQueryULong
WdfRegistryAssignULong
WdfRegistryQueryString
WdfRegistryAssignString
WdfRegistryClose
WdfStringCreate
WdfStringGetUnicodeString
WdfIoQueueCreate
WdfIoQueueGetDevice
WdfRequestRetrieveInputBuffer
WdfRequestRetrieveOutputBuffer
WdfRequestCompleteWithInformation
```
