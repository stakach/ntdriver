/*
 * KmdfInterfaceRegistryTest - KMDF device-interface / registry / property
 * test driver for userspace-ntos.
 *
 * Purpose:
 *   - WdfDeviceCreateDeviceInterface
 *   - WdfDeviceRetrieveDeviceInterfaceString
 *   - WDFSTRING
 *   - WdfDriverOpenParametersRegistryKey
 *   - WdfDeviceOpenRegistryKey
 *   - WdfRegistryQueryULong / AssignULong
 *   - WdfRegistryQueryString / AssignString
 *   - WdfDeviceAssignProperty
 *   - KMDF queue/request path through an interface-opened file handle
 *
 * Expected NT objects:
 *   \Device\KmdfInterfaceRegistryTest
 *   \DosDevices\KmdfInterfaceRegistryTest -> \Device\KmdfInterfaceRegistryTest
 *
 * Device interface class GUID:
 *   {9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001}
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <devpropdef.h>
#include <devpkey.h>
#include "KmdfInterfaceRegistryTest.h"

#define KMDF_IF_DEVICE_NAME   L"\\Device\\KmdfInterfaceRegistryTest"
#define KMDF_IF_DOS_NAME      L"\\DosDevices\\KmdfInterfaceRegistryTest"

/* {9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001} */
DEFINE_GUID(
    GUID_DEVINTERFACE_USERSPACE_NTOS_TEST,
    0x9a7b0b24, 0x6e57, 0x4c51,
    0xad, 0x3c, 0x6d, 0x9f, 0x5f, 0x0e, 0x00, 0x01
);

/* {9A7B0B24-6E57-4C51-AD3C-6D9F5F0E0001}, PID 2 */
DEFINE_DEVPROPKEY(
    DEVPKEY_UserspaceNtos_InterfaceTestAnswer,
    0x9a7b0b24, 0x6e57, 0x4c51,
    0xad, 0x3c, 0x6d, 0x9f, 0x5f, 0x0e, 0x00, 0x01,
    2
);

typedef struct _KMDF_IF_DEVICE_CONTEXT {
    ULONG Prepared;
    ULONG Powered;
    ULONG IoctlCount;

    ULONG Answer;
    ULONG SeenByDriver;
    ULONG DeviceSeenByDriver;
    ULONG RuntimeValue;
    ULONG CustomPropertyValue;

    WDFSTRING GreetingString;
    WDFSTRING InterfaceString;

    BOOLEAN InterfaceCreated;
    BOOLEAN LegacySymbolicLinkCreated;
} KMDF_IF_DEVICE_CONTEXT, *PKMDF_IF_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDF_IF_DEVICE_CONTEXT, KmdfIfGetContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD KmdfIfEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE KmdfIfEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE KmdfIfEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY KmdfIfEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT KmdfIfEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KmdfIfEvtIoDeviceControl;

static VOID
KmdfIfInitUnicode(_Out_ PUNICODE_STRING Name, _In_ PCWSTR Text)
{
    RtlInitUnicodeString(Name, Text);
}

static NTSTATUS
KmdfIfEnsureDriverStringValue(
    _In_ WDFKEY Key,
    _In_ PCWSTR ValueNameText,
    _In_ PCWSTR DefaultText,
    _In_ WDFSTRING String
)
{
    UNICODE_STRING value_name;
    UNICODE_STRING default_value;
    NTSTATUS status;

    KmdfIfInitUnicode(&value_name, ValueNameText);
    status = WdfRegistryQueryString(Key, &value_name, String);
    if (NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    KmdfIfInitUnicode(&default_value, DefaultText);
    status = WdfRegistryAssignString(Key, &value_name, &default_value);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return WdfRegistryQueryString(Key, &value_name, String);
}

static NTSTATUS
KmdfIfReadDriverRegistry(_In_ WDFDRIVER Driver, _In_ WDFDEVICE Device)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_OBJECT_ATTRIBUTES key_attributes;
    WDF_OBJECT_ATTRIBUTES string_attributes;
    WDFKEY params_key;
    UNICODE_STRING value_name;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);
    params_key = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&string_attributes);
    string_attributes.ParentObject = Device;

    status = WdfStringCreate(NULL, &string_attributes, &ctx->GreetingString);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&key_attributes);
    key_attributes.ParentObject = Device;

    status = WdfDriverOpenParametersRegistryKey(
        Driver,
        KEY_READ | KEY_WRITE,
        &key_attributes,
        &params_key
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KmdfIfInitUnicode(&value_name, L"Answer");
    status = WdfRegistryQueryULong(params_key, &value_name, &ctx->Answer);
    if (!NT_SUCCESS(status)) {
        ctx->Answer = 42;
        status = WdfRegistryAssignULong(params_key, &value_name, ctx->Answer);
        if (!NT_SUCCESS(status)) {
            WdfRegistryClose(params_key);
            return status;
        }
    }

    ctx->SeenByDriver = 1;
    KmdfIfInitUnicode(&value_name, L"SeenByDriver");
    status = WdfRegistryAssignULong(params_key, &value_name, ctx->SeenByDriver);
    if (!NT_SUCCESS(status)) {
        WdfRegistryClose(params_key);
        return status;
    }

    status = KmdfIfEnsureDriverStringValue(
        params_key,
        L"Greeting",
        L"hello registry",
        ctx->GreetingString
    );

    if (!NT_SUCCESS(status)) {
        WdfRegistryClose(params_key);
        return status;
    }

    WdfRegistryClose(params_key);
    return STATUS_SUCCESS;
}

static NTSTATUS
KmdfIfReadWriteDeviceRegistry(_In_ WDFDEVICE Device)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_OBJECT_ATTRIBUTES key_attributes;
    WDFKEY device_key;
    UNICODE_STRING value_name;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);
    device_key = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&key_attributes);
    key_attributes.ParentObject = Device;

    status = WdfDeviceOpenRegistryKey(
        Device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_READ | KEY_WRITE,
        &key_attributes,
        &device_key
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx->DeviceSeenByDriver = 1;
    KmdfIfInitUnicode(&value_name, L"DeviceSeenByDriver");
    status = WdfRegistryAssignULong(device_key, &value_name, ctx->DeviceSeenByDriver);
    if (!NT_SUCCESS(status)) {
        WdfRegistryClose(device_key);
        return status;
    }

    KmdfIfInitUnicode(&value_name, L"RuntimeValue");
    status = WdfRegistryQueryULong(device_key, &value_name, &ctx->RuntimeValue);
    if (!NT_SUCCESS(status)) {
        ctx->RuntimeValue = 0;
        status = WdfRegistryAssignULong(device_key, &value_name, ctx->RuntimeValue);
        if (!NT_SUCCESS(status)) {
            WdfRegistryClose(device_key);
            return status;
        }
    }

    WdfRegistryClose(device_key);
    return STATUS_SUCCESS;
}

static NTSTATUS
KmdfIfAssignProperties(_In_ WDFDEVICE Device)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_DEVICE_PROPERTY_DATA property_data;
    UNICODE_STRING friendly_name;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);
    ctx->CustomPropertyValue = ctx->Answer;

    KmdfIfInitUnicode(
        &friendly_name,
        L"userspace-ntos KMDF Interface Registry Test Device"
    );

    WDF_DEVICE_PROPERTY_DATA_INIT(&property_data, &DEVPKEY_Device_FriendlyName);

    status = WdfDeviceAssignProperty(
        Device,
        &property_data,
        DEVPROP_TYPE_STRING,
        friendly_name.Length + sizeof(WCHAR),
        friendly_name.Buffer
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_DEVICE_PROPERTY_DATA_INIT(&property_data, &DEVPKEY_UserspaceNtos_InterfaceTestAnswer);

    status = WdfDeviceAssignProperty(
        Device,
        &property_data,
        DEVPROP_TYPE_UINT32,
        sizeof(ctx->CustomPropertyValue),
        &ctx->CustomPropertyValue
    );

    return status;
}

static NTSTATUS
KmdfIfCreateInterface(_In_ WDFDEVICE Device)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_OBJECT_ATTRIBUTES string_attributes;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);

    status = WdfDeviceCreateDeviceInterface(
        Device,
        &GUID_DEVINTERFACE_USERSPACE_NTOS_TEST,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx->InterfaceCreated = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&string_attributes);
    string_attributes.ParentObject = Device;

    status = WdfStringCreate(NULL, &string_attributes, &ctx->InterfaceString);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceRetrieveDeviceInterfaceString(
        Device,
        &GUID_DEVINTERFACE_USERSPACE_NTOS_TEST,
        NULL,
        ctx->InterfaceString
    );

    return status;
}

static VOID
KmdfIfCopyUnicodeToOutput(
    _In_ PUNICODE_STRING Source,
    _Out_ PKMDF_IF_STRING_OUT Out,
    _In_ size_t OutSize
)
{
    USHORT max_bytes;
    USHORT copy_bytes;

    Out->LengthBytes = 0;
    Out->MaximumLengthBytes = sizeof(Out->Buffer);
    Out->Buffer[0] = L'\0';

    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0) {
        return;
    }

    max_bytes = (USHORT)(sizeof(Out->Buffer) - sizeof(WCHAR));
    copy_bytes = Source->Length;
    if (copy_bytes > max_bytes) {
        copy_bytes = max_bytes;
    }

    if (OutSize < sizeof(KMDF_IF_STRING_OUT)) {
        return;
    }

    RtlCopyMemory(Out->Buffer, Source->Buffer, copy_bytes);
    Out->Buffer[copy_bytes / sizeof(WCHAR)] = L'\0';
    Out->LengthBytes = copy_bytes;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_DRIVER_CONFIG_INIT(&config, KmdfIfEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );
}

NTSTATUS
KmdfIfEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
    WDF_OBJECT_ATTRIBUTES device_attributes;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDFQUEUE queue;
    UNICODE_STRING device_name;
    UNICODE_STRING symbolic_link;
    NTSTATUS status;
    PKMDF_IF_DEVICE_CONTEXT ctx;

    KmdfIfInitUnicode(&device_name, KMDF_IF_DEVICE_NAME);
    status = WdfDeviceInitAssignName(DeviceInit, &device_name);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
    pnp_power_callbacks.EvtDevicePrepareHardware = KmdfIfEvtDevicePrepareHardware;
    pnp_power_callbacks.EvtDeviceReleaseHardware = KmdfIfEvtDeviceReleaseHardware;
    pnp_power_callbacks.EvtDeviceD0Entry = KmdfIfEvtDeviceD0Entry;
    pnp_power_callbacks.EvtDeviceD0Exit = KmdfIfEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power_callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, KMDF_IF_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &device_attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = KmdfIfGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    KmdfIfInitUnicode(&symbolic_link, KMDF_IF_DOS_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &symbolic_link);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx->LegacySymbolicLinkCreated = TRUE;

    status = KmdfIfCreateInterface(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KmdfIfReadDriverRegistry(Driver, device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KmdfIfReadWriteDeviceRegistry(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KmdfIfAssignProperties(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queue_config,
        WdfIoQueueDispatchSequential
    );
    queue_config.EvtIoDeviceControl = KmdfIfEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        device,
        &queue_config,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );

    return status;
}

NTSTATUS
KmdfIfEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    ctx = KmdfIfGetContext(Device);
    ctx->Prepared = 1;
    return STATUS_SUCCESS;
}

NTSTATUS
KmdfIfEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    ctx = KmdfIfGetContext(Device);
    ctx->Prepared = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
KmdfIfEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(PreviousState);

    ctx = KmdfIfGetContext(Device);
    ctx->Powered = 1;
    return STATUS_SUCCESS;
}

NTSTATUS
KmdfIfEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(TargetState);

    ctx = KmdfIfGetContext(Device);
    ctx->Powered = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
KmdfIfSetRuntimeValue(_In_ WDFDEVICE Device, _In_ ULONG Value)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_OBJECT_ATTRIBUTES key_attributes;
    WDFKEY device_key;
    UNICODE_STRING value_name;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);
    device_key = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&key_attributes);
    key_attributes.ParentObject = Device;

    status = WdfDeviceOpenRegistryKey(
        Device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_READ | KEY_WRITE,
        &key_attributes,
        &device_key
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KmdfIfInitUnicode(&value_name, L"RuntimeValue");
    status = WdfRegistryAssignULong(device_key, &value_name, Value);
    if (NT_SUCCESS(status)) {
        ctx->RuntimeValue = Value;
    }

    WdfRegistryClose(device_key);
    return status;
}

static NTSTATUS
KmdfIfGetRuntimeValue(_In_ WDFDEVICE Device, _Out_ PULONG Value)
{
    PKMDF_IF_DEVICE_CONTEXT ctx;
    WDF_OBJECT_ATTRIBUTES key_attributes;
    WDFKEY device_key;
    UNICODE_STRING value_name;
    NTSTATUS status;

    ctx = KmdfIfGetContext(Device);
    device_key = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&key_attributes);
    key_attributes.ParentObject = Device;

    status = WdfDeviceOpenRegistryKey(
        Device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_READ | KEY_WRITE,
        &key_attributes,
        &device_key
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KmdfIfInitUnicode(&value_name, L"RuntimeValue");
    status = WdfRegistryQueryULong(device_key, &value_name, Value);
    if (NT_SUCCESS(status)) {
        ctx->RuntimeValue = *Value;
    }

    WdfRegistryClose(device_key);
    return status;
}

VOID
KmdfIfEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFDEVICE device;
    PKMDF_IF_DEVICE_CONTEXT ctx;
    NTSTATUS status;
    size_t bytes;
    PVOID input_buffer;
    PVOID output_buffer;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    ctx = KmdfIfGetContext(device);

    ctx->IoctlCount++;

    status = STATUS_SUCCESS;
    bytes = 0;
    input_buffer = NULL;
    output_buffer = NULL;

    switch (IoControlCode) {
    case IOCTL_KMDF_IF_PING:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            *((ULONG *)output_buffer) = KMDF_IF_PING_VALUE;
            bytes = sizeof(ULONG);
        }
        break;

    case IOCTL_KMDF_IF_GET_CONFIG:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_IF_CONFIG), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            PKMDF_IF_CONFIG config;
            UNICODE_STRING str;

            config = (PKMDF_IF_CONFIG)output_buffer;
            RtlZeroMemory(config, sizeof(*config));

            config->Prepared = ctx->Prepared;
            config->Powered = ctx->Powered;
            config->InterfaceCreated = ctx->InterfaceCreated ? 1 : 0;
            config->Answer = ctx->Answer;
            config->SeenByDriver = ctx->SeenByDriver;
            config->DeviceSeenByDriver = ctx->DeviceSeenByDriver;
            config->RuntimeValue = ctx->RuntimeValue;
            config->CustomPropertyValue = ctx->CustomPropertyValue;
            config->IoctlCount = ctx->IoctlCount;

            if (ctx->GreetingString != NULL) {
                WdfStringGetUnicodeString(ctx->GreetingString, &str);
                config->GreetingLengthBytes = str.Length;
            }

            if (ctx->InterfaceString != NULL) {
                WdfStringGetUnicodeString(ctx->InterfaceString, &str);
                config->InterfaceStringLengthBytes = str.Length;
            }

            bytes = sizeof(KMDF_IF_CONFIG);
        }
        break;

    case IOCTL_KMDF_IF_GET_INTERFACE_STRING:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_IF_STRING_OUT), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            UNICODE_STRING str;
            RtlZeroMemory(output_buffer, sizeof(KMDF_IF_STRING_OUT));

            if (ctx->InterfaceString != NULL) {
                WdfStringGetUnicodeString(ctx->InterfaceString, &str);
                KmdfIfCopyUnicodeToOutput(&str, (PKMDF_IF_STRING_OUT)output_buffer, sizeof(KMDF_IF_STRING_OUT));
            }

            bytes = sizeof(KMDF_IF_STRING_OUT);
        }
        break;

    case IOCTL_KMDF_IF_GET_GREETING:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_IF_STRING_OUT), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            UNICODE_STRING str;
            RtlZeroMemory(output_buffer, sizeof(KMDF_IF_STRING_OUT));

            if (ctx->GreetingString != NULL) {
                WdfStringGetUnicodeString(ctx->GreetingString, &str);
                KmdfIfCopyUnicodeToOutput(&str, (PKMDF_IF_STRING_OUT)output_buffer, sizeof(KMDF_IF_STRING_OUT));
            }

            bytes = sizeof(KMDF_IF_STRING_OUT);
        }
        break;

    case IOCTL_KMDF_IF_SET_RUNTIME_VALUE:
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(KMDF_IF_DWORD_VALUE), &input_buffer, NULL);
        if (NT_SUCCESS(status)) {
            status = KmdfIfSetRuntimeValue(device, ((PKMDF_IF_DWORD_VALUE)input_buffer)->Value);
        }
        break;

    case IOCTL_KMDF_IF_GET_RUNTIME_VALUE:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_IF_DWORD_VALUE), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            PKMDF_IF_DWORD_VALUE value;
            ULONG runtime_value;

            value = (PKMDF_IF_DWORD_VALUE)output_buffer;
            runtime_value = 0;
            status = KmdfIfGetRuntimeValue(device, &runtime_value);
            if (NT_SUCCESS(status)) {
                value->Value = runtime_value;
                bytes = sizeof(KMDF_IF_DWORD_VALUE);
            }
        }
        break;

    case IOCTL_KMDF_IF_ECHO:
        status = WdfRequestRetrieveInputBuffer(Request, 1, &input_buffer, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request, 1, &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            size_t copy_len;

            copy_len = InputBufferLength;
            if (copy_len > OutputBufferLength) {
                copy_len = OutputBufferLength;
            }

            RtlCopyMemory(output_buffer, input_buffer, copy_len);
            bytes = copy_len;
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytes);
}
