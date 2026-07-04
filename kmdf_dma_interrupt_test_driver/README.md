# KmdfDmaInterruptTest KMDF Driver

Tiny KMDF hardware-extension `.sys` driver for the `userspace-ntos` KMDF v0.2 milestone.

It tests:

```text
WDFINTERRUPT
WDFDMAENABLER
WDFCOMMONBUFFER
WDFTIMER
WDFWORKITEM
```

`WDFDMATRANSACTION` is intentionally a placeholder returning `STATUS_NOT_SUPPORTED`.

## Created objects

```text
\Device\KmdfDmaInterruptTest
\DosDevices\KmdfDmaInterruptTest -> \Device\KmdfDmaInterruptTest
```

## Simulated DMA/MMIO register layout

```text
offset 0x00: ID register, 0x4b444d41 ("KDMA") preferred, 0x444d4131 ("DMA1") accepted
offset 0x04: control
offset 0x08: status, bit0 done, bit1 error
offset 0x0c: interrupt ack, write 1 clears pending
offset 0x10: DMA logical address low
offset 0x14: DMA logical address high
offset 0x18: transfer length
offset 0x1c: command
offset 0x20: result/checksum
offset 0x24: interrupt count
```

## Expected flow

```text
DriverEntry
  -> WdfDriverCreate

EvtDeviceAdd
  -> WdfDeviceCreate
  -> WdfDeviceCreateSymbolicLink
  -> WdfIoQueueCreate
  -> WdfInterruptCreate
  -> WdfTimerCreate
  -> WdfWorkItemCreate

EvtDevicePrepareHardware
  -> WdfCmResourceListGetCount
  -> WdfCmResourceListGetDescriptor
  -> MmMapIoSpace
  -> verify ID register
  -> WdfDmaEnablerCreate
  -> WdfCommonBufferCreate
  -> WdfCommonBufferGetAlignedVirtualAddress
  -> WdfCommonBufferGetAlignedLogicalAddress
  -> WdfCommonBufferGetLength

D0
  -> WdfInterruptEnable

IOCTL_KMDF_DMA_COMMON_ROUNDTRIP
  -> fill common buffer
  -> store WDFREQUEST pending
  -> write logical address + length + command to MMIO

Simulated DMA completion
  -> HAL interrupt
  -> EvtInterruptIsr
  -> WdfInterruptQueueDpcForIsr
  -> EvtInterruptDpc
  -> WdfRequestCompleteWithInformation
```

## IOCTLs

```text
IOCTL_KMDF_DMA_PING
IOCTL_KMDF_DMA_GET_INFO
IOCTL_KMDF_DMA_READ_REG32
IOCTL_KMDF_DMA_WRITE_REG32
IOCTL_KMDF_DMA_COMMON_ROUNDTRIP
IOCTL_KMDF_DMA_TIMER_TEST
IOCTL_KMDF_DMA_WORKITEM_TEST
IOCTL_KMDF_DMA_CANCEL_PENDING
IOCTL_KMDF_DMA_TRANSACTION_PLACEHOLDER
```

## Build

Recommended:

1. Create a new **Kernel Mode Driver, Empty (KMDF)** project in Visual Studio + WDK.
2. Add:
   - `driver/Driver.c`
   - `driver/KmdfDmaInterruptTest.h`
3. Build x64 Release.

A minimal `.vcxproj` is included, but KMDF project files are WDK-version-sensitive.

## User-mode tester

Build:

```bat
cl /W4 /Fe:KmdfDmaInterruptUserTest.exe user\UserTest.c
```

Run synchronous paths:

```bat
KmdfDmaInterruptUserTest.exe
```

Run common-buffer DMA roundtrip too:

```bat
KmdfDmaInterruptUserTest.exe dma
```
