# MmioInterruptTest WDM Driver

Tiny legacy WDM `.sys` driver for the `userspace-ntos` HAL/resource/interrupt milestone.

This is the next test driver after:

- `SurtTest.sys`
- `AsyncTest.sys`

## Important

This driver is intended for `userspace-ntos`, not for production Windows machines.

It assumes fake/static translated resources:

```text
MMIO translated base: 0x10000000
MMIO length:          0x1000
Interrupt vector:     5
Interrupt IRQL:       5
Interrupt mode:       LevelSensitive
Share vector:         FALSE
Affinity:             1
```

On real Windows, mapping arbitrary physical address `0x10000000` and connecting vector `5` may fail or be unsafe. Build it with WDK to produce the `.sys`, but run it under your Driver Host / HAL service.

## What it exercises

Driver setup:

- `DriverEntry`
- `IoCreateDevice`
- `IoCreateSymbolicLink`
- `IoDeleteSymbolicLink`
- `IoDeleteDevice`
- `DriverUnload`

HAL/resource path:

- `MmMapIoSpace`
- `MmUnmapIoSpace`
- `READ_REGISTER_ULONG`
- `WRITE_REGISTER_ULONG`

Interrupt path:

- `IoConnectInterrupt`
- `IoDisconnectInterrupt`
- ISR callback
- `KeInsertQueueDpc`
- DPC callback
- pending IRP completion from DPC

I/O path:

- `IRP_MJ_CREATE`
- `IRP_MJ_CLOSE`
- `IRP_MJ_DEVICE_CONTROL`
- `METHOD_BUFFERED`
- `IoMarkIrpPending`
- `IoCompleteRequest`

## Created objects

```text
\Device\MmioInterruptTest
\DosDevices\MmioInterruptTest -> \Device\MmioInterruptTest
```

If your Object Manager uses `\??` as the DOS-device namespace, alias or map `\DosDevices` accordingly.

## Simulated register layout

The userspace-ntos simulated device should expose:

```text
offset 0x00: ID register, readonly, value 0x4d4d494f ("MMIO")
offset 0x04: control register, read/write
offset 0x08: status register, bit0 interrupt pending
offset 0x0c: interrupt ack register, write 1 clears pending
offset 0x10: interrupt count register, optional simulated-device count
```

## IOCTLs

```c
IOCTL_MMIOIT_GET_ID
```

Synchronously reads register `0x00` with `READ_REGISTER_ULONG`.

Expected output:

```text
0x4d4d494f
```

```c
IOCTL_MMIOIT_READ_REG32
```

Input/output:

```c
typedef struct _MMIOIT_REG32_REQUEST {
    ULONG Offset;
    ULONG Value;
} MMIOIT_REG32_REQUEST;
```

Reads a 32-bit register at `Offset` into `Value`.

```c
IOCTL_MMIOIT_WRITE_REG32
```

Input:

```c
MMIOIT_REG32_REQUEST { Offset, Value }
```

Writes `Value` to register `Offset`.

```c
IOCTL_MMIOIT_WRITE_CONTROL
```

Input:

```c
ULONG control_value
```

Writes to register `0x04`.

```c
IOCTL_MMIOIT_WAIT_FOR_INTERRUPT
```

Returns `STATUS_PENDING`.

The driver stores the IRP. When your test injects an interrupt through the HAL service:

```text
HAL_OP_INJECT_INTERRUPT
```

the ISR should run, acknowledge the interrupt by writing `1` to register `0x0c`, queue a DPC, and the DPC should complete the pending IRP.

Output:

```text
ULONG interrupt_count
```

```c
IOCTL_MMIOIT_GET_INTERRUPT_COUNT
```

Synchronously returns the driver's interrupt count.

```c
IOCTL_MMIOIT_CANCEL_WAIT
```

Cancels any pending interrupt wait IRP.

```c
IOCTL_MMIOIT_DISCONNECT_INTERRUPT
IOCTL_MMIOIT_RECONNECT_INTERRUPT
```

Useful for testing `IoDisconnectInterrupt` and reconnect behaviour.

## Expected milestone flow

A successful userspace-ntos integration test:

```text
load MmioInterruptTest.sys
call DriverEntry

DriverEntry:
  IoCreateDevice
  MmMapIoSpace(0x10000000, 0x1000, MmNonCached)
  READ_REGISTER_ULONG(ID) == 0x4d4d494f
  IoConnectInterrupt(vector=5, irql=5)
  IoCreateSymbolicLink

client:
  open \DosDevices\MmioInterruptTest
  send IOCTL_MMIOIT_GET_ID
  expect 0x4d4d494f

client:
  send IOCTL_MMIOIT_WRITE_CONTROL
  read back register 0x04

client:
  send IOCTL_MMIOIT_WAIT_FOR_INTERRUPT
  expect STATUS_PENDING

test harness:
  set simulated status register bit0
  inject interrupt resource/vector

Driver Host:
  calls ISR
  ISR reads status
  ISR writes ack
  ISR queues DPC

dispatcher:
  runs DPC
  DPC completes pending IRP

client:
  receives STATUS_SUCCESS and interrupt_count
```

## Build on Windows with WDK

Recommended:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add:
   - `driver/Driver.c`
   - `driver/MmioInterruptTest.h`
4. Build x64 Release.
5. The output will be `MmioInterruptTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Optional user-mode tester

`user/UserTest.c` only tests the synchronous paths. It does not inject the fake interrupt.

Build:

```bat
cl /W4 /Fe:MmioInterruptUserTest.exe user\UserTest.c
```

## Expected imports

Depending on compiler/WDK optimisation, expect imports close to:

```text
ntoskrnl.exe:
  IoCreateDevice
  IoCreateSymbolicLink
  IoDeleteDevice
  IoDeleteSymbolicLink
  IofCompleteRequest
  IoConnectInterrupt
  IoDisconnectInterrupt
  KeAcquireSpinLockRaiseToDpc / KeAcquireSpinLock
  KeInitializeDpc
  KeInitializeSpinLock
  KeInsertQueueDpc
  KeReleaseSpinLock
  MmMapIoSpace
  MmUnmapIoSpace
  RtlInitUnicodeString
  RtlZeroMemory / memset-like import depending on compiler
```

Register access helpers may compile as macros/inline volatile memory accesses rather than imports. That is fine: it means your Driver Host needs to make the mapped pointer projection work.
