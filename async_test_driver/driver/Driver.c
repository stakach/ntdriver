/*
 * AsyncTest - tiny legacy WDM async-completion driver for userspace-ntos.
 *
 * Purpose:
 *   - No PnP
 *   - No WDF/KMDF
 *   - No hardware
 *   - Creates one named device object and one DOS symbolic link
 *   - Handles CREATE, CLOSE, DEVICE_CONTROL
 *   - Exercises STATUS_PENDING + later IoCompleteRequest
 *   - Exercises KDPC, KTIMER+KDPC, and IO_WORKITEM paths
 *
 * Expected NT objects:
 *   \Device\AsyncTest
 *   \DosDevices\AsyncTest -> \Device\AsyncTest
 */

#include <ntddk.h>
#include "AsyncTest.h"

#define ASYNC_DEVICE_NAME   L"\\Device\\AsyncTest"
#define ASYNC_DOS_NAME      L"\\DosDevices\\AsyncTest"
#define ASYNC_POOL_TAG      'TsSA' /* "AsST" little-endian-ish */

typedef enum _ASYNC_KIND {
    AsyncKindDpc = 1,
    AsyncKindTimer = 2,
    AsyncKindWorkItem = 3
} ASYNC_KIND;

typedef struct _ASYNC_DEVICE_EXTENSION {
    volatile LONG Outstanding;
    volatile LONG Unloading;
    KEVENT ZeroOutstandingEvent;
} ASYNC_DEVICE_EXTENSION, *PASYNC_DEVICE_EXTENSION;

typedef struct _ASYNC_CONTEXT {
    ASYNC_KIND Kind;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;

    KDPC Dpc;
    KTIMER Timer;

    PIO_WORKITEM WorkItem;
} ASYNC_CONTEXT, *PASYNC_CONTEXT;

static VOID
AsyncReleaseOutstanding(_In_ PDEVICE_OBJECT DeviceObject)
{
    PASYNC_DEVICE_EXTENSION ext;

    ext = (PASYNC_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (InterlockedDecrement(&ext->Outstanding) == 0) {
        KeSetEvent(&ext->ZeroOutstandingEvent, IO_NO_INCREMENT, FALSE);
    }
}

static BOOLEAN
AsyncTryAcquireOutstanding(_In_ PDEVICE_OBJECT DeviceObject)
{
    PASYNC_DEVICE_EXTENSION ext;
    LONG count;

    ext = (PASYNC_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (ext->Unloading != 0) {
        return FALSE;
    }

    count = InterlockedIncrement(&ext->Outstanding);
    if (count == 1) {
        KeClearEvent(&ext->ZeroOutstandingEvent);
    }

    if (ext->Unloading != 0) {
        AsyncReleaseOutstanding(DeviceObject);
        return FALSE;
    }

    return TRUE;
}

static NTSTATUS
AsyncCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static VOID
AsyncFinishContext(_In_ PASYNC_CONTEXT Ctx, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    PDEVICE_OBJECT device;
    PIRP irp;

    device = Ctx->DeviceObject;
    irp = Ctx->Irp;

    if (Ctx->WorkItem != NULL) {
        IoFreeWorkItem(Ctx->WorkItem);
        Ctx->WorkItem = NULL;
    }

    ExFreePoolWithTag(Ctx, ASYNC_POOL_TAG);

    AsyncCompleteIrp(irp, Status, Information);
    AsyncReleaseOutstanding(device);
}

static VOID
AsyncDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PASYNC_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ctx = (PASYNC_CONTEXT)DeferredContext;
    AsyncFinishContext(ctx, STATUS_SUCCESS, ASYNC_RESULT_DPC);
}

static VOID
AsyncTimerDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PASYNC_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ctx = (PASYNC_CONTEXT)DeferredContext;
    AsyncFinishContext(ctx, STATUS_SUCCESS, ASYNC_RESULT_TIMER);
}

static VOID
AsyncWorkItemRoutine(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    PASYNC_CONTEXT ctx;

    UNREFERENCED_PARAMETER(DeviceObject);

    ctx = (PASYNC_CONTEXT)Context;
    AsyncFinishContext(ctx, STATUS_SUCCESS, ASYNC_RESULT_WORKITEM);
}

static NTSTATUS
AsyncQueueDpc(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PASYNC_CONTEXT ctx;
    BOOLEAN queued;

    if (!AsyncTryAcquireOutstanding(DeviceObject)) {
        return AsyncCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    ctx = (PASYNC_CONTEXT)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(ASYNC_CONTEXT), ASYNC_POOL_TAG);
    if (ctx == NULL) {
        AsyncReleaseOutstanding(DeviceObject);
        return AsyncCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Kind = AsyncKindDpc;
    ctx->DeviceObject = DeviceObject;
    ctx->Irp = Irp;

    IoMarkIrpPending(Irp);
    KeInitializeDpc(&ctx->Dpc, AsyncDpcRoutine, ctx);

    queued = KeInsertQueueDpc(&ctx->Dpc, NULL, NULL);
    if (!queued) {
        ExFreePoolWithTag(ctx, ASYNC_POOL_TAG);
        AsyncReleaseOutstanding(DeviceObject);
        return AsyncCompleteIrp(Irp, STATUS_UNSUCCESSFUL, 0);
    }

    return STATUS_PENDING;
}

static NTSTATUS
AsyncQueueTimer(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PASYNC_CONTEXT ctx;
    LARGE_INTEGER due_time;

    if (!AsyncTryAcquireOutstanding(DeviceObject)) {
        return AsyncCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    ctx = (PASYNC_CONTEXT)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(ASYNC_CONTEXT), ASYNC_POOL_TAG);
    if (ctx == NULL) {
        AsyncReleaseOutstanding(DeviceObject);
        return AsyncCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Kind = AsyncKindTimer;
    ctx->DeviceObject = DeviceObject;
    ctx->Irp = Irp;

    IoMarkIrpPending(Irp);

    KeInitializeTimer(&ctx->Timer);
    KeInitializeDpc(&ctx->Dpc, AsyncTimerDpcRoutine, ctx);

    /*
     * Relative due time, in 100 ns units. Negative means relative.
     * -10,000 * 100 ns = -1 ms. This is short enough for tests while still
     * exercising timer scheduling rather than immediate synchronous completion.
     */
    due_time.QuadPart = -10000;

    KeSetTimer(&ctx->Timer, due_time, &ctx->Dpc);

    return STATUS_PENDING;
}

static NTSTATUS
AsyncQueueWorkItem(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PASYNC_CONTEXT ctx;

    if (!AsyncTryAcquireOutstanding(DeviceObject)) {
        return AsyncCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
    }

    ctx = (PASYNC_CONTEXT)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(ASYNC_CONTEXT), ASYNC_POOL_TAG);
    if (ctx == NULL) {
        AsyncReleaseOutstanding(DeviceObject);
        return AsyncCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Kind = AsyncKindWorkItem;
    ctx->DeviceObject = DeviceObject;
    ctx->Irp = Irp;

    ctx->WorkItem = IoAllocateWorkItem(DeviceObject);
    if (ctx->WorkItem == NULL) {
        ExFreePoolWithTag(ctx, ASYNC_POOL_TAG);
        AsyncReleaseOutstanding(DeviceObject);
        return AsyncCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    IoMarkIrpPending(Irp);
    IoQueueWorkItem(ctx->WorkItem, AsyncWorkItemRoutine, DelayedWorkQueue, ctx);

    return STATUS_PENDING;
}

static NTSTATUS
AsyncCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return AsyncCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
AsyncUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return AsyncCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
AsyncDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG out_len;
    void *buffer;

    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (code) {
    case IOCTL_ASYNC_PING:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            return AsyncCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        *((ULONG *)buffer) = ASYNC_PING_VALUE;
        return AsyncCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_ASYNC_GET_VERSION:
        if (buffer == NULL || out_len < sizeof(ASYNC_TEST_VERSION)) {
            return AsyncCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        ((ASYNC_TEST_VERSION *)buffer)->Major = 0;
        ((ASYNC_TEST_VERSION *)buffer)->Minor = 1;
        ((ASYNC_TEST_VERSION *)buffer)->Patch = 0;
        ((ASYNC_TEST_VERSION *)buffer)->Build = 9;

        return AsyncCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ASYNC_TEST_VERSION));

    case IOCTL_ASYNC_COMPLETE_VIA_DPC:
        return AsyncQueueDpc(DeviceObject, Irp);

    case IOCTL_ASYNC_COMPLETE_VIA_TIMER:
        return AsyncQueueTimer(DeviceObject, Irp);

    case IOCTL_ASYNC_COMPLETE_VIA_WORKITEM:
        return AsyncQueueWorkItem(DeviceObject, Irp);

    default:
        return AsyncCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID
AsyncUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT device;
    PASYNC_DEVICE_EXTENSION ext;

    RtlInitUnicodeString(&dos_name, ASYNC_DOS_NAME);
    IoDeleteSymbolicLink(&dos_name);

    device = DriverObject->DeviceObject;
    while (device != NULL) {
        PDEVICE_OBJECT next = device->NextDevice;

        ext = (PASYNC_DEVICE_EXTENSION)device->DeviceExtension;
        ext->Unloading = 1;

        if (ext->Outstanding != 0) {
            KeWaitForSingleObject(
                &ext->ZeroOutstandingEvent,
                Executive,
                KernelMode,
                FALSE,
                NULL
            );
        }

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
    PASYNC_DEVICE_EXTENSION ext;
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    device_object = NULL;

    RtlInitUnicodeString(&device_name, ASYNC_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, ASYNC_DOS_NAME);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = AsyncUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = AsyncCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = AsyncCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AsyncDeviceControl;
    DriverObject->DriverUnload = AsyncUnload;

    status = IoCreateDevice(
        DriverObject,
        sizeof(ASYNC_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &device_object
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = (PASYNC_DEVICE_EXTENSION)device_object->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    KeInitializeEvent(&ext->ZeroOutstandingEvent, NotificationEvent, TRUE);

    device_object->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device_object);
        return status;
    }

    device_object->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
