# PowerPnpMmioTest WDM Driver

Tiny PnP + Power legacy WDM `.sys` driver for the `userspace-ntos` Power Manager milestone.

This is the power-aware version of `PnpMmioInterruptTest.sys`.

## Important

This is intended for `userspace-ntos`, not production Windows systems.

The driver expects your NT personality to perform the normal staged flow:

```text
DriverEntry
  -> AddDevice
  -> IRP_MN_START_DEVICE
  -> IRP_MJ_POWER / IRP_MN_QUERY_POWER
  -> IRP_MJ_POWER / IRP_MN_SET_POWER
  -> IRP_MN_REMOVE_DEVICE
```

## Created objects

```text
\Device\PowerPnpMmioTest
\DosDevices\PowerPnpMmioTest -> \Device\PowerPnpMmioTest
```

If your Object Manager uses `\??`, alias or map `\DosDevices` accordingly.

## Expected translated resources

The PnP Manager should deliver a `CM_RESOURCE_LIST` during `IRP_MN_START_DEVICE` with:

```text
Memory:
  start  = 0x10000000
  length = 0x1000

Interrupt:
  vector   = 5
  level    = 5
  affinity = 1
  mode     = LevelSensitive
```

The driver parses the translated resource list; it does not hard-code the values during `DriverEntry`.

## Simulated register layout

The userspace-ntos simulated device should expose:

```text
offset 0x00: ID register, readonly, value 0x4d4d494f ("MMIO")
offset 0x04: control register, read/write
offset 0x08: status register, bit0 = interrupt pending
offset 0x0c: interrupt ack register, write 1 clears pending
offset 0x10: interrupt count register, optional simulated-device count
```

## Power behaviour

After `IRP_MN_START_DEVICE` succeeds:

```text
Started = 1
Powered = 1
DevicePowerState = PowerDeviceD0
```

For `IRP_MJ_POWER / IRP_MN_QUERY_POWER`:

```text
DevicePowerState D0/D1/D2/D3 accepted
SystemPowerState Working/Sleep/Hibernate/Shutdown-like values accepted
```

For `IRP_MJ_POWER / IRP_MN_SET_POWER`:

```text
DevicePowerState D0:
  Powered = 1
  PoSetPowerState(DevicePowerState, D0)
  IOCTL/MMIO/interrupt path works

DevicePowerState D1/D2/D3:
  cancels pending interrupt wait
  Powered = 0
  PoSetPowerState(DevicePowerState, target)
  IOCTL/MMIO/interrupt path rejected with STATUS_DEVICE_NOT_READY

SystemPowerState Working:
  maps locally to D0

SystemPowerState > Working:
  maps locally to D3
```

The driver keeps MMIO mappings and interrupt connection across D0/D3 for this test. The Power Manager/HAL service should also enforce or trace power gating, especially dropping injected interrupts while not D0.

## IOCTLs

```c
IOCTL_POWERPNP_GET_POWER_STATE
```

Always allowed unless removed. Returns:

```c
typedef struct _POWERPNP_POWER_STATE_OUT {
    ULONG Started;
    ULONG Powered;
    ULONG SystemPowerState;
    ULONG DevicePowerState;
} POWERPNP_POWER_STATE_OUT;
```

```c
IOCTL_POWERPNP_GET_ID
```

Synchronously reads register `0x00`.

Expected output:

```text
0x4d4d494f
```

```c
IOCTL_POWERPNP_READ_REG32
IOCTL_POWERPNP_WRITE_REG32
```

Use:

```c
typedef struct _POWERPNP_REG32_REQUEST {
    ULONG Offset;
    ULONG Value;
} POWERPNP_REG32_REQUEST;
```

```c
IOCTL_POWERPNP_WRITE_CONTROL
```

Writes an `ULONG` to register `0x04`.

```c
IOCTL_POWERPNP_WAIT_FOR_INTERRUPT
```

Returns `STATUS_PENDING`.

The driver stores the IRP. When your HAL/Power test harness injects the interrupt in D0, the ISR should run, acknowledge the interrupt by writing `1` to register `0x0c`, queue a DPC, and the DPC should complete the pending IRP.

If the device is D3, this IOCTL returns `STATUS_DEVICE_NOT_READY`.

```c
IOCTL_POWERPNP_GET_INTERRUPT_COUNT
```

Synchronously returns the driver's interrupt count.

```c
IOCTL_POWERPNP_CANCEL_WAIT
```

Cancels any pending interrupt wait IRP.

## Expected userspace-ntos milestone flow

```text
load PowerPnpMmioTest.sys

DriverEntry:
  sets AddDevice
  sets IRP_MJ_PNP
  sets IRP_MJ_POWER
  returns STATUS_SUCCESS

PnP Manager:
  creates PDO/devnode
  calls AddDevice

AddDevice:
  IoCreateDevice(FDO)
  IoAttachDeviceToDeviceStack(FDO, PDO)
  IoCreateSymbolicLink
  returns STATUS_SUCCESS

PnP Manager:
  sends IRP_MJ_PNP / IRP_MN_START_DEVICE
  supplies raw + translated CM_RESOURCE_LISTs

Driver:
  forwards START_DEVICE to lower PDO
  parses translated resources
  MmMapIoSpace(memory resource)
  reads ID register == 0x4d4d494f
  IoConnectInterrupt(interrupt resource)
  PoSetPowerState(DevicePowerState, D0)
  completes START_DEVICE success

Power Manager:
  sends IRP_MJ_POWER / IRP_MN_QUERY_POWER / DevicePowerState D3
  expects STATUS_SUCCESS

Power Manager:
  sends IRP_MJ_POWER / IRP_MN_SET_POWER / DevicePowerState D3

Driver:
  cancels pending wait
  Powered = 0
  PoSetPowerState(DevicePowerState, D3)
  forwards to lower PDO
  completes success

Client:
  IOCTL_POWERPNP_GET_ID should fail STATUS_DEVICE_NOT_READY
  interrupt injection should be dropped by HAL/Power gating

Power Manager:
  sends IRP_MJ_POWER / IRP_MN_SET_POWER / DevicePowerState D0

Driver:
  forwards to lower PDO
  Powered = 1
  PoSetPowerState(DevicePowerState, D0)
  completes success

Client:
  IOCTL_POWERPNP_GET_ID works again
  WAIT_FOR_INTERRUPT + injected interrupt completes pending IRP

PnP Manager:
  sends IRP_MN_REMOVE_DEVICE

Driver:
  cancels pending wait
  sets D3
  disconnects interrupt
  unmaps MMIO
  deletes symlink
  detaches FDO
  deletes device
```

## Build on Windows with WDK

Recommended:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add:
   - `driver/Driver.c`
   - `driver/PowerPnpMmioTest.h`
4. Build x64 Release.
5. The output will be `PowerPnpMmioTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Optional user-mode tester

`user/UserTest.c` only tests post-START synchronous paths and `GET_POWER_STATE`.

It does not send power IRPs or inject interrupts. Drive those through the userspace-ntos Power Manager tests.

Build:

```bat
cl /W4 /Fe:PowerPnpMmioUserTest.exe user\UserTest.c
```

## Expected imports

Depending on compiler/WDK optimisation, expect imports close to:

```text
ntoskrnl.exe:
  IoCreateDevice
  IoCreateSymbolicLink
  IoDeleteDevice
  IoDeleteSymbolicLink
  IoAttachDeviceToDeviceStack
  IoDetachDevice
  IoCallDriver
  IofCompleteRequest
  IoSetCompletionRoutine
  KeInitializeEvent
  KeSetEvent
  KeWaitForSingleObject
  KeInitializeSpinLock
  KeAcquireSpinLock
  KeReleaseSpinLock
  KeInitializeDpc
  KeInsertQueueDpc
  IoConnectInterrupt
  IoDisconnectInterrupt
  MmMapIoSpace
  MmUnmapIoSpace
  PoCallDriver
  PoSetPowerState
  PoStartNextPowerIrp
  RtlInitUnicodeString
  RtlZeroMemory / memset-like import depending on compiler
```

Many helpers are macros and may not appear as imports:

```text
IoGetCurrentIrpStackLocation
IoSkipCurrentIrpStackLocation
IoCopyCurrentIrpStackLocationToNext
IoMarkIrpPending
READ_REGISTER_ULONG
WRITE_REGISTER_ULONG
```
