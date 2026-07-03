/*
 * MmioInterruptTest - tiny legacy WDM driver for userspace-ntos HAL testing.
 *
 * Purpose:
 *   - No PnP
 *   - No WDF/KMDF
 *   - No DMA
 *   - Uses hard-coded static "translated" resources supplied by userspace-ntos
 *   - Maps one fake MMIO region with MmMapIoSpace
 *   - Accesses registers with READ_REGISTER_ULONG / WRITE_REGISTER_ULONG
 *   - Connects one fake interrupt with IoConnectInterrupt
 *   - ISR acknowledges interrupt and queues a DPC
 *   - DPC completes one pending IOCTL IRP
 *
 * Expected NT objects:
 *   \Device\MmioInterruptTest
 *   \DosDevices\MmioInterruptTest -> \Device\MmioInterruptTest
 *
 * Expected userspace-ntos static resources:
 *   MMIO translated base: 0x10000000
 *   MMIO length:          0x1000
 *   Interrupt vector:     5
 *   Interrupt IRQL:       5
 *   Interrupt mode:       LevelSensitive
 *   Share vector:         FALSE
 *   Affinity:             1
 *
 * Simulated register layout:
 *   0x00 ID register, readonly, expected 0x4d4d494f ("MMIO")
 *   0x04 control register, read/write
 *   0x08 status register, bit0 = interrupt pending
 *   0x0c interrupt ack register, write 1 clears pending
 *   0x10 interrupt count register, optional simulated-device counter
 */

#include <ntddk.h>
#include "MmioInterruptTest.h"

#define MMIOIT_DEVICE_NAME   L"\\Device\\MmioInterruptTest"
#define MMIOIT_DOS_NAME      L"\\DosDevices\\MmioInterruptTest"

#define MMIOIT_PHYS_BASE     0x10000000ULL
#define MMIOIT_MMIO_LENGTH   0x1000

#define MMIOIT_VECTOR        5
#define MMIOIT_IRQL          ((KIRQL)5)
#define MMIOIT_AFFINITY      ((KAFFINITY)1)

#define MMIOIT_REG_ID        0x00
#define MMIOIT_REG_CONTROL   0x04
#define MMIOIT_REG_STATUS    0x08
#define MMIOIT_REG_ACK       0x0c
#define MMIOIT_REG_IRQ_COUNT 0x10

#define MMIOIT_STATUS_INTERRUPT_PENDING 0x00000001UL

typedef struct _MMIOIT_DEVICE_EXTENSION {
    PUCHAR MmioBase;
    ULONG MmioLength;

    PKINTERRUPT InterruptObject;
    KSPIN_LOCK InterruptLock;

    KDPC Dpc;
    KSPIN_LOCK PendingIrpLock;
    PIRP PendingInterruptIrp;

    volatile LONG InterruptCount;
    volatile LONG Unloading;

    BOOLEAN Mapped;
    BOOLEAN InterruptConnected;
} MMIOIT_DEVICE_EXTENSION, *PMMIOIT_DEVICE_EXTENSION;

static __inline PULONG
MmioReg32(_In_ PMMIOIT_DEVICE_EXTENSION Ext, _In_ ULONG Offset)
{
    return (PULONG)(Ext->MmioBase + Offset);
}

static NTSTATUS
MmioitCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN
MmioitValidReg32Offset(_In_ ULONG Offset)
{
    if ((Offset & 0x3) != 0) {
        return FALSE;
    }

    if (Offset > (MMIOIT_MMIO_LENGTH - sizeof(ULONG))) {
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
MmioitInterruptService(_In_ PKINTERRUPT Interrupt, _In_opt_ PVOID ServiceContext)
{
    PDEVICE_OBJECT device;
    PMMIOIT_DEVICE_EXTENSION ext;
    ULONG status;

    UNREFERENCED_PARAMETER(Interrupt);

    device = (PDEVICE_OBJECT)ServiceContext;
    ext = (PMMIOIT_DEVICE_EXTENSION)device->DeviceExtension;

    if (ext->Unloading != 0 || !ext->Mapped) {
        return FALSE;
    }

    status = READ_REGISTER_ULONG(MmioReg32(ext, MMIOIT_REG_STATUS));
    if ((status & MMIOIT_STATUS_INTERRUPT_PENDING) == 0) {
        return FALSE;
    }

    WRITE_REGISTER_ULONG(MmioReg32(ext, MMIOIT_REG_ACK), 1);

    InterlockedIncrement(&ext->InterruptCount);

    /*
     * Existing dispatcher runtime should run this at DISPATCH_LEVEL.
     */
    KeInsertQueueDpc(&ext->Dpc, NULL, NULL);

    return TRUE;
}

static VOID
MmioitDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PDEVICE_OBJECT device;
    PMMIOIT_DEVICE_EXTENSION ext;
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
    ext = (PMMIOIT_DEVICE_EXTENSION)device->DeviceExtension;

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
        MmioitCompleteIrp(irp, STATUS_SUCCESS, sizeof(ULONG));
    } else {
        MmioitCompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
}

static NTSTATUS
MmioitMapHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PMMIOIT_DEVICE_EXTENSION ext;
    PHYSICAL_ADDRESS physical;
    ULONG id;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Mapped) {
        return STATUS_SUCCESS;
    }

    physical.QuadPart = MMIOIT_PHYS_BASE;

    ext->MmioBase = (PUCHAR)MmMapIoSpace(physical, MMIOIT_MMIO_LENGTH, MmNonCached);
    if (ext->MmioBase == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ext->MmioLength = MMIOIT_MMIO_LENGTH;
    ext->Mapped = TRUE;

    /*
     * Force one read during DriverEntry so the runtime proves register access
     * immediately. Userspace-ntos simulated device should return "MMIO".
     */
    id = READ_REGISTER_ULONG(MmioReg32(ext, MMIOIT_REG_ID));
    if (id != MMIOIT_ID_VALUE) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
        ext->Mapped = FALSE;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
MmioitConnectInterrupt(_In_ PDEVICE_OBJECT DeviceObject)
{
    PMMIOIT_DEVICE_EXTENSION ext;
    NTSTATUS status;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->InterruptConnected) {
        return STATUS_SUCCESS;
    }

    status = IoConnectInterrupt(
        &ext->InterruptObject,
        MmioitInterruptService,
        DeviceObject,
        &ext->InterruptLock,
        MMIOIT_VECTOR,
        MMIOIT_IRQL,
        MMIOIT_IRQL,
        LevelSensitive,
        FALSE,
        MMIOIT_AFFINITY,
        FALSE
    );

    if (NT_SUCCESS(status)) {
        ext->InterruptConnected = TRUE;
    }

    return status;
}

static VOID
MmioitDisconnectInterrupt(_In_ PDEVICE_OBJECT DeviceObject)
{
    PMMIOIT_DEVICE_EXTENSION ext;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject = NULL;
        ext->InterruptConnected = FALSE;
    }
}

static VOID
MmioitUnmapHardware(_In_ PDEVICE_OBJECT DeviceObject)
{
    PMMIOIT_DEVICE_EXTENSION ext;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Mapped && ext->MmioBase != NULL) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
        ext->Mapped = FALSE;
    }
}

static NTSTATUS
MmioitCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return MmioitCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
MmioitUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return MmioitCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
MmioitWaitForInterrupt(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PMMIOIT_DEVICE_EXTENSION ext;
    KIRQL old_irql;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Unloading != 0) {
        return MmioitCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);

    if (ext->PendingInterruptIrp != NULL) {
        KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);
        return MmioitCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
    }

    IoMarkIrpPending(Irp);
    ext->PendingInterruptIrp = Irp;

    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    return STATUS_PENDING;
}

static NTSTATUS
MmioitCancelPendingWait(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PMMIOIT_DEVICE_EXTENSION ext;
    PIRP pending;
    KIRQL old_irql;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    pending = NULL;

    KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
    if (ext->PendingInterruptIrp != NULL) {
        pending = ext->PendingInterruptIrp;
        ext->PendingInterruptIrp = NULL;
    }
    KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

    if (pending != NULL) {
        MmioitCompleteIrp(pending, STATUS_CANCELLED, 0);
    }

    return MmioitCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
MmioitDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PMMIOIT_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    NTSTATUS status;
    ULONG value;
    PMMIOIT_REG32_REQUEST reg;

    ext = (PMMIOIT_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    if (!ext->Mapped) {
        return MmioitCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    }

    switch (code) {
    case IOCTL_MMIOIT_GET_ID:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = READ_REGISTER_ULONG(MmioReg32(ext, MMIOIT_REG_ID));
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_MMIOIT_GET_INTERRUPT_COUNT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        *((ULONG *)buffer) = (ULONG)ext->InterruptCount;
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_MMIOIT_READ_REG32:
        if (buffer == NULL || in_len < sizeof(MMIOIT_REG32_REQUEST) || out_len < sizeof(MMIOIT_REG32_REQUEST)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PMMIOIT_REG32_REQUEST)buffer;
        if (!MmioitValidReg32Offset(reg->Offset)) {
            return MmioitCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        reg->Value = READ_REGISTER_ULONG(MmioReg32(ext, reg->Offset));
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, sizeof(MMIOIT_REG32_REQUEST));

    case IOCTL_MMIOIT_WRITE_REG32:
        if (buffer == NULL || in_len < sizeof(MMIOIT_REG32_REQUEST)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        reg = (PMMIOIT_REG32_REQUEST)buffer;
        if (!MmioitValidReg32Offset(reg->Offset)) {
            return MmioitCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        WRITE_REGISTER_ULONG(MmioReg32(ext, reg->Offset), reg->Value);
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_MMIOIT_WRITE_CONTROL:
        if (buffer == NULL || in_len < sizeof(ULONG)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        value = *((ULONG *)buffer);
        WRITE_REGISTER_ULONG(MmioReg32(ext, MMIOIT_REG_CONTROL), value);
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_MMIOIT_WAIT_FOR_INTERRUPT:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return MmioitCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        return MmioitWaitForInterrupt(DeviceObject, Irp);

    case IOCTL_MMIOIT_CANCEL_WAIT:
        return MmioitCancelPendingWait(DeviceObject, Irp);

    case IOCTL_MMIOIT_DISCONNECT_INTERRUPT:
        MmioitDisconnectInterrupt(DeviceObject);
        return MmioitCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IOCTL_MMIOIT_RECONNECT_INTERRUPT:
        status = MmioitConnectInterrupt(DeviceObject);
        return MmioitCompleteIrp(Irp, status, 0);

    default:
        return MmioitCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID
MmioitUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT device;

    RtlInitUnicodeString(&dos_name, MMIOIT_DOS_NAME);
    IoDeleteSymbolicLink(&dos_name);

    device = DriverObject->DeviceObject;
    while (device != NULL) {
        PDEVICE_OBJECT next = device->NextDevice;
        PMMIOIT_DEVICE_EXTENSION ext = (PMMIOIT_DEVICE_EXTENSION)device->DeviceExtension;
        PIRP pending = NULL;
        KIRQL old_irql;

        ext->Unloading = 1;

        KeAcquireSpinLock(&ext->PendingIrpLock, &old_irql);
        if (ext->PendingInterruptIrp != NULL) {
            pending = ext->PendingInterruptIrp;
            ext->PendingInterruptIrp = NULL;
        }
        KeReleaseSpinLock(&ext->PendingIrpLock, old_irql);

        if (pending != NULL) {
            MmioitCompleteIrp(pending, STATUS_CANCELLED, 0);
        }

        MmioitDisconnectInterrupt(device);
        MmioitUnmapHardware(device);

        IoDeleteDevice(device);
        device = next;
    }
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT device_object;
    PMMIOIT_DEVICE_EXTENSION ext;
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    device_object = NULL;

    RtlInitUnicodeString(&device_name, MMIOIT_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, MMIOIT_DOS_NAME);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = MmioitUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = MmioitCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MmioitCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MmioitDeviceControl;
    DriverObject->DriverUnload = MmioitUnload;

    status = IoCreateDevice(
        DriverObject,
        sizeof(MMIOIT_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &device_object
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = (PMMIOIT_DEVICE_EXTENSION)device_object->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    KeInitializeSpinLock(&ext->InterruptLock);
    KeInitializeSpinLock(&ext->PendingIrpLock);
    KeInitializeDpc(&ext->Dpc, MmioitDpcRoutine, device_object);

    device_object->Flags |= DO_BUFFERED_IO;

    status = MmioitMapHardware(device_object);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device_object);
        return status;
    }

    status = MmioitConnectInterrupt(device_object);
    if (!NT_SUCCESS(status)) {
        MmioitUnmapHardware(device_object);
        IoDeleteDevice(device_object);
        return status;
    }

    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        MmioitDisconnectInterrupt(device_object);
        MmioitUnmapHardware(device_object);
        IoDeleteDevice(device_object);
        return status;
    }

    device_object->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
