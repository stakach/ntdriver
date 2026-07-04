/*
 * KmdfBasicTest - tiny KMDF driver for userspace-ntos WDF runtime testing.
 *
 * Purpose:
 *   - First controlled KMDF vertical slice
 *   - WdfDriverCreate
 *   - EvtDriverDeviceAdd
 *   - WdfDeviceCreate
 *   - WdfDeviceCreateSymbolicLink
 *   - WdfIoQueueCreate
 *   - EvtIoDeviceControl
 *   - WdfRequestRetrieveInputBuffer
 *   - WdfRequestRetrieveOutputBuffer
 *   - WdfRequestCompleteWithInformation
 *   - EvtDevicePrepareHardware / EvtDeviceReleaseHardware
 *   - EvtDeviceD0Entry / EvtDeviceD0Exit
 *
 * Expected NT objects:
 *   \Device\KmdfBasicTest
 *   \DosDevices\KmdfBasicTest -> \Device\KmdfBasicTest
 *
 * Optional translated memory resource:
 *   If your PnP fixture supplies a memory resource, this driver maps it in
 *   EvtDevicePrepareHardware and expects register 0x00 to read:
 *
 *     0x4b4d4446  ("KMDF")
 *
 * If no memory resource is supplied, the driver still loads and basic IOCTLs
 * work. Register IOCTLs then return STATUS_DEVICE_NOT_READY.
 */

#include <ntddk.h>
#include <wdf.h>
#include "KmdfBasicTest.h"

#define KMDF_BASIC_DEVICE_NAME   L"\\Device\\KmdfBasicTest"
#define KMDF_BASIC_DOS_NAME      L"\\DosDevices\\KmdfBasicTest"

#define KMDF_BASIC_REG_ID        0x00
#define KMDF_BASIC_REG_CONTROL   0x04
#define KMDF_BASIC_REG_STATUS    0x08

typedef struct _KMDF_BASIC_DEVICE_CONTEXT {
    PUCHAR MmioBase;
    ULONG MmioLength;
    PHYSICAL_ADDRESS MmioPhysicalStart;

    BOOLEAN Prepared;
    BOOLEAN Powered;
    BOOLEAN HasMmio;

    ULONG IoctlCount;
} KMDF_BASIC_DEVICE_CONTEXT, *PKMDF_BASIC_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDF_BASIC_DEVICE_CONTEXT, KmdfBasicGetContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD KmdfBasicEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE KmdfBasicEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE KmdfBasicEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY KmdfBasicEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT KmdfBasicEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KmdfBasicEvtIoDeviceControl;

static BOOLEAN
KmdfBasicValidReg32Offset(_In_ PKMDF_BASIC_DEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    if ((Offset & 0x3) != 0) {
        return FALSE;
    }

    if (Ctx->MmioLength < sizeof(ULONG)) {
        return FALSE;
    }

    if (Offset > (Ctx->MmioLength - sizeof(ULONG))) {
        return FALSE;
    }

    return TRUE;
}

static __inline PULONG
KmdfBasicReg32(_In_ PKMDF_BASIC_DEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    return (PULONG)(Ctx->MmioBase + Offset);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_DRIVER_CONFIG_INIT(&config, KmdfBasicEvtDeviceAdd);

    /*
     * Keep driver object attributes simple for v0.1. Object context is tested
     * on WDFDEVICE below.
     */
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
KmdfBasicEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
    WDF_OBJECT_ATTRIBUTES device_attributes;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDFQUEUE queue;
    UNICODE_STRING symbolic_link;
    NTSTATUS status;
    PKMDF_BASIC_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
    pnp_power_callbacks.EvtDevicePrepareHardware = KmdfBasicEvtDevicePrepareHardware;
    pnp_power_callbacks.EvtDeviceReleaseHardware = KmdfBasicEvtDeviceReleaseHardware;
    pnp_power_callbacks.EvtDeviceD0Entry = KmdfBasicEvtDeviceD0Entry;
    pnp_power_callbacks.EvtDeviceD0Exit = KmdfBasicEvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power_callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, KMDF_BASIC_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &device_attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = KmdfBasicGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    RtlInitUnicodeString(&symbolic_link, KMDF_BASIC_DOS_NAME);

    status = WdfDeviceCreateSymbolicLink(device, &symbolic_link);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queue_config,
        WdfIoQueueDispatchSequential
    );

    queue_config.EvtIoDeviceControl = KmdfBasicEvtIoDeviceControl;

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
KmdfBasicEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PKMDF_BASIC_DEVICE_CONTEXT ctx;
    ULONG count;
    ULONG index;
    BOOLEAN found_memory;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    ctx = KmdfBasicGetContext(Device);
    found_memory = FALSE;
    status = STATUS_SUCCESS;

    /*
     * This callback is optional-hardware friendly:
     *
     *   - If a memory resource exists, map it and verify the fake KMDF ID.
     *   - If no memory resource exists, leave HasMmio=false and still succeed.
     *
     * That lets your WDF runtime prove basic AddDevice/queue/request behaviour
     * before resource-list support is complete.
     */
    count = WdfCmResourceListGetCount(ResourcesTranslated);

    for (index = 0; index < count; index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;

        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, index);
        if (desc == NULL) {
            continue;
        }

        if (desc->Type == CmResourceTypeMemory && !found_memory) {
            ULONG id;

            ctx->MmioPhysicalStart = desc->u.Memory.Start;
            ctx->MmioLength = desc->u.Memory.Length;

            ctx->MmioBase = (PUCHAR)MmMapIoSpace(
                ctx->MmioPhysicalStart,
                ctx->MmioLength,
                MmNonCached
            );

            if (ctx->MmioBase == NULL) {
                ctx->MmioLength = 0;
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            ctx->HasMmio = TRUE;
            found_memory = TRUE;

            id = READ_REGISTER_ULONG(KmdfBasicReg32(ctx, KMDF_BASIC_REG_ID));
            if (id != KMDF_BASIC_ID_VALUE) {
                MmUnmapIoSpace(ctx->MmioBase, ctx->MmioLength);
                ctx->MmioBase = NULL;
                ctx->MmioLength = 0;
                ctx->HasMmio = FALSE;
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
    }

    ctx->Prepared = TRUE;

    return status;
}

NTSTATUS
KmdfBasicEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PKMDF_BASIC_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    ctx = KmdfBasicGetContext(Device);

    if (ctx->HasMmio && ctx->MmioBase != NULL) {
        MmUnmapIoSpace(ctx->MmioBase, ctx->MmioLength);
        ctx->MmioBase = NULL;
        ctx->MmioLength = 0;
        ctx->HasMmio = FALSE;
    }

    ctx->Prepared = FALSE;

    return STATUS_SUCCESS;
}

NTSTATUS
KmdfBasicEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PKMDF_BASIC_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(PreviousState);

    ctx = KmdfBasicGetContext(Device);
    ctx->Powered = TRUE;

    return STATUS_SUCCESS;
}

NTSTATUS
KmdfBasicEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PKMDF_BASIC_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(TargetState);

    ctx = KmdfBasicGetContext(Device);
    ctx->Powered = FALSE;

    return STATUS_SUCCESS;
}

VOID
KmdfBasicEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFDEVICE device;
    PKMDF_BASIC_DEVICE_CONTEXT ctx;
    NTSTATUS status;
    size_t bytes;
    PVOID input_buffer;
    PVOID output_buffer;

    device = WdfIoQueueGetDevice(Queue);
    ctx = KmdfBasicGetContext(device);

    ctx->IoctlCount++;

    bytes = 0;
    status = STATUS_SUCCESS;
    input_buffer = NULL;
    output_buffer = NULL;

    switch (IoControlCode) {
    case IOCTL_KMDF_BASIC_PING:
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(ULONG),
            &output_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            *((ULONG *)output_buffer) = KMDF_BASIC_PING_VALUE;
            bytes = sizeof(ULONG);
        }
        break;

    case IOCTL_KMDF_BASIC_GET_VERSION:
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(KMDF_BASIC_VERSION),
            &output_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            PKMDF_BASIC_VERSION version;

            version = (PKMDF_BASIC_VERSION)output_buffer;
            version->Major = 0;
            version->Minor = 1;
            version->Patch = 0;
            version->Build = 1;
            bytes = sizeof(KMDF_BASIC_VERSION);
        }
        break;

    case IOCTL_KMDF_BASIC_GET_STATE:
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(KMDF_BASIC_STATE),
            &output_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            PKMDF_BASIC_STATE state;

            state = (PKMDF_BASIC_STATE)output_buffer;
            state->Prepared = ctx->Prepared ? 1 : 0;
            state->Powered = ctx->Powered ? 1 : 0;
            state->HasMmio = ctx->HasMmio ? 1 : 0;
            state->MmioLength = ctx->MmioLength;
            state->IoctlCount = ctx->IoctlCount;
            bytes = sizeof(KMDF_BASIC_STATE);
        }
        break;

    case IOCTL_KMDF_BASIC_ECHO:
        status = WdfRequestRetrieveInputBuffer(
            Request,
            1,
            &input_buffer,
            NULL
        );

        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            1,
            &output_buffer,
            NULL
        );

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

    case IOCTL_KMDF_BASIC_READ_REG32:
        if (!ctx->HasMmio || ctx->MmioBase == NULL) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KMDF_BASIC_REG32_REQUEST),
            &input_buffer,
            NULL
        );

        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(KMDF_BASIC_REG32_REQUEST),
            &output_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            PKMDF_BASIC_REG32_REQUEST reg;

            reg = (PKMDF_BASIC_REG32_REQUEST)output_buffer;

            /*
             * METHOD_BUFFERED commonly gives the same system buffer for input
             * and output. Copy explicitly in case the runtime projects them
             * separately.
             */
            RtlCopyMemory(reg, input_buffer, sizeof(*reg));

            if (!KmdfBasicValidReg32Offset(ctx, reg->Offset)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            reg->Value = READ_REGISTER_ULONG(KmdfBasicReg32(ctx, reg->Offset));
            bytes = sizeof(KMDF_BASIC_REG32_REQUEST);
        }
        break;

    case IOCTL_KMDF_BASIC_WRITE_REG32:
        if (!ctx->HasMmio || ctx->MmioBase == NULL) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KMDF_BASIC_REG32_REQUEST),
            &input_buffer,
            NULL
        );

        if (NT_SUCCESS(status)) {
            PKMDF_BASIC_REG32_REQUEST reg;

            reg = (PKMDF_BASIC_REG32_REQUEST)input_buffer;

            if (!KmdfBasicValidReg32Offset(ctx, reg->Offset)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            WRITE_REGISTER_ULONG(KmdfBasicReg32(ctx, reg->Offset), reg->Value);
            bytes = 0;
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytes);
}
