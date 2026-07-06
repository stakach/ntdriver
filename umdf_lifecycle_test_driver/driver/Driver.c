/*
 * UmdfLifecycleTest - UMDF v2 lifecycle test driver for userspace-ntos
 * UMDF host/reflector bring-up.
 *
 * Ported from the original UMDF v1 (COM) project to the UMDF v2 WDF object
 * model. It builds to a user-mode driver DLL hosted by WUDFHost.exe through
 * the WUDFRd.sys reflector. No hardware; it exercises the driver/device/queue
 * lifecycle and reports per-callback counters through IOCTL_UMDFLT_GET_INFO.
 *
 * Lifecycle callbacks exercised:
 *   DriverEntry / EvtDriverUnload        (was IDriverEntry OnInitialize/OnDeinitialize)
 *   EvtDriverDeviceAdd                    (was IDriverEntry::OnDeviceAdd)
 *   EvtDevicePrepareHardware / ReleaseHardware
 *   EvtDeviceD0Entry / EvtDeviceD0Exit
 *   EvtIoDeviceControl
 *   EvtCleanupCallback                    (was IObjectCleanup::OnCleanup)
 */

#include "UmdfLifecycleTest.h"

// Process-wide lifecycle counters (the driver DLL is loaded once per host).
static volatile LONG g_OnInitializeCount = 0;
static volatile LONG g_OnDeviceAddCount = 0;
static volatile LONG g_OnPrepareHardwareCount = 0;
static volatile LONG g_OnD0EntryCount = 0;
static volatile LONG g_OnD0ExitCount = 0;
static volatile LONG g_OnReleaseHardwareCount = 0;
static volatile LONG g_OnIoctlCount = 0;
static volatile LONG g_OnCleanupCount = 0;
static volatile LONG g_OnDeinitializeCount = 0;
static volatile LONG g_LastControlCode = 0;
static volatile LONG g_LastInputSize = 0;
static volatile LONG g_LastOutputSize = 0;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD UmdfltEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD UmdfltEvtDriverUnload;
EVT_WDF_DEVICE_PREPARE_HARDWARE UmdfltEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE UmdfltEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY UmdfltEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT UmdfltEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL UmdfltEvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP UmdfltEvtDeviceCleanup;

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    InterlockedIncrement(&g_OnInitializeCount);

    WDF_DRIVER_CONFIG_INIT(&config, UmdfltEvtDeviceAdd);
    config.EvtDriverUnload = UmdfltEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );
}

VOID
UmdfltEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    InterlockedIncrement(&g_OnDeinitializeCount);
}

NTSTATUS
UmdfltEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
    WDF_OBJECT_ATTRIBUTES device_attributes;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDFQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    InterlockedIncrement(&g_OnDeviceAddCount);

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
    pnp_power_callbacks.EvtDevicePrepareHardware = UmdfltEvtDevicePrepareHardware;
    pnp_power_callbacks.EvtDeviceReleaseHardware = UmdfltEvtDeviceReleaseHardware;
    pnp_power_callbacks.EvtDeviceD0Entry = UmdfltEvtDeviceD0Entry;
    pnp_power_callbacks.EvtDeviceD0Exit = UmdfltEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power_callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT(&device_attributes);
    device_attributes.EvtCleanupCallback = UmdfltEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &device_attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_UMDF_LIFECYCLE_TEST,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchParallel);
    queue_config.EvtIoDeviceControl = UmdfltEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        device,
        &queue_config,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
UmdfltEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    InterlockedIncrement(&g_OnPrepareHardwareCount);
    return STATUS_SUCCESS;
}

NTSTATUS
UmdfltEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    InterlockedIncrement(&g_OnReleaseHardwareCount);
    return STATUS_SUCCESS;
}

NTSTATUS
UmdfltEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    InterlockedIncrement(&g_OnD0EntryCount);
    return STATUS_SUCCESS;
}

NTSTATUS
UmdfltEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    InterlockedIncrement(&g_OnD0ExitCount);
    return STATUS_SUCCESS;
}

VOID
UmdfltEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&g_OnCleanupCount);
}

VOID
UmdfltEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    NTSTATUS status;
    size_t bytes;
    PVOID output_buffer;

    UNREFERENCED_PARAMETER(Queue);

    InterlockedIncrement(&g_OnIoctlCount);
    InterlockedExchange(&g_LastControlCode, (LONG)IoControlCode);
    InterlockedExchange(&g_LastInputSize, (LONG)InputBufferLength);
    InterlockedExchange(&g_LastOutputSize, (LONG)OutputBufferLength);

    status = STATUS_SUCCESS;
    bytes = 0;

    switch (IoControlCode) {
    case IOCTL_UMDFLT_PING:
        break;

    case IOCTL_UMDFLT_FAIL_REQUEST:
        status = STATUS_UNSUCCESSFUL;
        break;

    case IOCTL_UMDFLT_GET_INFO:
        if (OutputBufferLength < sizeof(UMDFLT_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(UMDFLT_INFO),
            &output_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            UMDFLT_INFO info = {0};

            info.Magic = UMDFLT_MAGIC;
            info.Version = UMDFLT_VERSION;
            info.OnInitializeCount = (ULONG)g_OnInitializeCount;
            info.OnDeviceAddCount = (ULONG)g_OnDeviceAddCount;
            info.OnPrepareHardwareCount = (ULONG)g_OnPrepareHardwareCount;
            info.OnD0EntryCount = (ULONG)g_OnD0EntryCount;
            info.OnD0ExitCount = (ULONG)g_OnD0ExitCount;
            info.OnReleaseHardwareCount = (ULONG)g_OnReleaseHardwareCount;
            info.OnIoctlCount = (ULONG)g_OnIoctlCount;
            info.OnCleanupCount = (ULONG)g_OnCleanupCount;
            info.OnDeinitializeCount = (ULONG)g_OnDeinitializeCount;
            info.LastControlCode = (ULONG)g_LastControlCode;
            info.LastInputSize = (ULONG)g_LastInputSize;
            info.LastOutputSize = (ULONG)g_LastOutputSize;

            *((UMDFLT_INFO *)output_buffer) = info;
            bytes = sizeof(UMDFLT_INFO);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytes);
}
