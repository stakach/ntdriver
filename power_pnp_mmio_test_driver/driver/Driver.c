/*
 * PowerPnpMmioTest - tiny legacy WDM PnP + Power function driver for userspace-ntos.
 *
 * Purpose:
 *   - PnP-style WDM driver
 *   - No WDF/KMDF
 *   - No DMA
 *   - DriverEntry sets DriverExtension->AddDevice
 *   - AddDevice creates FDO and attaches to PDO
 *   - IRP_MN_START_DEVICE receives translated resources
 *   - START_DEVICE maps MMIO and connects interrupt
 *   - IRP_MJ_POWER handles QUERY_POWER and SET_POWER
 *   - DevicePower D0 enables IOCTL/MMIO/interrupt path
 *   - DevicePower D3 disables IOCTL/MMIO/interrupt path
 *   - IRP_MN_REMOVE_DEVICE disconnects/unmaps/detaches/deletes
 *
 * Expected NT objects created by this driver:
 *   \Device\PowerPnpMmioTest
 *   \DosDevices\PowerPnpMmioTest -> \Device\PowerPnpMmioTest
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
 * Simulated register layout:
 *   0x00 ID register, readonly, expected 0x4d4d494f ("MMIO")
 *   0x04 control register, read/write
 *   0x08 status register, bit0 = interrupt pending
 *   0x0c interrupt ack register, write 1 clears pending
 *   0x10 interrupt count register, optional simulated-device counter
 */

#include <ntddk.h>
#include "PowerPnpMmioTest.h"

#define POWERPNP_DEVICE_NAME   L"\\Device\\PowerPnpMmioTest"
#define POWERPNP_DOS_NAME      L"\\DosDevices\\PowerPnpMmioTest"

#define POWERPNP_REG_ID        0x00
#define POWERPNP_REG_CONTROL   0x04
#define POWERPNP_REG_STATUS    0x08
#define POWERPNP_REG_ACK       0x0c
#define POWERPNP_REG_IRQ_COUNT 0x10

#define POWERPNP_STATUS_INTERRUPT_PENDING 0x00000001UL

typedef struct _POWERPNP_DEVICE_EXTENSION {
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

    KDPC Dpc;
    KSPIN_LOCK PendingIrpLock;
    PIRP PendingInterruptIrp;

    volatile LONG InterruptCount;
    volatile LONG Started;
    volatile LONG Powered;
    volatile LONG Removed;

    SYSTEM_POWER_STATE SystemPowerState;
    DEVICE_POWER_STATE DevicePowerState;

    BOOLEAN SymbolicLinkCreated;
    BOOLEAN Mapped;
    BOOLEAN InterruptConnected;
} POWERPNP_DEVICE_EXTENSION, *PPOWERPNP_DEVICE_EXTENSION;

static __inline PULONG
PowerPnpReg32(_In_ PPOWERPNP_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
{
    return (PULONG)(Ext->MmioBase + Offset);
}

static NTSTATUS
PowerPnpCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN
PowerPnpValidReg32Offset(_In_ PPOWERPNP_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
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
PowerPnpInterruptService(_In_ PKINTERRUPT Interrupt, _In_opt_ PVOID ServiceContext)
{
    PDEVICE_OBJECT device;
    PPOWERPNP_DEVICE_EXTENSION ext;
    ULONG status;

    UNREFERENCED_PARAMETER(Interrupt);

    device = (PDEVICE_OBJECT)ServiceContext;
    ext = (PPOWERPNP_DEVICE_EXTENSION)device->DeviceExtension;

    if (ext->Removed != 0 || ext->Started == 0 || ext->Powered == 0 || !ext->Mapped) {
        return FALSE;
    }

    status = READ_REGISTER_ULONG(PowerPnpReg32(ext, POWERPNP_REG_STATUS));
    if ((status & POWERPNP_STATUS_INTERRUPT_PENDING) == 0) {
        return FALSE;
    }

    WRITE_REGISTER_ULONG(PowerPnpReg32(ext, POWERPNP_REG_ACK), 1);

    InterlockedIncrement(&ext->InterruptCount);
    KeInsertQueueDpc(&ext->Dpc, NULL, NULL);

    return TRUE;
}

static VOID
PowerPnpDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PDEVICE_OBJECT device;
    PPOWERPNP_DEVICE_EXTENSION ext;
    PIRP irp;
    KIRQL old_irql;
    ULONG count;
    void *buffer;
    PIO_STACK_LOCATION stack;
    ULONG out_len;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    device = (PDEVICE_OBJECT)DeferredContext;
    ext = (PPOWERPNP_DEVICE_EXTENSION)device->DeviceExtension;

    irp = NULL;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingInterruptIrp != NULL) {
        irp = ext->PendingInterruptIrp;
        ext->PendingInterruptIrp = NULL;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (irp == NULL) {
        return;
    }

    count = (ULONG)ext->InterruptCount;

    stack = IoGetCurrentIrpStackLocation(irp);
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = irp->AssociatedIrp.SystemBuffer;

    if (buffer != NULL && out_len >= sizeof(ULONG)) {
        *((ULONG *)buffer) = count;
        PowerPnpCompleteIrp(irp, STATUS_SUCCESS, sizeof(ULONG));
    } else {
        PowerPnpCompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
}

static NTSTATUS
PowerPnpCompletionRoutine(
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
PowerPnpForwardIrpSynchronously(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    KEVENT event;
    NTSTATUS status;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        PowerPnpCompletionRoutine,
        &event,
        TRUE,
        TRUE,
        TRUE
    );

    status = IoCallDriver(ext->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

static NTSTATUS
PowerPnpForwardPowerIrpSynchronously(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    KEVENT event;
    NTSTATUS status;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        PowerPnpCompletionRoutine,
        &event,
        TRUE,
        TRUE,
        TRUE
    );

    status = PoCallDriver(ext->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

static NTSTATUS
PowerPnpForwardIrpAndForget(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDeviceObject, Irp);
}

static NTSTATUS
PowerPnpForwardPowerIrpAndForget(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);

    if (ext->LowerDeviceObject == NULL) {
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDeviceObject, Irp);
}

static VOID
PowerPnpCancelPendingWait(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    PIRP pending;
    KIRQL old_irql;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    pending = NULL;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingInterruptIrp != NULL) {
        pending = ext->PendingInterruptIrp;
        ext->PendingInterruptIrp = NULL;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (pending != NULL) {
        PowerPnpCompleteIrp(pending, STATUS_CANCELLED, 0);
    }
}

static VOID
PowerPnpDisconnectInterrupt(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPOWERPNP_DEVICE_EXTENSION ext;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject = NULL;
        ext->InterruptConnected = FALSE;
    }
}

static VOID
PowerPnpUnmapHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPOWERPNP_DEVICE_EXTENSION ext;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Mapped && ext->MmioBase != NULL) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
        ext->Mapped = FALSE;
    }
}

static VOID
PowerPnpStopHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    PowerPnpCancelPendingWait(DeviceObject);

    ext->Powered = 0;
    ext->DevicePowerState = PowerDeviceD3;
    state.DeviceState = PowerDeviceD3;
    PoSetPowerState(DeviceObject, DevicePowerState, state);

    PowerPnpDisconnectInterrupt(DeviceObject);
    PowerPnpUnmapHardware(DeviceObject);

    ext->Started = 0;
}

static NTSTATUS
PowerPnpFindTranslatedResources(
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
PowerPnpStartHardware(_In_ PDEVICE_OBJECT DeviceObject, _In_ PCM_RESOURCE_LIST ResourcesTranslated)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    PHYSICAL_ADDRESS memory_start;
    ULONG memory_length;
    ULONG vector;
    KIRQL level;
    KAFFINITY affinity;
    KINTERRUPT_MODE mode;
    ULONG id;
    NTSTATUS status;
    POWER_STATE state;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

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

    status = PowerPnpFindTranslatedResources(
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

    id = READ_REGISTER_ULONG(PowerPnpReg32(ext, POWERPNP_REG_ID));
    if (id != POWERPNP_ID_VALUE) {
        PowerPnpUnmapHardware(DeviceObject);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ext->InterruptVector = vector;
    ext->InterruptLevel = level;
    ext->InterruptAffinity = affinity;
    ext->InterruptMode = mode;

    status = IoConnectInterrupt(
        &ext->InterruptObject,
        PowerPnpInterruptService,
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
        PowerPnpUnmapHardware(DeviceObject);
        return status;
    }

    ext->InterruptConnected = TRUE;
    ext->Started = 1;
    ext->Powered = 1;
    ext->SystemPowerState = PowerSystemWorking;
    ext->DevicePowerState = PowerDeviceD0;

    state.DeviceState = PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, state);

    return STATUS_SUCCESS;
}

static NTSTATUS
PowerPnpDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return PowerPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0 || ext->Powered == 0) {
        return PowerPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
PowerPnpDispatchUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return PowerPnpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
PowerPnpWaitForInterrupt(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    KIRQL old_irql;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return PowerPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0 || ext->Powered == 0) {
        return PowerPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);

    if (ext->PendingInterruptIrp != NULL) {
        KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);
        return PowerPnpCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }

    IoMarkIrpPending(Irp);
    ext->PendingInterruptIrp = Irp;

    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    return STATUS_PENDING;
}

static NTSTATUS
PowerPnpCancelWaitIoctl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PowerPnpCancelPendingWait(DeviceObject);
    return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
PowerPnpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    PPOWERPNP_REG32_REQUEST reg;
    ULONG value;
    POWERPNP_POWER_STATE_OUT *power_out;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    if (ext->Removed != 0) {
        return PowerPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (code == IOCTL_POWERPNP_GET_POWER_STATE) {
        if (buffer == NULL || out_len < sizeof(POWERPNP_POWER_STATE_OUT)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        power_out = (POWERPNP_POWER_STATE_OUT *)buffer;
        power_out->Started = (ULONG)ext->Started;
        power_out->Powered = (ULONG)ext->Powered;
        power_out->SystemPowerState = (ULONG)ext->SystemPowerState;
        power_out->DevicePowerState = (ULONG)ext->DevicePowerState;
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(POWERPNP_POWER_STATE_OUT));
    }

    if (ext->Started == 0 || ext->Powered == 0 || !ext->Mapped) {
        return PowerPnpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    switch (code) {
    case IOCTL_POWERPNP_GET_ID:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = READ_REGISTER_ULONG(PowerPnpReg32(ext, POWERPNP_REG_ID));
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_POWERPNP_GET_INTERRUPT_COUNT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = (ULONG)ext->InterruptCount;
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_POWERPNP_READ_REG32:
        if (buffer == NULL || in_len < sizeof(POWERPNP_REG32_REQUEST) || out_len < sizeof(POWERPNP_REG32_REQUEST)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PPOWERPNP_REG32_REQUEST)buffer;
        if (!PowerPnpValidReg32Offset(ext, reg->Offset)) {
            return PowerPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        reg->Value = READ_REGISTER_ULONG(PowerPnpReg32(ext, reg->Offset));
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(POWERPNP_REG32_REQUEST));

    case IOCTL_POWERPNP_WRITE_REG32:
        if (buffer == NULL || in_len < sizeof(POWERPNP_REG32_REQUEST)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PPOWERPNP_REG32_REQUEST)buffer;
        if (!PowerPnpValidReg32Offset(ext, reg->Offset)) {
            return PowerPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        WRITE_REGISTER_ULONG(PowerPnpReg32(ext, reg->Offset), reg->Value);
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_POWERPNP_WRITE_CONTROL:
        if (buffer == NULL || in_len < sizeof(ULONG)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        value = *((ULONG *)buffer);
        WRITE_REGISTER_ULONG(PowerPnpReg32(ext, POWERPNP_REG_CONTROL), value);
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_POWERPNP_WAIT_FOR_INTERRUPT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PowerPnpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        return PowerPnpWaitForInterrupt(DeviceObject, Irp);

    case IOCTL_POWERPNP_CANCEL_WAIT:
        return PowerPnpCancelWaitIoctl(DeviceObject, Irp);

    default:
        return PowerPnpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS
PowerPnpHandleStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS lower_status;
    NTSTATUS status;

    lower_status = PowerPnpForwardIrpSynchronously(DeviceObject, Irp);
    if (!NT_SUCCESS(lower_status)) {
        return PowerPnpCompleteIrp(Irp, lower_status, 0);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);

    status = PowerPnpStartHardware(
        DeviceObject,
        stack->Parameters.StartDevice.AllocatedResourcesTranslated
    );

    return PowerPnpCompleteIrp(Irp, status, 0);
}

static NTSTATUS
PowerPnpHandleStopDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    NTSTATUS lower_status;

    PowerPnpStopHardware(DeviceObject);

    lower_status = PowerPnpForwardIrpSynchronously(DeviceObject, Irp);
    return PowerPnpCompleteIrp(Irp, lower_status, 0);
}

static NTSTATUS
PowerPnpHandleRemoveDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    NTSTATUS lower_status;
    UNICODE_STRING dos_name;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ext->Removed = 1;

    PowerPnpStopHardware(DeviceObject);

    if (ext->SymbolicLinkCreated) {
        RtlInitUnicodeString(&dos_name, POWERPNP_DOS_NAME);
        IoDeleteSymbolicLink(&dos_name);
        ext->SymbolicLinkCreated = FALSE;
    }

    lower_status = PowerPnpForwardIrpSynchronously(DeviceObject, Irp);

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
PowerPnpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        return PowerPnpHandleStartDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_STOP_DEVICE:
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_STOP_DEVICE:
        return PowerPnpHandleStopDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
        return PowerPnpCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_REMOVE_DEVICE:
        return PowerPnpHandleRemoveDevice(DeviceObject, Irp);

    default:
        return PowerPnpForwardIrpAndForget(DeviceObject, Irp);
    }
}

static NTSTATUS
PowerPnpHandleQueryPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    POWER_STATE_TYPE type;
    POWER_STATE state;
    NTSTATUS status;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    type = stack->Parameters.Power.Type;
    state = stack->Parameters.Power.State;

    if (ext->Removed != 0) {
        PoStartNextPowerIrp(Irp);
        return PowerPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (type == DevicePowerState) {
        if (state.DeviceState != PowerDeviceD0 &&
            state.DeviceState != PowerDeviceD1 &&
            state.DeviceState != PowerDeviceD2 &&
            state.DeviceState != PowerDeviceD3) {
            PoStartNextPowerIrp(Irp);
            return PowerPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else if (type == SystemPowerState) {
        if (state.SystemState <= PowerSystemUnspecified ||
            state.SystemState >= PowerSystemMaximum) {
            PoStartNextPowerIrp(Irp);
            return PowerPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else {
        PoStartNextPowerIrp(Irp);
        return PowerPnpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    status = PowerPnpForwardPowerIrpSynchronously(DeviceObject, Irp);

    PoStartNextPowerIrp(Irp);
    return PowerPnpCompleteIrp(Irp, status, 0);
}

static NTSTATUS
PowerPnpSetDevicePower(_In_ PDEVICE_OBJECT DeviceObject, _In_ DEVICE_POWER_STATE TargetState)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (TargetState == PowerDeviceD0) {
        ext->Powered = 1;
        ext->DevicePowerState = PowerDeviceD0;
        state.DeviceState = PowerDeviceD0;
        PoSetPowerState(DeviceObject, DevicePowerState, state);
        return STATUS_SUCCESS;
    }

    if (TargetState == PowerDeviceD1 || TargetState == PowerDeviceD2 || TargetState == PowerDeviceD3) {
        /*
         * v0.1 collapses D1/D2/D3 into "not usable". Keep mappings and interrupt
         * connection intact; HAL/Power Manager should also gate interrupt delivery
         * while not D0.
         */
        PowerPnpCancelPendingWait(DeviceObject);

        ext->Powered = 0;
        ext->DevicePowerState = TargetState;
        state.DeviceState = TargetState;
        PoSetPowerState(DeviceObject, DevicePowerState, state);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
PowerPnpSetSystemPower(_In_ PDEVICE_OBJECT DeviceObject, _In_ SYSTEM_POWER_STATE TargetState)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    POWER_STATE state;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (TargetState == PowerSystemWorking) {
        ext->SystemPowerState = PowerSystemWorking;
        state.SystemState = PowerSystemWorking;
        PoSetPowerState(DeviceObject, SystemPowerState, state);
        return PowerPnpSetDevicePower(DeviceObject, PowerDeviceD0);
    }

    if (TargetState > PowerSystemWorking && TargetState < PowerSystemMaximum) {
        ext->SystemPowerState = TargetState;
        state.SystemState = TargetState;
        PoSetPowerState(DeviceObject, SystemPowerState, state);
        return PowerPnpSetDevicePower(DeviceObject, PowerDeviceD3);
    }

    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS
PowerPnpHandleSetPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPOWERPNP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    POWER_STATE_TYPE type;
    POWER_STATE state;
    NTSTATUS local_status;
    NTSTATUS lower_status;

    ext = (PPOWERPNP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    type = stack->Parameters.Power.Type;
    state = stack->Parameters.Power.State;

    if (ext->Removed != 0) {
        PoStartNextPowerIrp(Irp);
        return PowerPnpCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (type == DevicePowerState) {
        if (state.DeviceState == PowerDeviceD0) {
            /*
             * On power-up, let lower stack see the transition first, then mark
             * the function device usable.
             */
            lower_status = PowerPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
            if (NT_SUCCESS(lower_status)) {
                local_status = PowerPnpSetDevicePower(DeviceObject, PowerDeviceD0);
            } else {
                local_status = lower_status;
            }
        } else {
            /*
             * On power-down, quiesce this driver first, then pass the transition
             * down. This cancels pending interrupt waits before D3.
             */
            local_status = PowerPnpSetDevicePower(DeviceObject, state.DeviceState);
            if (NT_SUCCESS(local_status)) {
                lower_status = PowerPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
                local_status = lower_status;
            }
        }
    } else if (type == SystemPowerState) {
        /*
         * Simple v0.1 policy: system working maps to D0, any sleep/shutdown-like
         * state maps to D3 locally.
         */
        if (state.SystemState == PowerSystemWorking) {
            lower_status = PowerPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
            if (NT_SUCCESS(lower_status)) {
                local_status = PowerPnpSetSystemPower(DeviceObject, state.SystemState);
            } else {
                local_status = lower_status;
            }
        } else {
            local_status = PowerPnpSetSystemPower(DeviceObject, state.SystemState);
            if (NT_SUCCESS(local_status)) {
                lower_status = PowerPnpForwardPowerIrpSynchronously(DeviceObject, Irp);
                local_status = lower_status;
            }
        }
    } else {
        local_status = STATUS_INVALID_PARAMETER;
    }

    PoStartNextPowerIrp(Irp);
    return PowerPnpCompleteIrp(Irp, local_status, 0);
}

static NTSTATUS
PowerPnpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_QUERY_POWER:
        return PowerPnpHandleQueryPower(DeviceObject, Irp);

    case IRP_MN_SET_POWER:
        return PowerPnpHandleSetPower(DeviceObject, Irp);

    case IRP_MN_WAIT_WAKE:
    case IRP_MN_POWER_SEQUENCE:
    default:
        return PowerPnpForwardPowerIrpAndForget(DeviceObject, Irp);
    }
}

static NTSTATUS
PowerPnpAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT fdo;
    PPOWERPNP_DEVICE_EXTENSION ext;
    NTSTATUS status;

    fdo = NULL;

    RtlInitUnicodeString(&device_name, POWERPNP_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, POWERPNP_DOS_NAME);

    status = IoCreateDevice(
        DriverObject,
        sizeof(POWERPNP_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = (PPOWERPNP_DEVICE_EXTENSION)fdo->DeviceExtension;
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
    KeInitializeDpc(&ext->Dpc, PowerPnpDpcRoutine, fdo);

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
PowerPnpUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    /*
     * PnP cleanup should happen through IRP_MN_REMOVE_DEVICE.
     * Nothing global to release here.
     */
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = PowerPnpDispatchUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = PowerPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PowerPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PowerPnpDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = PowerPnpDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = PowerPnpDispatchPower;

    DriverObject->DriverExtension->AddDevice = PowerPnpAddDevice;
    DriverObject->DriverUnload = PowerPnpUnload;

    return STATUS_SUCCESS;
}
