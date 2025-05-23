#include <intrin.h>
#include <ntddk.h>
#include "MemManage.h"
#include "GuestShim.h"
#include "ia32.h"
#include "Debug.h"
#include "EPT.h"

/******************** External API ********************/


/******************** Module Typedefs ********************/

/* Each level of a paging structure. */
typedef enum
{
	MM_LEVEL_PTE = 1,
	MM_LEVEL_PDE = 2,
	MM_LEVEL_PDPTE = 3,
	MM_LEVEL_PML4E = 4
} MM_LEVEL;

/******************** Module Constants ********************/
#define POOL_TAG '1TST'

#define OFFSET_DIRECTORY_TABLE_BASE 0x028
#define OFFSET_USER_DIR_TABLE 0x388

#define SIZE_2MB (2 * 1024 * 1024)

/* Counts of how many entries are in a table. */
#define PAGING_TABLE_ENTRY_COUNT 512
#define PAGING_PML4E_COUNT		512
#define PAGING_PML3E_COUNT		512
#define PAGING_PML2E_COUNT		512
#define PAGING_PML1E_COUNT		512

/* Calculates the offset into the PDE (PML1) structure. */
#define ADDRMASK_PML1_OFFSET(_VAR_) ((SIZE_T)_VAR_ & 0xFFFULL)

/* Calculates the index of the PDE (PML1) within the PDT structure. */
#define ADDRMASK_PML1_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x1FF000ULL) >> 12)

/* Calculates the index of the PDT (PML2) within the PDPT*/
#define ADDRMASK_PML2_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x3FE00000ULL) >> 21)

/* Calculates the index of the PDPT (PML3) within the PML4 */
#define ADDRMASK_PML3_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0x7FC0000000ULL) >> 30)

/* Calculates the index of PML4. */
#define ADDRMASK_PML4_INDEX(_VAR_) (((SIZE_T)_VAR_ & 0xFF8000000000ULL) >> 39)

/******************** Module Variables ********************/


/******************** Module Prototypes ********************/
PT_ENTRY_64* getSystemPTEFromVA(CR3 tableBase, PVOID virtualAddress, MM_LEVEL* level);
static VOID* mapPhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress);
static void unmapPhysicalAddress(PMM_CONTEXT context);
static NTSTATUS split2MbPage(PDE_2MB_64* pdeLarge);
static UINT64 physicalFromVirtual(VOID* virtualAddress);
static VOID* virtualFromPhysical(UINT64 physicalAddress);

/******************** Public Code ********************/

NTSTATUS MemManage_init(PMM_CONTEXT context, CR3 hostCR3)
{
	NTSTATUS status = STATUS_SUCCESS;

	/* Reserve a single page, this will be used for mapping in the guest
		* page data into. */
	PVOID reservedPage = MmAllocateMappingAddress(PAGE_SIZE, POOL_TAG);
	if (NULL != reservedPage)
	{
		/* Attempt to get the page table entry of the reserved page,
			* we need to ensure this is not a 2MB large page, if so we must split it. */
		MM_LEVEL tableLevel;
		PT_ENTRY_64* reservedPagePTE = getSystemPTEFromVA(hostCR3, reservedPage, &tableLevel);
		if (MM_LEVEL_PDE == tableLevel)
		{
			/* A split must take place. */
			status = split2MbPage((PDE_2MB_64*)reservedPagePTE);

			/* Get the new PTE. */
			reservedPagePTE = getSystemPTEFromVA(hostCR3, reservedPage, &tableLevel);
		}

		/* Ensure we are still in success state, splitting could have failed. */
		if (NT_SUCCESS(status))
		{
			/* Drop the translation of the virtual address,
			 * If at any point we observe it at 0, we know nothing is "mapped". */
			reservedPagePTE->Flags = 0;

			context->reservedPage = reservedPage;
			context->reservedPagePte = (PTE_64*)reservedPagePTE;
		}
	}
	else
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
	}

	/* If an error took place during initialisation, free
		* all allocated memory to prevent leaks. */
	if (NT_ERROR(status))
	{
		if (NULL != reservedPage)
		{
			MmFreeMappingAddress(reservedPage, POOL_TAG);
		}
	}

	return status;
}

void MemManage_uninit(PMM_CONTEXT context)
{
	MmFreeMappingAddress(context->reservedPage, POOL_TAG);
}

NTSTATUS MemManage_readVirtualAddress(PMM_CONTEXT context, CR3 tableBase, GUEST_VIRTUAL_ADDRESS guestVA, PVOID buffer, SIZE_T size)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	GUEST_VIRTUAL_ADDRESS currentVA = guestVA;
	PUINT8 currentBuffer = buffer;

	while (0 != size)
	{
		/* We can only read one page at a time, therfore we need
		 * to split our actions into a per page business. */
		SIZE_T bytesThisPage = size;

		/* Now trim so the first time in the loop we only do to a page boundary. */
		if (currentVA & (PAGE_SIZE - 1))
		{
			SIZE_T misalignedBy = PAGE_SIZE - (currentVA & (PAGE_SIZE - 1));

			if (bytesThisPage > misalignedBy)
			{
				bytesThisPage = misalignedBy;
			}
		}

		/* If we still have more than a page left, only read a page this time. */
		if (bytesThisPage > PAGE_SIZE)
		{
			bytesThisPage = PAGE_SIZE;
		}

		/* Get the physical address of the guest memory. */
		HOST_PHYS_ADDRESS physHost = GuestShim_GuestUVAToHPA(context, tableBase, currentVA);
		if (0 != physHost)
		{
			/* Read the memory. */
			status = MemManage_readPhysicalAddress(context, physHost, currentBuffer, bytesThisPage);

			if (NT_SUCCESS(status))
			{
				/* Increment pointer and subtract size remaining. */
				currentVA += bytesThisPage;
				currentBuffer += bytesThisPage;
				size -= bytesThisPage;
			}
		}
	}

	return status;
}

NTSTATUS MemManage_writeVirtualAddress(PMM_CONTEXT context, CR3 tableBase, GUEST_VIRTUAL_ADDRESS guestVA, PVOID buffer, SIZE_T size)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	GUEST_VIRTUAL_ADDRESS currentVA = guestVA;
	PUINT8 currentBuffer = buffer;

	while (0 != size)
	{
		/* We can only read one page at a time, therfore we need
		 * to split our actions into a per page business. */
		SIZE_T bytesThisPage = size;

		/* Now trim so the first time in the loop we only do to a page boundary. */
		if (currentVA & (PAGE_SIZE - 1))
		{
			SIZE_T misalignedBy = PAGE_SIZE - (currentVA & (PAGE_SIZE - 1));

			if (bytesThisPage > misalignedBy)
			{
				bytesThisPage = misalignedBy;
			}
		}

		/* If we still have more than a page left, only read a page this time. */
		if (bytesThisPage > PAGE_SIZE)
		{
			bytesThisPage = PAGE_SIZE;
		}

		/* Get the physical address of the guest memory. */
		HOST_PHYS_ADDRESS physHost = GuestShim_GuestUVAToHPA(context, tableBase, currentVA);
		if (0 != physHost)
		{
			/* Write the memory. */
			status = MemManage_writePhysicalAddress(context, physHost, currentBuffer, bytesThisPage);

			if (NT_SUCCESS(status))
			{
				/* Increment pointer and subtract size remaining. */
				currentVA += bytesThisPage;
				currentBuffer += bytesThisPage;
				size -= bytesThisPage;
			}
		}
	}

	return status;
}

NTSTATUS MemManage_readPhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress, VOID* buffer, SIZE_T bytesToCopy)
{
	NTSTATUS status;

	/* Map the physical memory. */
	VOID* mappedVA = mapPhysicalAddress(context, physicalAddress);
	if (NULL != mappedVA)
	{
		/* Do the copy. */
		RtlCopyMemory(buffer, mappedVA, bytesToCopy);

		/* Unmap the physical memory. */
		unmapPhysicalAddress(context);
		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}

NTSTATUS MemManage_writePhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress, VOID* buffer, SIZE_T bytesToCopy)
{
	NTSTATUS status;

	/* Map the physical memory. */
	VOID* mappedVA = mapPhysicalAddress(context, physicalAddress);
	if (NULL != mappedVA)
	{
		/* Do the copy. */
		RtlCopyMemory(mappedVA, buffer, bytesToCopy);

		/* Unmap the physical memory. */
		unmapPhysicalAddress(context);
		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_NO_MEMORY;
	}

	return status;
}
CR3 MemManage_getPageTableBase(PEPROCESS process)
{
	/* As KVA shadowing is used for CR3 as a mitigation for spectre/meltdown
	* we cannot use the CR3 as it is a shadowed table instead. Directly
	* access to DirectoryTableBase of the process that caused
	* the VMExit.
	*
	* UserDirectoryTableBase at a fixed offset, EPROCESS->Pcb.UserDirectoryTableBase
	* This was found using the following windbg commands:
	* dt nt!_EPROCESS
	* dt nt!_KPROCESS
	*
	* This is subject to change between Windows versions. */

	/* If running as admin, UserDirectoryTableBase will be set as 1 and KVA shadowing will not be present,
	* therefor we should use the directory table base (kernel). Again due to KVA shadowing... */
	CR3 tableBase;

	tableBase = *((CR3*)((UINT64)process + OFFSET_USER_DIR_TABLE));
	tableBase.Flags &= ~0xFFF;
	if (0 == tableBase.Flags)
	{
		tableBase = *((CR3*)((UINT64)process + OFFSET_DIRECTORY_TABLE_BASE));
		tableBase.Flags &= ~0xFFF;
	}

	return tableBase;
}

/******************** Module Code ********************/

PT_ENTRY_64* getSystemPTEFromVA(CR3 tableBase, PVOID virtualAddress, MM_LEVEL* level)
{
	PT_ENTRY_64* result = NULL;

	/* Gather the indexes for the page tables from the VA. */
	UINT64 indexPML4 = ADDRMASK_PML4_INDEX(virtualAddress);
	UINT64 indexPML3 = ADDRMASK_PML3_INDEX(virtualAddress);
	UINT64 indexPML2 = ADDRMASK_PML2_INDEX(virtualAddress);
	UINT64 indexPML1 = ADDRMASK_PML1_INDEX(virtualAddress);

	/* Read the PML4 from the target. */
	PML4E_64* pml4 = virtualFromPhysical(tableBase.AddressOfPageDirectory * PAGE_SIZE);
	PML4E_64* pml4e = &pml4[indexPML4];

	result = (PT_ENTRY_64*)pml4e;
	*level = MM_LEVEL_PML4E;
	if (TRUE == pml4e->Present)
	{
		/* Read PML3 from the guest. */
		PDPTE_64* pdpt = virtualFromPhysical(pml4e->PageFrameNumber * PAGE_SIZE);
		PDPTE_64* pdpte = &pdpt[indexPML3];

		result = (PT_ENTRY_64*)pdpte;
		*level = MM_LEVEL_PDPTE;

		/* Only attempt to get lower level if present and not a large page. */
		if ((TRUE == pdpte->Present) && (FALSE == pdpte->LargePage))
		{
			/* Read PML2 from the guest. */
			PDE_64* pd = virtualFromPhysical(pdpte->PageFrameNumber * PAGE_SIZE);
			PDE_64* pde = &pd[indexPML2];

			result = (PT_ENTRY_64*)pde;
			*level = MM_LEVEL_PDE;
			if ((TRUE == pde->Present) && (FALSE == pde->LargePage))
			{
				/* Read PML1 from the guest. */
				PTE_64* pt = virtualFromPhysical(pde->PageFrameNumber * PAGE_SIZE);
				PTE_64* pte = &pt[indexPML1];

				result = (PT_ENTRY_64*)pte;
				*level = MM_LEVEL_PTE;
			}
		}
	}

	return result;
}

static VOID* mapPhysicalAddress(PMM_CONTEXT context, HOST_PHYS_ADDRESS physicalAddress)
{
	/* Map the requested physical address to our reserved page. */
	context->reservedPagePte->Present = TRUE;
	context->reservedPagePte->Write = TRUE;
	context->reservedPagePte->PageFrameNumber = physicalAddress / PAGE_SIZE;

	/* Invalidate the TLB entries so we don't get cached old data. */
	__invlpg(context->reservedPage);

	return (VOID*)(((PUINT8)context->reservedPage) + ADDRMASK_PML1_OFFSET(physicalAddress));
}

static void unmapPhysicalAddress(PMM_CONTEXT context)
{
	/* Clear the page entry and flush the cache (TLB). */
	context->reservedPagePte->Flags = 0;
	__invlpg(context->reservedPage);
}

static NTSTATUS split2MbPage(PDE_2MB_64* pdeLarge)
{
	NTSTATUS status;

	/* Allocate a new page table, this will be used for splitting the 2MB
	 * entry into 512 4kb pages, 512 8byte entries = one page. */
	PTE_64* pt = OsAllocateContiguousAlignedPages(NonPagedPool, PAGE_SIZE);
	if (NULL != pt)
	{
		/* Close the large page bit and then propagate the current permissions to
		 * all the entries in our newly allocated PT. */
		pdeLarge->LargePage = FALSE;
		__stosq((UINT64*)pt, pdeLarge->Flags, PAGING_TABLE_ENTRY_COUNT);

		/* Calculate the physical address of where the PDPTE will be. */
		UINT64 base = pdeLarge->PageFrameNumber * SIZE_2MB;

		/* Update the page frame mapping for each PTE. */
		for (SIZE_T i = 0; i < PAGING_TABLE_ENTRY_COUNT; i++)
		{
			UINT64 physAddrToMap = base + (i * PAGE_SIZE);
			pt[i].PageFrameNumber = physAddrToMap / PAGE_SIZE;
		}

		/* Now update the convert the largePDE to a standard entry. */
		PDE_64* pde = (PDE_64*)pdeLarge;
		pde->Reserved1 = 0;
		pde->Reserved2 = 0;
		pde->PageFrameNumber = physicalFromVirtual(pt) / PAGE_SIZE;
		status = STATUS_SUCCESS;
	}
	else
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
	}

	return status;
}

static UINT64 physicalFromVirtual(VOID* virtualAddress)
{
	return MmGetPhysicalAddress(virtualAddress).QuadPart;
}

static VOID* virtualFromPhysical(UINT64 physicalAddress)
{
	PHYSICAL_ADDRESS pa;
	pa.QuadPart = physicalAddress;

	return MmGetVirtualForPhysical(pa);
}
