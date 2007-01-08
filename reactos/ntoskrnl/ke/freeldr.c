/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/freeldr.c
 * PURPOSE:         FreeLDR Bootstrap Support
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* FreeLDR Module Data */
LOADER_MODULE KeLoaderModules[64];
ULONG KeLoaderModuleCount;
static CHAR KeLoaderModuleStrings[64][256];

/* FreeLDR Memory Data */
ADDRESS_RANGE KeMemoryMap[64];
ULONG KeMemoryMapRangeCount;
ULONG_PTR MmFreeLdrFirstKrnlPhysAddr, MmFreeLdrLastKrnlPhysAddr;
ULONG_PTR MmFreeLdrLastKernelAddress;
ULONG MmFreeLdrMemHigher;
ULONG MmFreeLdrPageDirectoryEnd;

/* FreeLDR Loader Data */
ROS_LOADER_PARAMETER_BLOCK KeRosLoaderBlock;
static CHAR KeLoaderCommandLine[256];
BOOLEAN AcpiTableDetected;

/* FreeLDR PE Hack Data */
extern LDR_DATA_TABLE_ENTRY HalModuleObject;

/* NT Loader Data */
LOADER_PARAMETER_BLOCK BldrLoaderBlock;
LOADER_PARAMETER_EXTENSION BldrExtensionBlock;
CHAR BldrCommandLine[256];
CHAR BldrArcBootPath[64];
CHAR BldrArcHalPath[64];
CHAR BldrNtHalPath[64];
CHAR BldrNtBootPath[64];
LDR_DATA_TABLE_ENTRY BldrModules[64];
MEMORY_ALLOCATION_DESCRIPTOR BldrMemoryDescriptors[64];
WCHAR BldrModuleStrings[64][260];
NLS_DATA_BLOCK BldrNlsDataBlock;
SETUP_LOADER_BLOCK BldrSetupBlock;
struct _boot_infos_t *BootInfo;

#ifdef _M_PPC
#include "font.h"
boot_infos_t PpcEarlybootInfo;
#endif

#define KSEG0_BASE 0x80000000

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
KiRosFrldrLpbToNtLpb(IN PROS_LOADER_PARAMETER_BLOCK RosLoaderBlock,
                     IN PLOADER_PARAMETER_BLOCK *NtLoaderBlock)
{
    PLOADER_PARAMETER_BLOCK LoaderBlock;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR MdEntry;
    PLOADER_MODULE RosEntry;
    ULONG i, j, ModSize;
    PVOID ModStart;
    PCHAR DriverName;
    PCHAR BootPath, HalPath;
    CHAR CommandLine[256];

    /* First get some kernel-loader globals */
    AcpiTableDetected = (RosLoaderBlock->Flags & MB_FLAGS_ACPI_TABLE) ? TRUE : FALSE;
    MmFreeLdrMemHigher = RosLoaderBlock->MemHigher;
    MmFreeLdrPageDirectoryEnd = RosLoaderBlock->PageDirectoryEnd;
    KeLoaderModuleCount = RosLoaderBlock->ModsCount;

    /* Set the NT Loader block and initialize it */
    *NtLoaderBlock = LoaderBlock = &BldrLoaderBlock;
    RtlZeroMemory(LoaderBlock, sizeof(LOADER_PARAMETER_BLOCK));

    /* Set the NLS Data block */
    LoaderBlock->NlsData = &BldrNlsDataBlock;

    /* Assume this is from FreeLDR's SetupLdr */
    LoaderBlock->SetupLdrBlock = &BldrSetupBlock;

    /* Setup the list heads */
    InitializeListHead(&LoaderBlock->LoadOrderListHead);
    InitializeListHead(&LoaderBlock->MemoryDescriptorListHead);
    InitializeListHead(&LoaderBlock->BootDriverListHead);

    /* Loop boot driver list */
    for (i = 0; i < KeLoaderModuleCount; i++)
    {
        /* Get the ROS loader entry */
        RosEntry = &KeLoaderModules[i];
        DriverName = (PCHAR)RosEntry->String;
        ModStart = (PVOID)RosEntry->ModStart;
        ModSize = RosEntry->ModEnd - (ULONG_PTR)ModStart;

        /* Check if this is any of the NLS files */
        if (!_stricmp(DriverName, "ansi.nls"))
        {
            /* ANSI Code page */
            LoaderBlock->NlsData->AnsiCodePageData = ModStart;

            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderNlsData;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
            continue;
        }
        else if (!_stricmp(DriverName, "oem.nls"))
        {
            /* OEM Code page */
            LoaderBlock->NlsData->OemCodePageData = ModStart;

            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderNlsData;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
            continue;
        }
        else if (!_stricmp(DriverName, "casemap.nls"))
        {
            /* Unicode Code page */
            LoaderBlock->NlsData->UnicodeCodePageData = ModStart;

            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderNlsData;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
            continue;
        }

        /* Check if this is the SYSTEM hive */
        if (!(_stricmp(DriverName, "system")) ||
            !(_stricmp(DriverName, "system.hiv")))
        {
            /* Save registry data */
            LoaderBlock->RegistryBase = ModStart;
            LoaderBlock->RegistryLength = ModSize;

            /* Disable setup mode */
            LoaderBlock->SetupLdrBlock = NULL;

            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderRegistryData;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
            continue;
        }

        /* Check if this is the HARDWARE hive */
        if (!(_stricmp(DriverName, "hardware")) ||
            !(_stricmp(DriverName, "hardware.hiv")))
        {
            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderRegistryData;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
            continue;
        }

        /* Setup the loader entry */
        LdrEntry = &BldrModules[i];
        RtlZeroMemory(LdrEntry, sizeof(LDR_DATA_TABLE_ENTRY));

        /* Convert driver name from ANSI to Unicode */
        for (j = 0; j < strlen(DriverName); j++)
        {
            BldrModuleStrings[i][j] = DriverName[j];
        }

        /* Setup driver name */
        RtlInitUnicodeString(&LdrEntry->BaseDllName, BldrModuleStrings[i]);
        RtlInitUnicodeString(&LdrEntry->FullDllName, BldrModuleStrings[i]);

        /* Copy data from Freeldr Module Entry */
        LdrEntry->DllBase = ModStart;
        LdrEntry->SizeOfImage = ModSize;

        /* Initialize other data */
        LdrEntry->LoadCount = 1;
        LdrEntry->Flags = LDRP_IMAGE_DLL |
                          LDRP_ENTRY_PROCESSED;
        if (RosEntry->Reserved) LdrEntry->Flags |= LDRP_ENTRY_INSERTED;

        /* Insert it into the loader block */
        InsertTailList(&LoaderBlock->LoadOrderListHead,
                       &LdrEntry->InLoadOrderLinks);

        /* Check if this is the kernel */
        if (!(_stricmp(DriverName, "ntoskrnl.exe")))
        {
            /* Create an MD for it */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderSystemCode;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
        }
        else if (!(_stricmp(DriverName, "hal.dll")))
        {
            /* The HAL actually gets loaded somewhere else */
            ModStart = HalModuleObject.DllBase;

            /* Create an MD for the HAL */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderHalCode;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
        }
        else
        {
            /* Create an MD for any driver */
            MdEntry = &BldrMemoryDescriptors[i];
            MdEntry->MemoryType = LoaderBootDriver;
            MdEntry->BasePage = (ULONG_PTR)ModStart >> PAGE_SHIFT;
            MdEntry->PageCount = ModSize >> PAGE_SHIFT;
            InsertTailList(&LoaderBlock->MemoryDescriptorListHead,
                           &MdEntry->ListEntry);
        }
    }

    /* Setup command line */
    LoaderBlock->LoadOptions = BldrCommandLine;
    strcpy(BldrCommandLine, KeLoaderCommandLine);

    /* Setup the extension block */
    LoaderBlock->Extension = &BldrExtensionBlock;
    LoaderBlock->Extension->Size = sizeof(LOADER_PARAMETER_EXTENSION);
    LoaderBlock->Extension->MajorVersion = 5;
    LoaderBlock->Extension->MinorVersion = 2;

    /* Now setup the setup block if we have one */
    if (LoaderBlock->SetupLdrBlock)
    {
        /* All we'll setup right now is the flag for text-mode setup */
        LoaderBlock->SetupLdrBlock->Flags = 1;
    }

    /* Make a copy of the command line */
    strcpy(CommandLine, LoaderBlock->LoadOptions);

    /* Find the first \, separating the ARC path from NT path */
    BootPath = strchr(CommandLine, '\\');
    *BootPath = ANSI_NULL;
    strncpy(BldrArcBootPath, CommandLine, 63);
    LoaderBlock->ArcBootDeviceName = BldrArcBootPath;

    /* The rest of the string is the NT path */
    HalPath = strchr(BootPath + 1, ' ');
    *HalPath = ANSI_NULL;
    BldrNtBootPath[0] = '\\';
    strncat(BldrNtBootPath, BootPath + 1, 63);
    strcat(BldrNtBootPath,"\\");
    LoaderBlock->NtBootPathName = BldrNtBootPath;

    /* Set the HAL paths */
    strncpy(BldrArcHalPath, BldrArcBootPath, 63);
    LoaderBlock->ArcHalDeviceName = BldrArcHalPath;
    strcpy(BldrNtHalPath, "\\");
    LoaderBlock->NtHalPathName = BldrNtHalPath;

    /* Use this new command line */
    strncpy(LoaderBlock->LoadOptions, HalPath + 2, 255);

    /* Parse it and change every slash to a space */
    BootPath = LoaderBlock->LoadOptions;
    do {if (*BootPath == '/') *BootPath = ' ';} while (*BootPath++);
}

VOID
INIT_FUNCTION
NTAPI
LdrpSettleHal(PVOID NewHalBase);

VOID
FASTCALL
KiRosPrepareForSystemStartup(IN ULONG Dummy,
                             IN PROS_LOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG i;
    ULONG size;
    ULONG StartKernelBase;
    ULONG HalBase;
    ULONG DriverBase;
    ULONG DriverSize;
    PLOADER_PARAMETER_BLOCK NtLoaderBlock;
    CHAR* s;
#ifdef _M_IX86
    PKTSS Tss;
    PKGDTENTRY TssEntry;
#endif
#ifdef _M_PPC
    { 
	__asm__("ori 0,0,0");
	char *nk = "ntoskrnl is here";
	boot_infos_t *XBootInfo = (boot_infos_t *)LoaderBlock->ArchExtra;
	memcpy(&PpcEarlybootInfo, XBootInfo, sizeof(PpcEarlybootInfo));
	PpcEarlybootInfo.dispFont = BootDigits;
	BootInfo = (struct _boot_infos_t *)&PpcEarlybootInfo;
	DrawNumber(BootInfo, 0x1234abcd, 10, 100);
	DrawNumber(BootInfo, (ULONG)nk, 10 , 150);
	DrawString(BootInfo, nk, 100, 150);
	__asm__("ori 0,0,0");
    }
#endif

#ifdef _M_IX86
    /* Load the GDT and IDT */
    Ke386SetGlobalDescriptorTable(KiGdtDescriptor);
    Ke386SetInterruptDescriptorTable(KiIdtDescriptor);

    /* Initialize the boot TSS */
    Tss = &KiBootTss;
    TssEntry = &KiBootGdt[KGDT_TSS / sizeof(KGDTENTRY)];
    TssEntry->HighWord.Bits.Type = I386_TSS;
    TssEntry->HighWord.Bits.Pres = 1;
    TssEntry->HighWord.Bits.Dpl = 0;
    TssEntry->BaseLow = (USHORT)((ULONG_PTR)Tss & 0xFFFF);
    TssEntry->HighWord.Bytes.BaseMid = (UCHAR)((ULONG_PTR)Tss >> 16);
    TssEntry->HighWord.Bytes.BaseHi = (UCHAR)((ULONG_PTR)Tss >> 24);
#endif

    DrawNumber(BootInfo, 0xb0071f03, 190, 90);
    DrawNumber(BootInfo, (ULONG)BootInfo, 190, 100);

    /* Copy the Loader Block Data locally since Low-Memory will be wiped */
    memcpy(&KeRosLoaderBlock, LoaderBlock, sizeof(ROS_LOADER_PARAMETER_BLOCK));
    memcpy(&KeLoaderModules[0],
           (PVOID)KeRosLoaderBlock.ModsAddr,
           sizeof(LOADER_MODULE) * KeRosLoaderBlock.ModsCount);
    KeRosLoaderBlock.ModsAddr = (ULONG)&KeLoaderModules;

    /* Check for BIOS memory map */
    KeMemoryMapRangeCount = 0;
    if (KeRosLoaderBlock.Flags & MB_FLAGS_MMAP_INFO)
    {
        /* We have a memory map from the nice BIOS */
        size = *((PULONG)(KeRosLoaderBlock.MmapAddr - sizeof(ULONG)));
        i = 0;

        /* Map it until we run out of size */
        while (i < KeRosLoaderBlock.MmapLength)
        {
            /* Copy into the Kernel Memory Map */
            memcpy (&KeMemoryMap[KeMemoryMapRangeCount],
                    (PVOID)(KeRosLoaderBlock.MmapAddr + i),
                    sizeof(ADDRESS_RANGE));

            /* Increase Memory Map Count */
            KeMemoryMapRangeCount++;

            /* Increase Size */
            i += size;
        }

        /* Save data */
        KeRosLoaderBlock.MmapLength = KeMemoryMapRangeCount *
                                   sizeof(ADDRESS_RANGE);
        KeRosLoaderBlock.MmapAddr = (ULONG)KeMemoryMap;
    }
    else
    {
        /* Nothing from BIOS */
        KeRosLoaderBlock.MmapLength = 0;
        KeRosLoaderBlock.MmapAddr = (ULONG)KeMemoryMap;
    }

    /* Save the Base Address */
    MmSystemRangeStart = (PVOID)KeRosLoaderBlock.KernelBase;

    /* Set the Command Line */
    strcpy(KeLoaderCommandLine, (PCHAR)LoaderBlock->CommandLine);
    KeRosLoaderBlock.CommandLine = (ULONG)KeLoaderCommandLine;

    /* Get the address of ntoskrnl in openfirmware memory */
    StartKernelBase = KeLoaderModules[0].ModStart;

    /* Create a block for each module */
    for (i = 0; i < KeRosLoaderBlock.ModsCount; i++)
    {
        /* Check if we have to copy the path or not */
        if ((s = strrchr((PCHAR)KeLoaderModules[i].String, '/')) != 0)
        {
            strcpy(KeLoaderModuleStrings[i], s + 1);
        }
        else
        {
            strcpy(KeLoaderModuleStrings[i], (PCHAR)KeLoaderModules[i].String);
        }

#ifdef _M_PPC
	if(i == 0) {
	    DrawNumber(BootInfo, KeLoaderModules[i].ModStart, 10, 200);
	    DrawNumber(BootInfo, KeLoaderModules[i].ModEnd - KeLoaderModules[i].ModStart, 100, 200);
	    DrawNumber(BootInfo, KeLoaderModules[i].ModEnd, 190, 200);
	    DrawNumber(BootInfo, KeLoaderModules[i+1].ModStart, 10, 210);
	    DrawNumber(BootInfo, KeLoaderModules[i+1].ModEnd - KeLoaderModules[i+1].ModStart, 100, 210);
	    DrawNumber(BootInfo, KeLoaderModules[i+1].ModEnd, 190, 210);
	}
	KeLoaderModules[i].ModStart += KSEG0_BASE - StartKernelBase;
	KeLoaderModules[i].ModEnd   += KSEG0_BASE - StartKernelBase;
#else
        /* Substract the base Address in Physical Memory */
        KeLoaderModules[i].ModStart -= 0x200000;

        /* Add the Kernel Base Address in Virtual Memory */
        KeLoaderModules[i].ModStart += KSEG0_BASE;

        /* Substract the base Address in Physical Memory */
        KeLoaderModules[i].ModEnd -= 0x200000;

        /* Add the Kernel Base Address in Virtual Memory */
        KeLoaderModules[i].ModEnd += KSEG0_BASE;
#endif

        /* Select the proper String */
        KeLoaderModules[i].String = (ULONG)KeLoaderModuleStrings[i];
    }

    /* Choose last module address as the final kernel address */
    MmFreeLdrLastKernelAddress =
        PAGE_ROUND_UP(KeLoaderModules[KeRosLoaderBlock.ModsCount - 1].ModEnd);

#ifndef _M_PPC
    /* Select the HAL Base */
    HalBase = KeLoaderModules[1].ModStart;

    /* Choose Driver Base */
    DriverBase = MmFreeLdrLastKernelAddress;
    LdrHalBase = (ULONG_PTR)DriverBase;
#else
    HalBase = KeLoaderModules[1].ModStart;
    DriverBase = MmFreeLdrLastKernelAddress;
    LdrHalBase = KeLoaderModules[1].ModStart;
#endif

    /* Initialize Module Management */
    LdrInitModuleManagement((PVOID)KeLoaderModules[0].ModStart);

    /* Load HAL.DLL with the PE Loader */
    LdrSafePEProcessModule((PVOID)HalBase,
                            (PVOID)DriverBase,
                            (PVOID)KeLoaderModules[0].ModStart,
                            &DriverSize);

#ifdef _M_PPC
    LdrpSettleHal((PVOID)DriverBase);
#endif

    /* Increase the last kernel address with the size of HAL */
    MmFreeLdrLastKernelAddress += PAGE_ROUND_UP(DriverSize);

#ifdef _M_IX86
    /* Now select the final beginning and ending Kernel Addresses */
    MmFreeLdrFirstKrnlPhysAddr = KeLoaderModules[0].ModStart -
                                 KSEG0_BASE + 0x200000;
    MmFreeLdrLastKrnlPhysAddr = MmFreeLdrLastKernelAddress -
                                KSEG0_BASE + 0x200000;
#endif

    /* Setup the IDT */
    KeInitExceptions(); // ONCE HACK BELOW IS GONE, MOVE TO KISYSTEMSTARTUP!
    KeInitInterrupts(); // ROS HACK DEPRECATED SOON BY NEW HAL

    /* Load the Kernel with the PE Loader */
    LdrSafePEProcessModule((PVOID)KeLoaderModules[0].ModStart,
                           (PVOID)KeLoaderModules[0].ModStart,
                           (PVOID)DriverBase,
                           &DriverSize);

    /* Sets up the VDM Data */
#ifdef _M_IX86
    NtEarlyInitVdm();
#endif

    /* Convert the loader block */
    KiRosFrldrLpbToNtLpb(&KeRosLoaderBlock, &NtLoaderBlock);

    /* Do general System Startup */
    KiSystemStartup(NtLoaderBlock);
}

/* EOF */
