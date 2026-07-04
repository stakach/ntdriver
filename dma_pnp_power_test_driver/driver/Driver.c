/*
 * DmaPnpPowerTest - tiny legacy WDM PnP + Power + DMA function driver
 * for userspace-ntos.
 *
 * Purpose:
 *   - PnP-style WDM driver
 *   - Power-aware D0/D3 behaviour
 *   - MMIO + interrupt path
 *   - IoGetDmaAdapter
 *   - DMA_ADAPTER / DMA_OPERATIONS
 *   - AllocateCommonBuffer / FreeCommonBuffer
 *   - IoAllocateMdl / IoFreeMdl / MmBuildMdlForNonPagedPool
 *   - MmGetSystemAddressForMdlSafe
 *   - METHOD_OUT_DIRECT MDL exposure via IRP->MdlAddress
 *
 * v0.1 target:
 *   common-buffer simulated DMA first.
 *
 * Expected NT objects:
 *   \Device\DmaPnpPowerTest
 *   \DosDevices\DmaPnpPowerTest -> \Device\DmaPnpPowerTest
 *
 * Expected translated resources in IRP_MN_START_DEVICE:
 *   Memory resource:
 *     start  = 0x10000000
 *     length = 0x1000
 *
 *   Interrupt resource:
 *     vector   = 5
 *     level    = 5
 *     affinity = 1
 *     mode     = LevelSensitive
 *
 * Simulated DMA/MMIO register layout:
 *   0x00 ID register, readonly, expected 0x444d4131 ("DMA1")
 *   0x04 control register
 *   0x08 status register, bit0 = done/interrupt pending, bit1 = error
 *   0x0c interrupt ack register, write 1 clears pending
 *   0x10 DMA logical address low
 *   0x14 DMA logical address high
 *   0x18 transfer length
 *   0x1c command
 *   0x20 result/checksum
 *   0x24 interrupt count register, optional simulated-device counter
 */

#include <ntddk.h>
#include "DmaPnpPowerTest.h"

#define DMAPNP_DEVICE_NAME   L"\\Device\\DmaPnpPowerTest"
#define DMAPNP_DOS_NAME      L"\\DosDevices\\DmaPnpPowerTest"

#define DMAPNP_REG_ID        0x00
#define DMAPNP_REG_CONTROL   0x04
#define DMAPNP_REG_STATUS    0x08
#define DMAPNP_REG_ACK       0x0c
#define DMAPNP_REG_DMA_LO    0x10
#define DMAPNP_REG_DMA_HI    0x14
#define DMAPNP_REG_LENGTH    0x18
#define DMAPNP_REG_COMMAND   0x1c
#define DMAPNP_REG_RESULT    0x20
#define DMAPNP_REG_IRQ_COUNT 0x24

#define DMAPNP_STATUS_DONE   0x00000001UL
#define DMAPNP_STATUS_ERROR  0x00000002UL

#define DMAPNP_COMMON_BUFFER_LENGTH 4096
#define DMAPNP_POOL_TAG 'PmDD' /* "DDmP" */

#define DMAPNP_CMD_COMMON_INVERT       1
#define DMAPNP_CMD_COMMON_FILL_PATTERN 2
#define DMAPNP_CMD_COMMON_CHECKSUM     3

typedef enum _DMAPNP_PENDING_KIND {
    DmaPnpPendingNone = 0,
    DmaPnpPendingCommonRoundtrip = 1,
    DmaPnpPendingWaitOnly = 2
} DMAPNP_PENDING_KIND;

typedef struct _DMAPNP_DEVICE_EXTENSION {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT LowerDeviceObject;

    PUCHAR MmioBase;
    ULONG MmioLength;
    PHYSICAL_ADDRESS MmioPhysicalStart;

    PKINTERRUPT InterruptObject;
    KSPIN_LOCK InterruptLock;
    ULONG InterruptVector;
    KIRQL InterruptLevel;
    KAFFINITY InterruptAffinity;
    KINTERRUPT_MODE InterruptMode;

    PDMA_ADAPTER DmaAdapter;
    ULONG NumberOfMapRegisters;
    PVOID CommonBuffer;
    PHYSICAL_ADDRESS CommonLogicalAddress;
    ULONG CommonBufferLength;
    BOOLEAN CommonBufferAllocated;

    KDPC Dpc;
    KSPIN_LOCK PendingIrpLock;
    PIRP PendingIrp;
    DMAPNP_PENDING_KIND PendingKind;

    volatile LONG InterruptCount;
    volatile LONG Started;
    volatile LONG Powered;
    volatile LONG Removed;

    SYSTEM_POWER_STATE SystemPowerState;
    DEVICE_POWER_STATE DevicePowerState;

    BOOLEAN SymbolicLinkCreated;
    BOOLEAN Mapped;
    BOOLEAN InterruptConnected;
} DMAPNP_DEVICE_EXTENSION, *PDMAPNP_DEVICE_EXTENSION;

static __inline PULONG
DmaPnpReg32(_In_ PDMAPNP_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
{
    return (PULONG)(Ext->MmioBase + Offset);
}

static NTSTATUS
DmaPnpCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN
DmaPnpValidReg32Offset(_In_ PDMAPNP_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
{
    if ((Offset & 0x3) != 0) {
        return FALSE;
    }

    if (Ext->MmioLength < sizeof(ULONG)) {
        return FALSE;
    }

    if (Offset > (Ext->MmioLength - sizeof(ULONG))) {
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
DmaPnpInterruptService(_In_ PKINTERRUPT Interrupt, _In_opt_ PVOID ServiceContext)
{
    PDEVICE_OBJECT device;
    PDMAPNP_DEVICE_EXTENSION ext;
    ULONG status;

    UNREFERENCED_PARAMETER(Interrupt);

    device = (PDEVICE_OBJECT)ServiceContext;
    ext = (PDMAPNP_DEVICE_EXTENSION)device->DeviceExtension;

    if (ext->Removed != 0 || ext->Started == 0 || ext->Powered == 0 || !ext->Mapped) {
        return FALSE;
    }

    status = READ_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_STATUS));
    if ((status & DMAPNP_STATUS_DONE) == 0) {
        return FALSE;
    }

    WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_ACK), 1);

    InterlockedIncrement(&ext->InterruptCount);
    KeInsertQueueDpc(&ext->Dpc, NULL, NULL);

    return TRUE;
}

static VOID
DmaPnpDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PDEVICE_OBJECT device;
    PDMAPNP_DEVICE_EXTENSION ext;
    PIRP irp;
    DMAPNP_PENDING_KIND kind;
    KIRQL old_irql;
    ULONG result;
    ULONG status_reg;
    void *buffer;
    PIO_STACK_LOCATION stack;
    ULONG out_len;
    PDMAPNP_COMMON_ROUNDTRIP req;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    device = (PDEVICE_OBJECT)DeferredContext;
    ext = (PDMAPNP_DEVICE_EXTENSION)device->DeviceExtension;

    irp = NULL;
    kind = DmaPnpPendingNone;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingIrp != NULL) {
        irp = ext->PendingIrp;
        kind = ext->PendingKind;
        ext->PendingIrp = NULL;
        ext->PendingKind = DmaPnpPendingNone;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (irp == NULL) {
        return;
    }

    status_reg = READ_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_STATUS));
    result = READ_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_RESULT));

    stack = IoGetCurrentIrpStackLocation(irp);
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = irp->AssociatedIrp.SystemBuffer;

    if ((status_reg & DMAPNP_STATUS_ERROR) != 0) {
        DmaPnpCompleteIrp(irp, STATUS_UNSUCCESSFUL, 0);
        return;
    }

    if (kind == DmaPnpPendingCommonRoundtrip) {
        if (buffer == NULL || out_len < sizeof(DMAPNP_COMMON_ROUNDTRIP)) {
            DmaPnpCompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, 0);
            return;
        }

        req = (PDMAPNP_COMMON_ROUNDTRIP)buffer;
        req->Result = result;
        req->InterruptCount = (ULONG)ext->InterruptCount;

        DmaPnpCompleteIrp(irp, STATUS_SUCCESS, sizeof(DMAPNP_COMMON_ROUNDTRIP));
        return;
    }

    if (kind == DmaPnpPendingWaitOnly) {
        if (buffer != NULL && out_len >= sizeof(ULONG)) {
            *((ULONG *)buffer) = (ULONG)ext->InterruptCount;
            DmaPnpCompleteIrp(irp, STATUS_SUCCESS, sizeof(ULONG));
        } else {
            DmaPnpCompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        return;
    }

    DmaPnpCompleteIrp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
DmaPnpCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
)
{
    PKEVENT event;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    event = (PKEVENT)Context;
    KeSetEvent(event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
DmaPnpForwardIrpSynchronously(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    KEVENT event;
    NTSTATUS status;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, DmaPnpCompletionRoutine, &event, TRUE, TRUE, TRUE);

    status = IoCallDriver(ext->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

static NTSTATUS
DmaPnpForwardPowerIrpSynchronously(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    KEVENT event;
    NTSTATUS status;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, DmaPnpCompletionRoutine, &event, TRUE, TRUE, TRUE);

    status = PoCallDriver(ext->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

static NTSTATUS
DmaPnpForwardIrpAndForget(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDeviceObject, Irp);
}

static NTSTATUS
DmaPnpForwardPowerIrpAndForget(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);

    if (ext->LowerDeviceObject == NULL) {
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDeviceObject, Irp);
}

static VOID
DmaPnpCancelPending(_In_ PDEVICE_OBJECT DeviceObject, _In_ NTSTATUS Status)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PIRP pending;
    KIRQL old_irql;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    pending = NULL;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingIrp != NULL) {
        pending = ext->PendingIrp;
        ext->PendingIrp = NULL;
        ext->PendingKind = DmaPnpPendingNone;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (pending != NULL) {
        DmaPnpCompleteIrp(pending, Status, 0);
    }
}

static VOID
DmaPnpDisconnectInterrupt(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject = NULL;
        ext->InterruptConnected = FALSE;
    }
}

static VOID
DmaPnpFreeDmaResources(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->DmaAdapter != NULL && ext->DmaAdapter->DmaOperations != NULL) {
        if (ext->CommonBufferAllocated && ext->CommonBuffer != NULL) {
            ext->DmaAdapter->DmaOperations->FreeCommonBuffer(
                ext->DmaAdapter,
                ext->CommonBufferLength,
                ext->CommonLogicalAddress,
                ext->CommonBuffer,
                FALSE
            );

            ext->CommonBuffer = NULL;
            ext->CommonBufferLength = 0;
            ext->CommonLogicalAddress.QuadPart = 0;
            ext->CommonBufferAllocated = FALSE;
        }

        if (ext->DmaAdapter->DmaOperations->PutDmaAdapter != NULL) {
            ext->DmaAdapter->DmaOperations->PutDmaAdapter(ext->DmaAdapter);
        }

        ext->DmaAdapter = NULL;
        ext->NumberOfMapRegisters = 0;
    }
}

static VOID
DmaPnpUnmapHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Mapped && ext->MmioBase != NULL) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
        ext->Mapped = FALSE;
    }
}

static VOID
DmaPnpStopHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    DmaPnpCancelPending(DeviceObject, STATUS_CANCELLED);

    ext->Powered = 0;
    ext->DevicePowerState = PowerDeviceD3;
    state.DeviceState = PowerDeviceD3;
    PoSetPowerState(DeviceObject, DevicePowerState, state);

    DmaPnpDisconnectInterrupt(DeviceObject);
    DmaPnpFreeDmaResources(DeviceObject);
    DmaPnpUnmapHardware(DeviceObject);

    ext->Started = 0;
}

static NTSTATUS
DmaPnpFindTranslatedResources(
    _In_ PCM_RESOURCE_LIST ResourcesTranslated,
    _Out_ PHYSICAL_ADDRESS *MemoryStart,
    _Out_ PULONG MemoryLength,
    _Out_ PULONG Vector,
    _Out_ KIRQL *Level,
    _Out_ PKAFFINITY Affinity,
    _Out_ KINTERRUPT_MODE *Mode
)
{
    ULONG full_index;
    ULONG partial_index;
    BOOLEAN found_memory;
    BOOLEAN found_interrupt;

    found_memory = FALSE;
    found_interrupt = FALSE;

    if (ResourcesTranslated == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (full_index = 0; full_index < ResourcesTranslated->Count; full_index++) {
        PCM_FULL_RESOURCE_DESCRIPTOR full;
        PCM_PARTIAL_RESOURCE_LIST partial_list;

        full = &ResourcesTranslated->List[full_index];
        partial_list = &full->PartialResourceList;

        for (partial_index = 0; partial_index < partial_list->Count; partial_index++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;

            desc = &partial_list->PartialDescriptors[partial_index];

            switch (desc->Type) {
            case CmResourceTypeMemory:
                if (!found_memory) {
                    *MemoryStart = desc->u.Memory.Start;
                    *MemoryLength = desc->u.Memory.Length;
                    found_memory = TRUE;
                }
                break;

            case CmResourceTypeInterrupt:
                if (!found_interrupt) {
                    *Vector = desc->u.Interrupt.Vector;
                    *Level = (KIRQL)desc->u.Interrupt.Level;
                    *Affinity = desc->u.Interrupt.Affinity;

                    if ((desc->Flags & CM_RESOURCE_INTERRUPT_LATCHED) != 0) {
                        *Mode = Latched;
                    } else {
                        *Mode = LevelSensitive;
                    }

                    found_interrupt = TRUE;
                }
                break;

            default:
                break;
            }
        }
    }

    if (!found_memory || !found_interrupt) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
DmaPnpAcquireDmaAdapter(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    DEVICE_DESCRIPTION desc;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    RtlZeroMemory(&desc, sizeof(desc));

    desc.Version = DEVICE_DESCRIPTION_VERSION;
    desc.Master = TRUE;
    desc.ScatterGather = TRUE;
    desc.Dma32BitAddresses = TRUE;
    desc.Dma64BitAddresses = TRUE;
    desc.InterfaceType = Internal;
    desc.MaximumLength = DMAPNP_COMMON_BUFFER_LENGTH;

    ext->NumberOfMapRegisters = 0;
    ext->DmaAdapter = IoGetDmaAdapter(
        ext->PhysicalDeviceObject,
        &desc,
        &ext->NumberOfMapRegisters
    );

    if (ext->DmaAdapter == NULL || ext->DmaAdapter->DmaOperations == NULL) {
        ext->DmaAdapter = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
DmaPnpAllocateCommonBuffer(_In_ PDEVICE_OBJECT DeviceObject)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->DmaAdapter == NULL || ext->DmaAdapter->DmaOperations == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (ext->DmaAdapter->DmaOperations->AllocateCommonBuffer == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    ext->CommonBufferLength = DMAPNP_COMMON_BUFFER_LENGTH;
    ext->CommonLogicalAddress.QuadPart = 0;

    ext->CommonBuffer = ext->DmaAdapter->DmaOperations->AllocateCommonBuffer(
        ext->DmaAdapter,
        ext->CommonBufferLength,
        &ext->CommonLogicalAddress,
        FALSE
    );

    if (ext->CommonBuffer == NULL) {
        ext->CommonBufferLength = 0;
        ext->CommonLogicalAddress.QuadPart = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ext->CommonBufferAllocated = TRUE;
    RtlZeroMemory(ext->CommonBuffer, ext->CommonBufferLength);

    return STATUS_SUCCESS;
}

static NTSTATUS
DmaPnpStartHardware(_In_ PDEVICE_OBJECT DeviceObject, _In_ PCM_RESOURCE_LIST ResourcesTranslated)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PHYSICAL_ADDRESS memory_start;
    ULONG memory_length;
    ULONG vector;
    KIRQL level;
    KAFFINITY affinity;
    KINTERRUPT_MODE mode;
    ULONG id;
    NTSTATUS status;
    POWER_STATE state;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Started != 0) {
        ext->Powered = 1;
        ext->DevicePowerState = PowerDeviceD0;
        return STATUS_SUCCESS;
    }

    memory_start.QuadPart = 0;
    memory_length = 0;
    vector = 0;
    level = 0;
    affinity = 0;
    mode = LevelSensitive;

    status = DmaPnpFindTranslatedResources(
        ResourcesTranslated,
        &memory_start,
        &memory_length,
        &vector,
        &level,
        &affinity,
        &mode
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext->MmioBase = (PUCHAR)MmMapIoSpace(memory_start, memory_length, MmNonCached);
    if (ext->MmioBase == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ext->MmioPhysicalStart = memory_start;
    ext->MmioLength = memory_length;
    ext->Mapped = TRUE;

    id = READ_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_ID));
    if (id != DMAPNP_ID_VALUE) {
        DmaPnpUnmapHardware(DeviceObject);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ext->InterruptVector = vector;
    ext->InterruptLevel = level;
    ext->InterruptAffinity = affinity;
    ext->InterruptMode = mode;

    status = IoConnectInterrupt(
        &ext->InterruptObject,
        DmaPnpInterruptService,
        DeviceObject,
        &ext->InterruptLock,
        vector,
        level,
        level,
        mode,
        FALSE,
        affinity,
        FALSE
    );

    if (!NT_SUCCESS(status)) {
        DmaPnpUnmapHardware(DeviceObject);
        return status;
    }

    ext->InterruptConnected = TRUE;

    status = DmaPnpAcquireDmaAdapter(DeviceObject);
    if (!NT_SUCCESS(status)) {
        DmaPnpDisconnectInterrupt(DeviceObject);
        DmaPnpUnmapHardware(DeviceObject);
        return status;
    }

    status = DmaPnpAllocateCommonBuffer(DeviceObject);
    if (!NT_SUCCESS(status)) {
        DmaPnpFreeDmaResources(DeviceObject);
        DmaPnpDisconnectInterrupt(DeviceObject);
        DmaPnpUnmapHardware(DeviceObject);
        return status;
    }

    ext->Started = 1;
    ext->Powered = 1;
    ext->SystemPowerState = PowerSystemWorking;
    ext->DevicePowerState = PowerDeviceD0;

    state.DeviceState = PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, state);

    return STATUS_SUCCESS;
}

static NTSTATUS
DmaPnpDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0 || ext->Powered == 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
DmaPnpDispatchUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return DmaPnpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
DmaPnpQueuePendingIrp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ DMAPNP_PENDING_KIND Kind
)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    KIRQL old_irql;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0 || ext->Powered == 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);

    if (ext->PendingIrp != NULL) {
        KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);
        return DmaPnpCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }

    IoMarkIrpPending(Irp);
    ext->PendingIrp = Irp;
    ext->PendingKind = Kind;

    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    return STATUS_PENDING;
}

static NTSTATUS
DmaPnpStartCommonRoundtrip(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    PDMAPNP_COMMON_ROUNDTRIP req;
    ULONG length;
    ULONG pattern;
    PUCHAR bytes;
    ULONG i;
    NTSTATUS status;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    if (buffer == NULL || in_len < sizeof(DMAPNP_COMMON_ROUNDTRIP) || out_len < sizeof(DMAPNP_COMMON_ROUNDTRIP)) {
        return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    if (!ext->CommonBufferAllocated || ext->CommonBuffer == NULL) {
        return DmaPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    req = (PDMAPNP_COMMON_ROUNDTRIP)buffer;

    length = req->Length;
    if (length == 0 || length > ext->CommonBufferLength) {
        return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    pattern = req->Pattern;
    bytes = (PUCHAR)ext->CommonBuffer;

    for (i = 0; i < length; i++) {
        bytes[i] = (UCHAR)(pattern + i);
    }

    req->Result = 0;
    req->InterruptCount = 0;
    req->LogicalAddressLow = ext->CommonLogicalAddress.LowPart;
    req->LogicalAddressHigh = (ULONG)ext->CommonLogicalAddress.HighPart;

    status = DmaPnpQueuePendingIrp(DeviceObject, Irp, DmaPnpPendingCommonRoundtrip);
    if (status != STATUS_PENDING) {
        return status;
    }

    WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_DMA_LO), ext->CommonLogicalAddress.LowPart);
    WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_DMA_HI), (ULONG)ext->CommonLogicalAddress.HighPart);
    WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_LENGTH), length);
    WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_COMMAND), DMAPNP_CMD_COMMON_INVERT);

    return STATUS_PENDING;
}

static NTSTATUS
DmaPnpMdlSelfTest(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PVOID buffer;
    PMDL mdl;
    PVOID mapped;
    ULONG length;
    PUCHAR bytes;
    ULONG i;
    ULONG checksum;
    void *out_buffer;
    PIO_STACK_LOCATION stack;
    ULONG out_len;

    UNREFERENCED_PARAMETER(DeviceObject);

    stack = IoGetCurrentIrpStackLocation(Irp);
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    out_buffer = Irp->AssociatedIrp.SystemBuffer;

    if (out_buffer == NULL || out_len < sizeof(ULONG)) {
        return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    length = 128;
    buffer = ExAllocatePoolWithTag(NonPagedPoolNx, length, DMAPNP_POOL_TAG);
    if (buffer == NULL) {
        return DmaPnpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    RtlZeroMemory(buffer, length);

    mdl = IoAllocateMdl(buffer, length, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        ExFreePoolWithTag(buffer, DMAPNP_POOL_TAG);
        return DmaPnpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    MmBuildMdlForNonPagedPool(mdl);

    mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (mapped == NULL) {
        IoFreeMdl(mdl);
        ExFreePoolWithTag(buffer, DMAPNP_POOL_TAG);
        return DmaPnpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    bytes = (PUCHAR)mapped;
    checksum = 0;

    for (i = 0; i < length; i++) {
        bytes[i] = (UCHAR)i;
        checksum += bytes[i];
    }

    IoFreeMdl(mdl);
    ExFreePoolWithTag(buffer, DMAPNP_POOL_TAG);

    *((ULONG *)out_buffer) = checksum;
    return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));
}

static NTSTATUS
DmaPnpDirectBufferFill(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG out_len;
    PVOID mapped;
    PUCHAR bytes;
    ULONG i;

    UNREFERENCED_PARAMETER(DeviceObject);

    stack = IoGetCurrentIrpStackLocation(Irp);
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;

    if (Irp->MdlAddress == NULL || out_len == 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    mapped = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (mapped == NULL) {
        return DmaPnpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    bytes = (PUCHAR)mapped;
    for (i = 0; i < out_len; i++) {
        bytes[i] = (UCHAR)(0xa5 + i);
    }

    return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, out_len);
}

static NTSTATUS
DmaPnpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    PDMAPNP_REG32_REQUEST reg;
    ULONG value;
    PDMAPNP_POWER_STATE_OUT power_out;
    PDMAPNP_DMA_INFO_OUT dma_out;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    if (ext->Removed != 0) {
        return DmaPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (code == IOCTL_DMAPNP_GET_POWER_STATE) {
        if (buffer == NULL || out_len < sizeof(DMAPNP_POWER_STATE_OUT)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        power_out = (PDMAPNP_POWER_STATE_OUT)buffer;
        power_out->Started = (ULONG)ext->Started;
        power_out->Powered = (ULONG)ext->Powered;
        power_out->SystemPowerState = (ULONG)ext->SystemPowerState;
        power_out->DevicePowerState = (ULONG)ext->DevicePowerState;
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(DMAPNP_POWER_STATE_OUT));
    }

    if (ext->Started == 0 || ext->Powered == 0 || !ext->Mapped) {
        return DmaPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    switch (code) {
    case IOCTL_DMAPNP_GET_ID:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = READ_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_ID));
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_DMAPNP_GET_DMA_INFO:
        if (buffer == NULL || out_len < sizeof(DMAPNP_DMA_INFO_OUT)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        dma_out = (PDMAPNP_DMA_INFO_OUT)buffer;
        dma_out->NumberOfMapRegisters = ext->NumberOfMapRegisters;
        dma_out->CommonBufferLength = ext->CommonBufferLength;
        dma_out->CommonLogicalAddressLow = ext->CommonLogicalAddress.LowPart;
        dma_out->CommonLogicalAddressHigh = (ULONG)ext->CommonLogicalAddress.HighPart;
        dma_out->CommonBufferAllocated = ext->CommonBufferAllocated ? 1 : 0;
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(DMAPNP_DMA_INFO_OUT));

    case IOCTL_DMAPNP_GET_INTERRUPT_COUNT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = (ULONG)ext->InterruptCount;
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_DMAPNP_READ_REG32:
        if (buffer == NULL || in_len < sizeof(DMAPNP_REG32_REQUEST) || out_len < sizeof(DMAPNP_REG32_REQUEST)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PDMAPNP_REG32_REQUEST)buffer;
        if (!DmaPnpValidReg32Offset(ext, reg->Offset)) {
            return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        reg->Value = READ_REGISTER_ULONG(DmaPnpReg32(ext, reg->Offset));
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(DMAPNP_REG32_REQUEST));

    case IOCTL_DMAPNP_WRITE_REG32:
        if (buffer == NULL || in_len < sizeof(DMAPNP_REG32_REQUEST)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PDMAPNP_REG32_REQUEST)buffer;
        if (!DmaPnpValidReg32Offset(ext, reg->Offset)) {
            return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        WRITE_REGISTER_ULONG(DmaPnpReg32(ext, reg->Offset), reg->Value);
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_DMAPNP_WRITE_CONTROL:
        if (buffer == NULL || in_len < sizeof(ULONG)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        value = *((ULONG *)buffer);
        WRITE_REGISTER_ULONG(DmaPnpReg32(ext, DMAPNP_REG_CONTROL), value);
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_DMAPNP_WAIT_FOR_INTERRUPT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return DmaPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        return DmaPnpQueuePendingIrp(DeviceObject, Irp, DmaPnpPendingWaitOnly);

    case IOCTL_DMAPNP_CANCEL_PENDING:
        DmaPnpCancelPending(DeviceObject, STATUS_CANCELLED);
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_DMAPNP_COMMON_BUFFER_ROUNDTRIP:
        return DmaPnpStartCommonRoundtrip(DeviceObject, Irp);

    case IOCTL_DMAPNP_MDL_SELF_TEST:
        return DmaPnpMdlSelfTest(DeviceObject, Irp);

    case IOCTL_DMAPNP_DIRECT_BUFFER_FILL:
        return DmaPnpDirectBufferFill(DeviceObject, Irp);

    case IOCTL_DMAPNP_PACKET_DMA_PLACEHOLDER:
    case IOCTL_DMAPNP_SCATTER_GATHER_PLACEHOLDER:
        return DmaPnpCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);

    default:
        return DmaPnpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS
DmaPnpHandleStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS lower_status;
    NTSTATUS status;

    lower_status = DmaPnpForwardIrpSynchronously(DeviceObject, Irp);
    if (!NT_SUCCESS(lower_status)) {
        return DmaPnpCompleteIrp(Irp, lower_status, 0);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);

    status = DmaPnpStartHardware(
        DeviceObject,
        stack->Parameters.StartDevice.AllocatedResourcesTranslated
    );

    return DmaPnpCompleteIrp(Irp, status, 0);
}

static NTSTATUS
DmaPnpHandleStopDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    NTSTATUS lower_status;

    DmaPnpStopHardware(DeviceObject);

    lower_status = DmaPnpForwardIrpSynchronously(DeviceObject, Irp);
    return DmaPnpCompleteIrp(Irp, lower_status, 0);
}

static NTSTATUS
DmaPnpHandleRemoveDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    NTSTATUS lower_status;
    UNICODE_STRING dos_name;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ext->Removed = 1;

    DmaPnpStopHardware(DeviceObject);

    if (ext->SymbolicLinkCreated) {
        RtlInitUnicodeString(&dos_name, DMAPNP_DOS_NAME);
        IoDeleteSymbolicLink(&dos_name);
        ext->SymbolicLinkCreated = FALSE;
    }

    lower_status = DmaPnpForwardIrpSynchronously(DeviceObject, Irp);

    Irp->IoStatus.Status = lower_status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (ext->LowerDeviceObject != NULL) {
        IoDetachDevice(ext->LowerDeviceObject);
        ext->LowerDeviceObject = NULL;
    }

    IoDeleteDevice(DeviceObject);

    return lower_status;
}

static NTSTATUS
DmaPnpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        return DmaPnpHandleStartDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_STOP_DEVICE:
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_STOP_DEVICE:
        return DmaPnpHandleStopDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
        return DmaPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_REMOVE_DEVICE:
        return DmaPnpHandleRemoveDevice(DeviceObject, Irp);

    default:
        return DmaPnpForwardIrpAndForget(DeviceObject, Irp);
    }
}

static NTSTATUS
DmaPnpHandleQueryPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    POWER_STATE_TYPE type;
    POWER_STATE state;
    NTSTATUS status;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    type = stack->Parameters.Power.Type;
    state = stack->Parameters.Power.State;

    if (ext->Removed != 0) {
        PoStartNextPowerIrp(Irp);
        return DmaPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (type == DevicePowerState) {
        if (state.DeviceState != PowerDeviceD0 &&
            state.DeviceState != PowerDeviceD1 &&
            state.DeviceState != PowerDeviceD2 &&
            state.DeviceState != PowerDeviceD3) {
            PoStartNextPowerIrp(Irp);
            return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else if (type == SystemPowerState) {
        if (state.SystemState <= PowerSystemUnspecified ||
            state.SystemState >= PowerSystemMaximum) {
            PoStartNextPowerIrp(Irp);
            return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else {
        PoStartNextPowerIrp(Irp);
        return DmaPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    status = DmaPnpForwardPowerIrpSynchronously(DeviceObject, Irp);

    PoStartNextPowerIrp(Irp);
    return DmaPnpCompleteIrp(Irp, status, 0);
}

static NTSTATUS
DmaPnpSetDevicePower(_In_ PDEVICE_OBJECT DeviceObject, _In_ DEVICE_POWER_STATE TargetState)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (TargetState == PowerDeviceD0) {
        ext->Powered = 1;
        ext->DevicePowerState = PowerDeviceD0;
        state.DeviceState = PowerDeviceD0;
        PoSetPowerState(DeviceObject, DevicePowerState, state);
        return STATUS_SUCCESS;
    }

    if (TargetState == PowerDeviceD1 || TargetState == PowerDeviceD2 || TargetState == PowerDeviceD3) {
        DmaPnpCancelPending(DeviceObject, STATUS_CANCELLED);

        ext->Powered = 0;
        ext->DevicePowerState = TargetState;
        state.DeviceState = TargetState;
        PoSetPowerState(DeviceObject, DevicePowerState, state);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
DmaPnpSetSystemPower(_In_ PDEVICE_OBJECT DeviceObject, _In_ SYSTEM_POWER_STATE TargetState)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (TargetState == PowerSystemWorking) {
        ext->SystemPowerState = PowerSystemWorking;
        state.SystemState = PowerSystemWorking;
        PoSetPowerState(DeviceObject, SystemPowerState, state);
        return DmaPnpSetDevicePower(DeviceObject, PowerDeviceD0);
    }

    if (TargetState > PowerSystemWorking && TargetState < PowerSystemMaximum) {
        ext->SystemPowerState = TargetState;
        state.SystemState = TargetState;
        PoSetPowerState(DeviceObject, SystemPowerState, state);
        return DmaPnpSetDevicePower(DeviceObject, PowerDeviceD3);
    }

    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
DmaPnpHandleSetPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PDMAPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    POWER_STATE_TYPE type;
    POWER_STATE state;
    NTSTATUS local_status;
    NTSTATUS lower_status;

    ext = (PDMAPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    type = stack->Parameters.Power.Type;
    state = stack->Parameters.Power.State;

    if (ext->Removed != 0) {
        PoStartNextPowerIrp(Irp);
        return DmaPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (type == DevicePowerState) {
        if (state.DeviceState == PowerDeviceD0) {
            lower_status = DmaPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
            if (NT_SUCCESS(lower_status)) {
                local_status = DmaPnpSetDevicePower(DeviceObject, PowerDeviceD0);
            } else {
                local_status = lower_status;
            }
        } else {
            local_status = DmaPnpSetDevicePower(DeviceObject, state.DeviceState);
            if (NT_SUCCESS(local_status)) {
                lower_status = DmaPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
                local_status = lower_status;
            }
        }
    } else if (type == SystemPowerState) {
        if (state.SystemState == PowerSystemWorking) {
            lower_status = DmaPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
            if (NT_SUCCESS(lower_status)) {
                local_status = DmaPnpSetSystemPower(DeviceObject, state.SystemState);
            } else {
                local_status = lower_status;
            }
        } else {
            local_status = DmaPnpSetSystemPower(DeviceObject, state.SystemState);
            if (NT_SUCCESS(local_status)) {
                lower_status = DmaPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
                local_status = lower_status;
            }
        }
    } else {
        local_status = STATUS_INVALID_PARAMETER;
    }

    PoStartNextPowerIrp(Irp);
    return DmaPnpCompleteIrp(Irp, local_status, 0);
}

static NTSTATUS
DmaPnpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_QUERY_POWER:
        return DmaPnpHandleQueryPower(DeviceObject, Irp);

    case IRP_MN_SET_POWER:
        return DmaPnpHandleSetPower(DeviceObject, Irp);

    case IRP_MN_WAIT_WAKE:
    case IRP_MN_POWER_SEQUENCE:
    default:
        return DmaPnpForwardPowerIrpAndForget(DeviceObject, Irp);
    }
}

static NTSTATUS
DmaPnpAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT fdo;
    PDMAPNP_DEVICE_EXTENSION ext;
    NTSTATUS status;

    fdo = NULL;

    RtlInitUnicodeString(&device_name, DMAPNP_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, DMAPNP_DOS_NAME);

    status = IoCreateDevice(
        DriverObject,
        sizeof(DMAPNP_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = (PDMAPNP_DEVICE_EXTENSION)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self = fdo;
    ext->PhysicalDeviceObject = PhysicalDeviceObject;
    ext->LowerDeviceObject = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (ext->LowerDeviceObject == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    ext->SystemPowerState = PowerSystemWorking;
    ext->DevicePowerState = PowerDeviceD3;
    ext->Powered = 0;

    KeInitializeSpinLock(&ext->InterruptLock);
    KeInitializeSpinLock(&ext->PendingIrpLock);
    KeInitializeDpc(&ext->Dpc, DmaPnpDpcRoutine, fdo);

    fdo->Flags |= DO_BUFFERED_IO;
    fdo->Flags |= DO_POWER_PAGABLE;

    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDetachDevice(ext->LowerDeviceObject);
        IoDeleteDevice(fdo);
        return status;
    }

    ext->SymbolicLinkCreated = TRUE;

    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static VOID
DmaPnpUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = DmaPnpDispatchUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DmaPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DmaPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DmaPnpDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = DmaPnpDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = DmaPnpDispatchPower;

    DriverObject->DriverExtension->AddDevice = DmaPnpAddDevice;
    DriverObject->DriverUnload = DmaPnpUnload;

    return STATUS_SUCCESS;
}
