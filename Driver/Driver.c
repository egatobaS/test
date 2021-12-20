#include <wdm.h>
#include "Hypervisor.h"
#include "ComHandler.h"
#include <ntddk.h>
#include "VMHook.h"
#include "Process.h"

#define DebugPrint( X, ... )                                                                                           \
	DbgPrintEx( DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[P] [%04i] " X,                                          \
		   1, __VA_ARGS__ )

#define DELAY_ONE_MICROSECOND ( -10 )
#define DELAY_ONE_MILLISECOND ( DELAY_ONE_MICROSECOND * 1000 )
#define DELAY_ONE_SECOND ( DELAY_ONE_MILLISECOND * 1000 )

void* Original;

void Sleep(UINT32 millis) {
	LARGE_INTEGER interval;

	interval.QuadPart = DELAY_ONE_MILLISECOND;
	interval.QuadPart *= millis;

	KeDelayExecutionThread(KernelMode, 0, &interval);
}

UINT64 Hook()
{
	return 0xDEADBEEFCAFEBEEF;
}


void KernelThread()
{
	while (1) {
		UINT64(*test)() = (UINT64(*)())Original;


		DebugPrint("[KernelThread] ret %p\n", test());

		Sleep(1);


	}
}

NTSTATUS DriverEntry(
	_In_ PDRIVER_OBJECT  DriverObject,
	_In_ PUNICODE_STRING RegistryPath
)
{
	UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(RegistryPath);

	/* Initialise the com handler so we have a way of UM->KM comms. */
	//ComHandler_init();

	/* Initialise other loggers. */
	//RegistryLogger_init();
	//WMILogger_init();

	//DebugPrint("Hv init 1234\n");

	Original = ExAllocatePool(NonPagedPool, 0x1000);
	
	if (Original)
	{
		memcpy((void*)Original, (void*)"\x48\xb8\x02\x00\xde\xc0\xef\xbe\xad\xde\xc3\x00\x00\x00", 12);
	
		HANDLE threadHandle = 0;
		PsCreateSystemThread(&threadHandle, (ACCESS_MASK)0, NULL, (HANDLE)0, NULL, (PKSTART_ROUTINE)KernelThread, NULL);
	
		void* stub = 0;
		VMHook_queueHook((PVOID)Original, (PVOID)Hook, (PVOID*)&stub);
	
	}

	HANDLE threadHandle = 0;
	return PsCreateSystemThread(&threadHandle, (ACCESS_MASK)0, NULL, (HANDLE)0, NULL, (PKSTART_ROUTINE)Hypervisor_init, NULL);
}

/******************** Module Code ********************/