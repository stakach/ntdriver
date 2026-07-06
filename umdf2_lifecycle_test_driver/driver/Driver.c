#include <windows.h>
#include <initguid.h>
#include <wdf.h>
#include "Umdf2LifecycleTest.h"

// {5F4D7E3B-8C43-4B9A-9E47-9E7E3DF57D22}
DEFINE_GUID(GUID_DEVINTERFACE_UMDF2_LIFECYCLE_TEST,
    0x5f4d7e3b, 0x8c43, 0x4b9a, 0x9e, 0x47, 0x9e, 0x7e, 0x3d, 0xf5, 0x7d, 0x22);

typedef struct _DEVICE_CONTEXT {
    ULONG DriverEntryCount;
    ULONG DeviceAddCount;
    ULONG PrepareHardwareCount;
    ULONG ReleaseHardwareCount;
    ULONG D0EntryCount;
    ULONG D0ExitCount;
    ULONG IoctlCount;
    ULONG CleanupCount;
    ULONG LastIoControlCode;
    ULONG RegistryAnswer;
    ULONG RegistrySeenByDriver;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Umdf2LtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Umdf2LtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Umdf2LtEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Umdf2LtEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT Umdf2LtEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Umdf2LtEvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP Umdf2LtEvtDeviceContextCleanup;

static VOID
Umdf2LtReadDriverParameters(_In_ WDFDRIVER Driver, _Out_ PULONG Answer, _Out_ PULONG Seen)
{
    WDFKEY key;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(answerName, L"Answer");
    DECLARE_CONST_UNICODE_STRING(seenName, L"SeenByDriver");

    *Answer = 0;
    *Seen = 0;

    status = WdfDriverOpenParametersRegistryKey(
        Driver,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);

    if (!NT_SUCCESS(status)) {
        return;
    }

    (VOID)WdfRegistryQueryULong(key, &answerName, Answer);

    *Seen = 1;
    (VOID)WdfRegistryAssignULong(key, &seenName, *Seen);

    WdfRegistryClose(key);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    UNREFERENCED_PARAMETER(RegistryPath);

    WDF_DRIVER_CONFIG_INIT(&config, Umdf2LtEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);
}

NTSTATUS
Umdf2LtEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT context;
    ULONG answer = 0;
    ULONG seen = 0;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = Umdf2LtEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = Umdf2LtEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = Umdf2LtEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = Umdf2LtEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = Umdf2LtEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = DeviceGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->DriverEntryCount = 1;
    context->DeviceAddCount = 1;

    Umdf2LtReadDriverParameters(Driver, &answer, &seen);
    context->RegistryAnswer = answer;
    context->RegistrySeenByDriver = seen;

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_UMDF2_LIFECYCLE_TEST,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = Umdf2LtEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UNREFERENCED_PARAMETER(queue);
    return STATUS_SUCCESS;
}

NTSTATUS
Umdf2LtEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    context->PrepareHardwareCount++;
    return STATUS_SUCCESS;
}

NTSTATUS
Umdf2LtEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    context->ReleaseHardwareCount++;
    return STATUS_SUCCESS;
}

NTSTATUS
Umdf2LtEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    context->D0EntryCount++;
    return STATUS_SUCCESS;
}

NTSTATUS
Umdf2LtEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    context->D0ExitCount++;
    return STATUS_SUCCESS;
}

VOID
Umdf2LtEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t information = 0;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = DeviceGetContext(device);

    context->IoctlCount++;
    context->LastIoControlCode = IoControlCode;

    switch (IoControlCode) {
    case IOCTL_UMDF2LT_PING:
        status = STATUS_SUCCESS;
        information = 0;
        break;

    case IOCTL_UMDF2LT_GET_INFO:
    {
        PUMDF2LT_INFO info;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), (PVOID*)&info, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(info, sizeof(*info));
            info->Signature = UMDF2LT_SIGNATURE;
            info->Version = UMDF2LT_VERSION;
            info->DriverEntryCount = context->DriverEntryCount;
            info->DeviceAddCount = context->DeviceAddCount;
            info->PrepareHardwareCount = context->PrepareHardwareCount;
            info->ReleaseHardwareCount = context->ReleaseHardwareCount;
            info->D0EntryCount = context->D0EntryCount;
            info->D0ExitCount = context->D0ExitCount;
            info->IoctlCount = context->IoctlCount;
            info->CleanupCount = context->CleanupCount;
            info->LastIoControlCode = context->LastIoControlCode;
            info->RegistryAnswer = context->RegistryAnswer;
            info->RegistrySeenByDriver = context->RegistrySeenByDriver;
            information = sizeof(*info);
        }
        break;
    }

    case IOCTL_UMDF2LT_ECHO:
    {
        PVOID inBuf;
        PVOID outBuf;
        size_t inLen = 0;
        size_t outLen = 0;
        size_t copyLen;

        status = WdfRequestRetrieveInputBuffer(Request, 1, &inBuf, &inLen);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen);
        if (!NT_SUCCESS(status)) {
            break;
        }

        copyLen = (inLen < outLen) ? inLen : outLen;
        RtlCopyMemory(outBuf, inBuf, copyLen);
        information = copyLen;
        break;
    }

    case IOCTL_UMDF2LT_FAIL_REQUEST:
        status = STATUS_UNSUCCESSFUL;
        information = 0;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        information = 0;
        break;
    }

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    WdfRequestCompleteWithInformation(Request, status, information);
}

VOID
Umdf2LtEvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    PDEVICE_CONTEXT context = DeviceGetContext((WDFDEVICE)Object);
    context->CleanupCount++;
}
