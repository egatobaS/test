#include <ntifs.h>
#include "ComHandler.h"
#include "VMHook.h"
#include "Process.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* Type definition of the kernel API we are hooking. */
typedef NTSTATUS(NTAPI* fnNtCreateFile)(
	PHANDLE            FileHandle,
	ACCESS_MASK        DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK   IoStatusBlock,
	PLARGE_INTEGER     AllocationSize,
	ULONG              FileAttributes,
	ULONG              ShareAccess,
	ULONG              CreateDisposition,
	ULONG              CreateOptions,
	PVOID              EaBuffer,
	ULONG              EaLength
	);



static fnNtCreateFile origNtCreateFile = NULL;


static NTSTATUS NTAPI hookNtCreateFile(
	PHANDLE            FileHandle,
	ACCESS_MASK        DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK   IoStatusBlock,
	PLARGE_INTEGER     AllocationSize,
	ULONG              FileAttributes,
	ULONG              ShareAccess,
	ULONG              CreateDisposition,
	ULONG              CreateOptions,
	PVOID              EaBuffer,
	ULONG              EaLength
);



void ComHandler_init(void)
{
	/* It's not possible to use a VMExit handler for comms as if we rely on API it will cause blue screens.
	 * Instead we do this by sending a VMCALL to the HV to EPT hook a specified windows API for us. */
#pragma warning(disable:4054)
	VMHook_queueHook((PVOID)NtCreateFile, (PVOID)hookNtCreateFile, (PVOID*)&origNtCreateFile);
}

#define DebugPrint( X, ... )                                                                                           \
	DbgPrintEx( DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[P] [%04i] " X,                                          \
		   1, __VA_ARGS__ )

/******************** Module Code ********************/

NTSTATUS NTAPI hookNtCreateFile(
	PHANDLE            FileHandle,
	ACCESS_MASK        DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK   IoStatusBlock,
	PLARGE_INTEGER     AllocationSize,
	ULONG              FileAttributes,
	ULONG              ShareAccess,
	ULONG              CreateDisposition,
	ULONG              CreateOptions,
	PVOID              EaBuffer,
	ULONG              EaLength
)
{
	//DebugPrint("hook called\n");

	NTSTATUS status = origNtCreateFile(FileHandle,
		DesiredAccess,
		ObjectAttributes,
		IoStatusBlock,
		AllocationSize,
		FileAttributes,
		ShareAccess,
		CreateDisposition,
		CreateOptions,
		EaBuffer,
		EaLength);

	return status;
}

