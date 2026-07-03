/*
 * PnpMmioInterruptTest - tiny legacy WDM PnP function driver for userspace-ntos.
 *
 * Purpose:
 *   - PnP-style WDM driver
 *   - No WDF/KMDF
 *   - No DMA
 *   - No real bus enumeration
 *   - DriverEntry sets DriverExtension->AddDevice
 *   - AddDevice creates an FDO and attaches to the supplied PDO
 *   - IRP_MN_START_DEVICE receives raw/translated CM_RESOURCE_LISTs
 *   - START_DEVICE maps MMIO and connects interrupt from translated resources
 *   - DEVICE_CONTROL works only after START_DEVICE succeeds
 *   - IRP_MN_REMOVE_DEVICE disconnects/unmaps/detaches/deletes
 *
 * Expected NT objects created by this driver:
 *   \Device\PnpMmioInterruptTest
 *   \DosDevices\PnpMmioInterruptTest -> \Device\PnpMmioInterruptTest
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
#include "PnpMmioInterruptTest.h"

#define PNPIT_DEVICE_NAME   L"\\Device\\PnpMmioInterruptTest"
#define PNPIT_DOS_NAME      L"\\DosDevices\\PnpMmioInterruptTest"

#define PNPIT_REG_ID        0x00
#define PNPIT_REG_CONTROL   0x04
#define PNPIT_REG_STATUS    0x08
#define PNPIT_REG_ACK       0x0c
#define PNPIT_REG_IRQ_COUNT 0x10

#define PNPIT_STATUS_INTERRUPT_PENDING 0x00000001UL

typedef struct _PNPIT_DEVICE_EXTENSION {
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
    volatile LONG Removed;

    BOOLEAN SymbolicLinkCreated;
    BOOLEAN Mapped;
    BOOLEAN InterruptConnected;
} PNPIT_DEVICE_EXTENSION, *PPNPIT_DEVICE_EXTENSION;

static __inline PULONG
PnpitReg32(_In_ PPNPIT_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
{
    return (PULONG)(Ext->MmioBase + Offset);
}

static NTSTATUS
PnpitCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN
PnpitValidReg32Offset(_In_ PPNPIT_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
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
PnpitInterruptService(_In_ PKINTERRUPT Interrupt, _In_opt_ PVOID ServiceContext)
{
    PDEVICE_OBJECT device;
    PPNPIT_DEVICE_EXTENSION ext;
    ULONG status;

    UNREFERENCED_PARAMETER(Interrupt);

    device = (PDEVICE_OBJECT)ServiceContext;
    ext = (PPNPIT_DEVICE_EXTENSION)device->DeviceExtension;

    if (ext->Removed != 0 || ext->Started == 0 || !ext->Mapped) {
        return FALSE;
    }

    status = READ_REGISTER_ULONG(PnpitReg32(ext, PNPIT_REG_STATUS));
    if ((status & PNPIT_STATUS_INTERRUPT_PENDING) == 0) {
        return FALSE;
    }

    WRITE_REGISTER_ULONG(PnpitReg32(ext, PNPIT_REG_ACK), 1);

    InterlockedIncrement(&ext->InterruptCount);
    KeInsertQueueDpc(&ext->Dpc, NULL, NULL);

    return TRUE;
}

static VOID
PnpitDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PDEVICE_OBJECT device;
    PPNPIT_DEVICE_EXTENSION ext;
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
    ext = (PPNPIT_DEVICE_EXTENSION)device->DeviceExtension;

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
        PnpitCompleteIrp(irp, STATUS_SUCCESS, sizeof(ULONG));
    } else {
        PnpitCompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
}

static NTSTATUS
PnpitPnpCompletionRoutine(
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

    /*
     * Stop completion processing so the caller can inspect the lower status and
     * then complete the IRP itself.
     */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
PnpitForwardIrpSynchronously(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;
    KEVENT event;
    NTSTATUS status;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        PnpitPnpCompletionRoutine,
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
PnpitForwardIrpAndForget(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->LowerDeviceObject == NULL) {
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDeviceObject, Irp);
}

static VOID
PnpitCancelPendingWait(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPNPIT_DEVICE_EXTENSION ext;
    PIRP pending;
    KIRQL old_irql;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    pending = NULL;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingInterruptIrp != NULL) {
        pending = ext->PendingInterruptIrp;
        ext->PendingInterruptIrp = NULL;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (pending != NULL) {
        PnpitCompleteIrp(pending, STATUS_CANCELLED, 0);
    }
}

static VOID
PnpitDisconnectInterrupt(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPNPIT_DEVICE_EXTENSION ext;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject = NULL;
        ext->InterruptConnected = FALSE;
    }
}

static VOID
PnpitUnmapHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PPNPIT_DEVICE_EXTENSION ext;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Mapped && ext->MmioBase != NULL) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
        ext->Mapped = FALSE;
    }
}

static VOID
PnpitStopHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PnpitCancelPendingWait(DeviceObject);
    PnpitDisconnectInterrupt(DeviceObject);
    PnpitUnmapHardware(DeviceObject);

    ((PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->Started = 0;
}

static NTSTATUS
PnpitFindTranslatedResources(
    _In_ PCM_RESOURCE_LIST ResourcesTranslated,
    _Out_ PHYSICAL_ADDRESS *MemoryStart,
    _Out_ PULONG MemoryLength,
    _Out_ PULONG Vector,
    _Out_ PKIRQL Level,
    _Out_ PKAFFINITY Affinity,
    _Out_ PKINTERRUPT_MODE Mode
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
PnpitStartHardware(_In_ PDEVICE_OBJECT DeviceObject, _In_ PCM_RESOURCE_LIST ResourcesTranslated)
{
    PPNPIT_DEVICE_EXTENSION ext;
    PHYSICAL_ADDRESS memory_start;
    ULONG memory_length;
    ULONG vector;
    KIRQL level;
    KAFFINITY affinity;
    KINTERRUPT_MODE mode;
    ULONG id;
    NTSTATUS status;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Started != 0) {
        return STATUS_SUCCESS;
    }

    memory_start.QuadPart = 0;
    memory_length = 0;
    vector = 0;
    level = 0;
    affinity = 0;
    mode = LevelSensitive;

    status = PnpitFindTranslatedResources(
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

    id = READ_REGISTER_ULONG(PnpitReg32(ext, PNPIT_REG_ID));
    if (id != PNPIT_ID_VALUE) {
        PnpitUnmapHardware(DeviceObject);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ext->InterruptVector = vector;
    ext->InterruptLevel = level;
    ext->InterruptAffinity = affinity;
    ext->InterruptMode = mode;

    status = IoConnectInterrupt(
        &ext->InterruptObject,
        PnpitInterruptService,
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
        PnpitUnmapHardware(DeviceObject);
        return status;
    }

    ext->InterruptConnected = TRUE;
    ext->Started = 1;

    return STATUS_SUCCESS;
}

static NTSTATUS
PnpitDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return PnpitCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0) {
        return PnpitCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
PnpitDispatchUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return PnpitCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
PnpitWaitForInterrupt(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;
    KIRQL old_irql;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Removed != 0) {
        return PnpitCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0) {
        return PnpitCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);

    if (ext->PendingInterruptIrp != NULL) {
        KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);
        return PnpitCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }

    IoMarkIrpPending(Irp);
    ext->PendingInterruptIrp = Irp;

    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    return STATUS_PENDING;
}

static NTSTATUS
PnpitCancelWaitIoctl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PnpitCancelPendingWait(DeviceObject);
    return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
PnpitDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    PPNPIT_REG32_REQUEST reg;
    ULONG value;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    if (ext->Removed != 0) {
        return PnpitCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    if (ext->Started == 0 || !ext->Mapped) {
        return PnpitCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    switch (code) {
    case IOCTL_PNPIT_GET_ID:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = READ_REGISTER_ULONG(PnpitReg32(ext, PNPIT_REG_ID));
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_PNPIT_GET_INTERRUPT_COUNT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = (ULONG)ext->InterruptCount;
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_PNPIT_READ_REG32:
        if (buffer == NULL || in_len < sizeof(PNPIT_REG32_REQUEST) || out_len < sizeof(PNPIT_REG32_REQUEST)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PPNPIT_REG32_REQUEST)buffer;
        if (!PnpitValidReg32Offset(ext, reg->Offset)) {
            return PnpitCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        reg->Value = READ_REGISTER_ULONG(PnpitReg32(ext, reg->Offset));
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(PNPIT_REG32_REQUEST));

    case IOCTL_PNPIT_WRITE_REG32:
        if (buffer == NULL || in_len < sizeof(PNPIT_REG32_REQUEST)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PPNPIT_REG32_REQUEST)buffer;
        if (!PnpitValidReg32Offset(ext, reg->Offset)) {
            return PnpitCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        WRITE_REGISTER_ULONG(PnpitReg32(ext, reg->Offset), reg->Value);
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_PNPIT_WRITE_CONTROL:
        if (buffer == NULL || in_len < sizeof(ULONG)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        value = *((ULONG *)buffer);
        WRITE_REGISTER_ULONG(PnpitReg32(ext, PNPIT_REG_CONTROL), value);
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_PNPIT_WAIT_FOR_INTERRUPT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return PnpitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        return PnpitWaitForInterrupt(DeviceObject, Irp);

    case IOCTL_PNPIT_CANCEL_WAIT:
        return PnpitCancelWaitIoctl(DeviceObject, Irp);

    default:
        return PnpitCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS
PnpitHandleStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS lower_status;
    NTSTATUS status;

    lower_status = PnpitForwardIrpSynchronously(DeviceObject, Irp);
    if (!NT_SUCCESS(lower_status)) {
        return PnpitCompleteIrp(Irp, lower_status, 0);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);

    status = PnpitStartHardware(
        DeviceObject,
        stack->Parameters.StartDevice.AllocatedResourcesTranslated
    );

    return PnpitCompleteIrp(Irp, status, 0);
}

static NTSTATUS
PnpitHandleStopDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    NTSTATUS lower_status;

    PnpitStopHardware(DeviceObject);

    lower_status = PnpitForwardIrpSynchronously(DeviceObject, Irp);
    return PnpitCompleteIrp(Irp, lower_status, 0);
}

static NTSTATUS
PnpitHandleRemoveDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PPNPIT_DEVICE_EXTENSION ext;
    NTSTATUS lower_status;
    UNICODE_STRING dos_name;

    ext = (PPNPIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ext->Removed = 1;

    PnpitStopHardware(DeviceObject);

    if (ext->SymbolicLinkCreated) {
        RtlInitUnicodeString(&dos_name, PNPIT_DOS_NAME);
        IoDeleteSymbolicLink(&dos_name);
        ext->SymbolicLinkCreated = FALSE;
    }

    lower_status = PnpitForwardIrpSynchronously(DeviceObject, Irp);

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
PnpitDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;

    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        return PnpitHandleStartDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_STOP_DEVICE:
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_STOP_DEVICE:
        return PnpitHandleStopDevice(DeviceObject, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
        return PnpitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_REMOVE_DEVICE:
        return PnpitHandleRemoveDevice(DeviceObject, Irp);

    default:
        return PnpitForwardIrpAndForget(DeviceObject, Irp);
    }
}

static NTSTATUS
PnpitAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT fdo;
    PPNPIT_DEVICE_EXTENSION ext;
    NTSTATUS status;

    fdo = NULL;

    RtlInitUnicodeString(&device_name, PNPIT_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, PNPIT_DOS_NAME);

    status = IoCreateDevice(
        DriverObject,
        sizeof(PNPIT_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = (PPNPIT_DEVICE_EXTENSION)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self = fdo;
    ext->PhysicalDeviceObject = PhysicalDeviceObject;
    ext->LowerDeviceObject = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (ext->LowerDeviceObject == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    KeInitializeSpinLock(&ext->InterruptLock);
    KeInitializeSpinLock(&ext->PendingIrpLock);
    KeInitializeDpc(&ext->Dpc, PnpitDpcRoutine, fdo);

    fdo->Flags |= DO_BUFFERED_IO;

    /*
     * The symbolic link is created during AddDevice to make lookup simple.
     * CREATE/CLOSE still fail with STATUS_DEVICE_NOT_READY until START_DEVICE
     * succeeds.
     */
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
PnpitUnload(_In_ PDRIVER_OBJECT DriverObject)
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
        DriverObject->MajorFunction[i] = PnpitDispatchUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = PnpitDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PnpitDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PnpitDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpitDispatchPnp;

    DriverObject->DriverExtension->AddDevice = PnpitAddDevice;
    DriverObject->DriverUnload = PnpitUnload;

    return STATUS_SUCCESS;
}
