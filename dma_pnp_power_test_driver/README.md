# DmaPnpPowerTest WDM Driver

Tiny PnP + Power + DMA legacy WDM `.sys` driver for the `userspace-ntos` DMA / MDL / IOMMU-facade milestone.

This is the DMA-aware version of `PowerPnpMmioTest.sys`.

## Scope

This driver focuses on the first DMA milestone:

```text
IoGetDmaAdapter
DMA_ADAPTER
DMA_OPERATIONS
AllocateCommonBuffer
FreeCommonBuffer
common-buffer logical address
IoAllocateMdl
IoFreeMdl
MmBuildMdlForNonPagedPool
MmGetSystemAddressForMdlSafe
METHOD_OUT_DIRECT -> IRP->MdlAddress
```

Packet DMA and scatter/gather IOCTLs are included as placeholders that return `STATUS_NOT_SUPPORTED`. Once your common-buffer and MDL path is stable, those can be expanded into `MapTransfer` / `FlushAdapterBuffers` / `GetScatterGatherList`.

## Created objects

```text
\Device\DmaPnpPowerTest
\DosDevices\DmaPnpPowerTest -> \Device\DmaPnpPowerTest
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

The driver parses the translated resource list and then obtains a DMA adapter.

## Simulated DMA/MMIO register layout

The userspace-ntos simulated DMA device should expose:

```text
offset 0x00: ID register, readonly, value 0x444d4131 ("DMA1")
offset 0x04: control register
offset 0x08: status register, bit0 = done/interrupt pending, bit1 = error
offset 0x0c: interrupt ack register, write 1 clears pending
offset 0x10: DMA logical address low
offset 0x14: DMA logical address high
offset 0x18: transfer length
offset 0x1c: command
offset 0x20: result/checksum
offset 0x24: interrupt count register, optional simulated-device counter
```

## START_DEVICE behaviour

During `IRP_MN_START_DEVICE`, the driver does:

```text
forward START_DEVICE to lower PDO
parse translated CM_RESOURCE_LIST
MmMapIoSpace(memory resource)
READ_REGISTER_ULONG(ID) == 0x444d4131
IoConnectInterrupt(interrupt resource)
IoGetDmaAdapter(PDO, DEVICE_DESCRIPTION, &NumberOfMapRegisters)
DmaAdapter->DmaOperations->AllocateCommonBuffer(...)
PoSetPowerState(DevicePowerState, D0)
```

## D3 behaviour

For `IRP_MN_SET_POWER / DevicePowerState D3`:

```text
pending DMA/common-buffer command is cancelled
Powered = 0
PoSetPowerState(DevicePowerState, D3)
new IOCTL/DMA work fails STATUS_DEVICE_NOT_READY
HAL/Power Manager should also reject simulated DMA commands while not D0
```

The common buffer remains allocated across D0/D3 for this v0.1 test. It is freed on STOP/REMOVE.

## IOCTLs

```c
IOCTL_DMAPNP_GET_ID
```

Reads register `0x00`.

Expected:

```text
0x444d4131
```

```c
IOCTL_DMAPNP_GET_DMA_INFO
```

Returns:

```c
typedef struct _DMAPNP_DMA_INFO_OUT {
    ULONG NumberOfMapRegisters;
    ULONG CommonBufferLength;
    ULONG CommonLogicalAddressLow;
    ULONG CommonLogicalAddressHigh;
    ULONG CommonBufferAllocated;
} DMAPNP_DMA_INFO_OUT;
```

```c
IOCTL_DMAPNP_COMMON_BUFFER_ROUNDTRIP
```

Input/output:

```c
typedef struct _DMAPNP_COMMON_ROUNDTRIP {
    ULONG Length;
    ULONG Pattern;
    ULONG Result;
    ULONG InterruptCount;
    ULONG LogicalAddressLow;
    ULONG LogicalAddressHigh;
} DMAPNP_COMMON_ROUNDTRIP;
```

Flow:

```text
driver fills common buffer with Pattern+i bytes
driver stores the IOCTL IRP as pending
driver writes DMA logical address to MMIO regs
driver writes transfer length
driver writes command = COMMON_INVERT

simulated DMA device:
  resolves logical address through DMA Manager
  reads/writes common buffer
  writes result register
  sets status DONE
  injects interrupt

driver ISR:
  reads status
  ACKs interrupt
  queues DPC

DPC:
  reads result
  writes Result/InterruptCount to IOCTL output
  completes IRP
```

```c
IOCTL_DMAPNP_MDL_SELF_TEST
```

Tests internal MDL routines:

```text
ExAllocatePoolWithTag
IoAllocateMdl
MmBuildMdlForNonPagedPool
MmGetSystemAddressForMdlSafe
IoFreeMdl
ExFreePoolWithTag
```

Returns a simple checksum.

```c
IOCTL_DMAPNP_DIRECT_BUFFER_FILL
```

`METHOD_OUT_DIRECT`.

The driver expects `Irp->MdlAddress` to be present, maps it with `MmGetSystemAddressForMdlSafe`, fills the direct output buffer with a simple pattern, and completes synchronously.

This is an I/O Manager / MDL projection test, not full packet DMA.

```c
IOCTL_DMAPNP_PACKET_DMA_PLACEHOLDER
IOCTL_DMAPNP_SCATTER_GATHER_PLACEHOLDER
```

Return `STATUS_NOT_SUPPORTED` for now.

## Expected milestone flow

```text
load DmaPnpPowerTest.sys

DriverEntry:
  sets AddDevice
  sets IRP_MJ_PNP
  sets IRP_MJ_POWER
  sets IRP_MJ_DEVICE_CONTROL

PnP Manager:
  creates PDO/devnode
  calls AddDevice
  sends START_DEVICE with resources

Driver START_DEVICE:
  maps MMIO
  connects interrupt
  obtains DMA_ADAPTER
  allocates common buffer
  enters D0

Client:
  IOCTL_DMAPNP_GET_ID succeeds
  IOCTL_DMAPNP_GET_DMA_INFO returns common logical address
  IOCTL_DMAPNP_MDL_SELF_TEST succeeds
  IOCTL_DMAPNP_DIRECT_BUFFER_FILL succeeds if direct I/O MDL path exists

Client:
  IOCTL_DMAPNP_COMMON_BUFFER_ROUNDTRIP returns STATUS_PENDING

Sim DMA device:
  resolves logical address
  modifies common buffer
  sets result/status
  injects interrupt

Driver:
  ISR ACKs
  DPC completes pending IOCTL

Power Manager:
  SET_POWER D3
  new DMA work rejected

Power Manager:
  SET_POWER D0
  DMA work succeeds again

PnP Manager:
  REMOVE_DEVICE
  common buffer freed
  DMA adapter released
  interrupt disconnected
  MMIO unmapped
```

## Build on Windows with WDK

Recommended:

1. Open Visual Studio with the Windows Driver Kit installed.
2. Create an empty **Kernel Mode Driver, Empty (WDM)** project.
3. Add:
   - `driver/Driver.c`
   - `driver/DmaPnpPowerTest.h`
4. Build x64 Release.
5. The output will be `DmaPnpPowerTest.sys`.

A minimal `.vcxproj` is included, but WDK/MSBuild versions are fussy. If it does not load, create a fresh WDM empty project and copy the two driver files in.

## Optional user-mode tester

`user/UserTest.c` tests post-START synchronous paths:

```text
GET_POWER_STATE
GET_DMA_INFO
GET_ID
READ/WRITE register
MDL_SELF_TEST
DIRECT_BUFFER_FILL
GET_INTERRUPT_COUNT
```

It does not inject simulated DMA interrupts.

Build:

```bat
cl /W4 /Fe:DmaPnpPowerUserTest.exe user\UserTest.c
```

## Expected imports

Depending on compiler/WDK optimisation, expect imports close to:

```text
ntoskrnl.exe:
  ExAllocatePoolWithTag
  ExFreePoolWithTag
  IoAllocateMdl
  IoFreeMdl
  IoGetDmaAdapter
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
  MmBuildMdlForNonPagedPool
  MmGetSystemAddressForMdlSafe
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
