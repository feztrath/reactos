
#include <ntddk.h>
#include <usbioctl.h>

#include <debug.h>

#include "../miniport/usb_wrapper.h"
#include "../usbport/hub.h"

#define USB_HUB_TAG 'hbsu'

typedef struct _HUB_DEVICE_EXTENSION
{
	BOOLEAN IsFDO;
	struct usb_device* dev;
	PDEVICE_OBJECT LowerDevice;

	PDEVICE_OBJECT Children[USB_MAXCHILDREN];

	/* Fields valid only when IsFDO == FALSE */
	UNICODE_STRING DeviceId;          // REG_SZ
	UNICODE_STRING InstanceId;        // REG_SZ
	UNICODE_STRING HardwareIds;       // REG_MULTI_SZ
	UNICODE_STRING CompatibleIds;     // REG_MULTI_SZ
	UNICODE_STRING SymbolicLinkName;
} HUB_DEVICE_EXTENSION, *PHUB_DEVICE_EXTENSION;

/* createclose.c */
NTSTATUS NTAPI
UsbhubCreate(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS NTAPI
UsbhubClose(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS NTAPI
UsbhubCleanup(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

/* fdo.c */
NTSTATUS NTAPI
UsbhubPnpFdo(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS
UsbhubDeviceControlFdo(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

/* misc.c */
NTSTATUS
ForwardIrpAndWait(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS NTAPI
ForwardIrpAndForget(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS
UsbhubDuplicateUnicodeString(
	OUT PUNICODE_STRING Destination,
	IN PUNICODE_STRING Source,
	IN POOL_TYPE PoolType);

NTSTATUS
UsbhubInitMultiSzString(
	OUT PUNICODE_STRING Destination,
	... /* list of PCSZ */);

/* pdo.c */
NTSTATUS NTAPI
UsbhubPnpPdo(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);

NTSTATUS
UsbhubInternalDeviceControlPdo(
	IN PDEVICE_OBJECT DeviceObject,
	IN PIRP Irp);
