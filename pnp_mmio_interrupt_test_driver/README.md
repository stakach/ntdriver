# PnpMmioInterruptTest WDM Driver

Tiny PnP-style legacy WDM `.sys` driver for the `userspace-ntos` minimal PnP Manager milestone.

This is the PnP version of `MmioInterruptTest.sys`.

## Important

This is intended for `userspace-ntos`, not production Windows systems.

Unlike `MmioInterruptTest.sys`, this driver does **not** hard-code MMIO/interrupt resources in `DriverEntry`.

Instead:

```text
DriverEntry
  -> sets DriverObject->DriverExtension->AddDevice

PnP Manager
  -> creates PDO/devnode
  -> calls AddDevice

AddDevice
  -> creates FDO
  -> IoAttachDeviceToDeviceStack(FDO, PDO)
  -> creates \Device\PnpMmioInterruptTest
  -> creates \DosDevices\PnpMmioInterruptTest

PnP Manager
  -> sends IRP_MN_START_DEVICE with translated resources

START_DEVICE handler
  -> parses CM_RESOURCE_LIST
  -> MmMapIoSpace(memory resource)
  -> READ_REGISTER_ULONG(ID)
  -> IoConnectInterrupt(interrupt resource)
```

## Created objects

```text
\Device\PnpMmioInterruptTest
\DosDevices\PnpMmioInterruptTest -> \Device\PnpMmioInterruptTest
```

If your Object Manager uses `\??`, alias or map `\DosDevices` accordingly.

## Expected translated resources

The PnP Manager should deliver a `CM_RESOURCE_LIST` with:

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

The exact values do not need to be hard-coded in the driver; they are parsed from `AllocatedResourcesTranslated`.

## Simulated register layout

The userspace-ntos simulated device should expose:

```text
offset 0x00: ID register, readonly, value 0x4d4d494f ("MMIO")
offset 0x04: control register, read/write
offset 0x08: status register, bit0 = interrupt pending
offset 0x0c: interrupt ack register, write 1 clears pending
offset 0x10: interrupt count register, optional simulated-device count
```

## IOCTLs

```c
IOCTL_PNPIT_GET_ID
```

Synchronously reads register `0x00`.

Expected output:

```text
0x4d4d494f
```

```c
IOCTL_PNPIT_READ_REG32
IOCTL_PNPIT_WRITE_REG32
```

Use:

```c
typedef struct _PNPIT_REG32_REQUEST {
    ULONG Offset;
    ULONG Value;
} PNPIT_REG32_REQUEST;
```

```c
IOCTL_PNPIT_WRITE_CONTROL
```

Writes an `ULONG` to register `0x04`.

```c
IOCTL_PNPIT_WAIT_FOR_INTERRUPT
```

Returns `STATUS_PENDING`.

The driver stores the IRP. When your HAL/PnP test harness injects the interrupt:

```text
HAL_OP_INJECT_INTERRUPT
```

the ISR should run, acknowledge the interrupt by writing `1` to register `0x0c`, queue a DPC, and the DPC should complete the pending IRP.

Output:

```text
ULONG interrupt_count
```

```c
IOCTL_PNPIT_GET_INTERRUPT_COUNT
```

Synchronously returns the driver's interrupt count.

```c
IOCTL_PNPIT_CANCEL_WAIT
```

Cancels any pending interrupt wait IRP.

## Expected userspace-ntos milestone flow

```text
load PnpMmioInterruptTest.sys

DriverEntry:
  sets AddDevice
  sets IRP_MJ_PNP
  returns STATUS_SUCCESS

PnP Manager:
  creates PDO/devnode
  calls AddDevice

AddDevice:
  IoCreateDevice(FDO)
  IoAttachDeviceToDeviceStack(FDO, PDO)
  IoCreateSymbolicLink
  returns STATUS_SUCCESS

Before START_DEVICE:
  CREATE/IOCTL should fail with STATUS_DEVICE_NOT_READY

PnP Manager:
  sends IRP_MJ_PNP / IRP_MN_START_DEVICE
  supplies raw + translated CM_RESOURCE_LISTs

Driver:
  forwards START_DEVICE to lower PDO
  parses translated resources
  MmMapIoSpace(memory resource)
  reads ID register == 0x4d4d494f
  IoConnectInterrupt(interrupt resource)
  completes START_DEVICE success

Client:
  opens device
  sends IOCTL_PNPIT_GET_ID
  sends IOCTL_PNPIT_WAIT_FOR_INTERRUPT

Test harness:
  sets simulated status bit0
  injects interrupt

Driver Host:
  invokes ISR
  ISR reads status
  ISR writes ACK
  ISR queues DPC

Dispatcher:
  runs DPC
  DPC completes pending IOCTL

PnP Manager:
  sends IRP_MN_REMOVE_DEVICE

Driver:
  cancels pending wait
  disconnects interrupt
  unmaps MMIO
  deletes symlink
  detaches from PDO
  deletes FDO
```

## Build on Windows with WDK

Recommended:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add:
   - `driver/Driver.c`
   - `driver/PnpMmioInterruptTest.h`
4. Build x64 Release.
5. The output will be `PnpMmioInterruptTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Optional user-mode tester

`user/UserTest.c` only tests post-START synchronous paths. It does not inject the fake interrupt.

Build:

```bat
cl /W4 /Fe:PnpMmioInterruptUserTest.exe user\UserTest.c
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
