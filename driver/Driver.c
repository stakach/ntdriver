/*
 * SurtTest - tiny legacy WDM driver for userspace-ntos Driver Host testing.
 *
 * Purpose:
 *   - No PnP
 *   - No WDF/KMDF
 *   - No hardware
 *   - Creates one named device object and one DOS symbolic link
 *   - Handles CREATE, CLOSE, DEVICE_CONTROL
 *   - Uses METHOD_BUFFERED IOCTLs only
 *
 * Expected NT objects:
 *   \\Device\\SurtTest
 *   \\DosDevices\\SurtTest -> \\Device\\SurtTest
 */

#include <ntddk.h>
#include "SurtTest.h"

#define SURT_DEVICE_NAME   L"\\Device\\SurtTest"
#define SURT_DOS_NAME      L"\\DosDevices\\SurtTest"

static VOID
SurtCopyBytes(_Out_writes_bytes_(length) UCHAR *dst,
              _In_reads_bytes_(length) const UCHAR *src,
              _In_ ULONG length)
{
    ULONG i;
    for (i = 0; i < length; i++) {
        dst[i] = src[i];
    }
}

static NTSTATUS
SurtCompleteIrp(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;

    /* IoCompleteRequest is a macro that normally calls IofCompleteRequest. */
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
SurtCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SurtCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
SurtUnsupported(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SurtCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS
SurtDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG in_len;
    ULONG out_len;
    void *buffer;
    NTSTATUS status;
    ULONG_PTR information;

    UNREFERENCED_PARAMETER(DeviceObject);

    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;

    /* METHOD_BUFFERED: AssociatedIrp.SystemBuffer is input and output. */
    buffer = Irp->AssociatedIrp.SystemBuffer;

    status = STATUS_SUCCESS;
    information = 0;

    switch (code) {
    case IOCTL_SURT_PING:
        if (buffer == NULL || out_len < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        *((ULONG *)buffer) = SURT_PING_VALUE;
        information = sizeof(ULONG);
        break;

    case IOCTL_SURT_GET_VERSION:
        if (buffer == NULL || out_len < sizeof(SURT_TEST_VERSION)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        ((SURT_TEST_VERSION *)buffer)->Major = 0;
        ((SURT_TEST_VERSION *)buffer)->Minor = 1;
        ((SURT_TEST_VERSION *)buffer)->Patch = 0;
        ((SURT_TEST_VERSION *)buffer)->Build = 9;
        information = sizeof(SURT_TEST_VERSION);
        break;

    case IOCTL_SURT_ECHO:
        if (buffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        information = (out_len < in_len) ? out_len : in_len;
        if (information > 0) {
            SurtCopyBytes((UCHAR *)buffer, (const UCHAR *)buffer, (ULONG)information);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return SurtCompleteIrp(Irp, status, information);
}

static VOID
SurtUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT device;

    RtlInitUnicodeString(&dos_name, SURT_DOS_NAME);
    IoDeleteSymbolicLink(&dos_name);

    device = DriverObject->DeviceObject;
    while (device != NULL) {
        PDEVICE_OBJECT next = device->NextDevice;
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
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    device_object = NULL;

    RtlInitUnicodeString(&device_name, SURT_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, SURT_DOS_NAME);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = SurtUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = SurtCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SurtCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SurtDeviceControl;
    DriverObject->DriverUnload = SurtUnload;

    status = IoCreateDevice(
        DriverObject,
        0,
        &device_name,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &device_object
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    device_object->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device_object);
        return status;
    }

    device_object->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
