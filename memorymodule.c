/*
 * Memory DLL loading code
 * Version 0.0.4
 *
 * Copyright (c) 2004-2015 by Joachim Bauch / mail@joachim-bauch.de
 * http://www.joachim-bauch.de
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 2.0 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is MemoryModule.c
 *
 * The Initial Developer of the Original Code is Joachim Bauch.
 *
 * Portions created by Joachim Bauch are Copyright (C) 2004-2015
 * Joachim Bauch. All Rights Reserved.
 *
 *
 * THeller: Added binary search in MemoryGetProcAddress function
 * (#define USE_BINARY_SEARCH to enable it).  This gives a very large
 * speedup for libraries that exports lots of functions.
 *
 * These portions are Copyright (C) 2013 Thomas Heller.
 */

#include <windows.h>
#include <winnt.h>
#include <stddef.h>
#include <tchar.h>

#ifndef IMAGE_SIZEOF_BASE_RELOCATION
// Vista SDKs no longer define IMAGE_SIZEOF_BASE_RELOCATION!?
#define IMAGE_SIZEOF_BASE_RELOCATION (sizeof(IMAGE_BASE_RELOCATION))
#endif

#ifdef _WIN64
#define HOST_MACHINE IMAGE_FILE_MACHINE_AMD64
#else
#define HOST_MACHINE IMAGE_FILE_MACHINE_I386
#endif

#include "MemoryModule.h"

struct ExportNameEntry {
    LPCSTR name;
    WORD idx;
};

typedef BOOL (WINAPI *DllEntryProc)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);
typedef int (WINAPI *ExeEntryProc)(void);

#ifdef _WIN64
typedef struct POINTER_LIST {
    struct POINTER_LIST *next;
    void *address;
} POINTER_LIST;
#endif

typedef struct {
    PIMAGE_NT_HEADERS headers;
    unsigned char *codeBase;
    HCUSTOMMODULE *modules;
    int numModules;
    BOOL initialized;
    BOOL isDLL;
    BOOL isRelocated;
    struct ExportNameEntry *nameExportsTable;
    ExeEntryProc exeEntry;
    DWORD pageSize;
#ifdef _WIN64
    POINTER_LIST *blockedMemory;
#endif
} MEMORYMODULE, *PMEMORYMODULE;

typedef struct {
    LPVOID address;
    LPVOID alignedAddress;
    SIZE_T size;
    DWORD characteristics;
    BOOL last;
} SECTIONFINALIZEDATA, *PSECTIONFINALIZEDATA;

#define GET_HEADER_DICTIONARY(module, idx)  &(module)->headers->OptionalHeader.DataDirectory[idx]

static inline uintptr_t
AlignValueDown(uintptr_t value, uintptr_t alignment) {
    return value & ~(alignment - 1);
}

static inline LPVOID
AlignAddressDown(LPVOID address, uintptr_t alignment) {
    return (LPVOID) AlignValueDown((uintptr_t) address, alignment);
}

static inline size_t
AlignValueUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline void*
OffsetPointer(void* data, ptrdiff_t offset) {
    return (void*) ((uintptr_t) data + offset);
}

static inline void
OutputLastError(const char *msg)
{
    UNREFERENCED_PARAMETER(msg);
}

#ifdef _WIN64
static void
FreePointerList(POINTER_LIST *head, CustomFreeFunc freeMemory)
{
    POINTER_LIST *node = head;
    while (node) {
        freeMemory(node->address, 0, MEM_RELEASE);
        POINTER_LIST* next = node->next;
        free(node);
        node = next;
    }
}
#endif

static BOOL
CheckSize(size_t size, size_t expected) {
    if (size < expected) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    return TRUE;
}

static BOOL
CopySections(const unsigned char *data, size_t size, PIMAGE_NT_HEADERS old_headers, PMEMORYMODULE module)
{
    unsigned char *codeBase = module->codeBase;
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
    for (int i=0; i<module->headers->FileHeader.NumberOfSections; i++, section++) {
        if (section->SizeOfRawData == 0) {
            // section doesn't contain data in the dll itself, but may define
            // uninitialized data
            int section_size = old_headers->OptionalHeader.SectionAlignment;
            if (section_size > 0) {
                unsigned char* dest = (unsigned char *)VirtualAlloc(codeBase + section->VirtualAddress,
                    section_size,
                    MEM_COMMIT,
                    PAGE_READWRITE);
                if (dest == NULL) {
                    return FALSE;
                }

                // Always use position from file to support alignments smaller
                // than page size (allocation above will align to page size).
                dest = codeBase + section->VirtualAddress;
                // NOTE: On 64bit systems we truncate to 32bit here but expand
                // again later when "PhysicalAddress" is used.
                section->Misc.PhysicalAddress = (DWORD) ((uintptr_t) dest & 0xffffffff);
                memset(dest, 0, section_size);
            }

            // section is empty
            continue;
        }

        if (!CheckSize(size, section->PointerToRawData + section->SizeOfRawData)) {
            return FALSE;
        }

        // commit memory block and copy data from dll
        unsigned char* dest = (unsigned char *)VirtualAlloc(codeBase + section->VirtualAddress,
                            section->SizeOfRawData,
                            MEM_COMMIT,
                            PAGE_READWRITE);
        if (dest == NULL) {
            return FALSE;
        }

        // Always use position from file to support alignments smaller
        // than page size (allocation above will align to page size).
        dest = codeBase + section->VirtualAddress;
        memcpy(dest, data + section->PointerToRawData, section->SizeOfRawData);
        // NOTE: On 64bit systems we truncate to 32bit here but expand
        // again later when "PhysicalAddress" is used.
        section->Misc.PhysicalAddress = (DWORD) ((uintptr_t) dest & 0xffffffff);
    }

    return TRUE;
}

// Protection flags for memory pages (Executable, Readable, Writeable)
static int ProtectionFlags[2][2][2] = {
    {
        // not executable
        {PAGE_NOACCESS, PAGE_WRITECOPY},
        {PAGE_READONLY, PAGE_READWRITE},
    }, {
        // executable
        {PAGE_EXECUTE, PAGE_EXECUTE_WRITECOPY},
        {PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE},
    },
};

static SIZE_T
GetRealSectionSize(PMEMORYMODULE module, PIMAGE_SECTION_HEADER section) {
    DWORD size = section->SizeOfRawData;
    if (size == 0) {
        if (section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) {
            size = module->headers->OptionalHeader.SizeOfInitializedData;
        } else if (section->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
            size = module->headers->OptionalHeader.SizeOfUninitializedData;
        }
    }
    return (SIZE_T) size;
}

static BOOL
FinalizeSection(PMEMORYMODULE module, PSECTIONFINALIZEDATA sectionData) {
    if (sectionData->size == 0) {
        return TRUE;
    }

    if (sectionData->characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
        // section is not needed any more and can safely be freed
        if (sectionData->address == sectionData->alignedAddress &&
            (sectionData->last ||
             module->headers->OptionalHeader.SectionAlignment == module->pageSize ||
             (sectionData->size % module->pageSize) == 0)
           ) {
            // Only allowed to decommit whole pages
            VirtualFree(sectionData->address, sectionData->size, MEM_DECOMMIT);
        }
        return TRUE;
    }

    // determine protection flags based on characteristics
    BOOL executable = (sectionData->characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    BOOL readable =   (sectionData->characteristics & IMAGE_SCN_MEM_READ) != 0;
    BOOL writeable =  (sectionData->characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    DWORD protect = ProtectionFlags[executable][readable][writeable];
    if (sectionData->characteristics & IMAGE_SCN_MEM_NOT_CACHED) {
        protect |= PAGE_NOCACHE;
    }

    // change memory access flags
    DWORD oldProtect;
    if (VirtualProtect(sectionData->address, sectionData->size, protect, &oldProtect) == 0) {
        OutputLastError("Error protecting memory page");
        return FALSE;
    }

    return TRUE;
}

static BOOL
FinalizeSections(PMEMORYMODULE module)
{
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(module->headers);
#ifdef _WIN64
    // "PhysicalAddress" might have been truncated to 32bit above, expand to
    // 64bits again.
    uintptr_t imageOffset = ((uintptr_t) module->headers->OptionalHeader.ImageBase & 0xffffffff00000000);
#else
    static const uintptr_t imageOffset = 0;
#endif
    SECTIONFINALIZEDATA sectionData;
    sectionData.address = (LPVOID)((uintptr_t)section->Misc.PhysicalAddress | imageOffset);
    sectionData.alignedAddress = AlignAddressDown(sectionData.address, module->pageSize);
    sectionData.size = GetRealSectionSize(module, section);
    sectionData.characteristics = section->Characteristics;
    sectionData.last = FALSE;
    section++;

    // loop through all sections and change access flags
    for (int i=1; i<module->headers->FileHeader.NumberOfSections; i++, section++) {
        LPVOID sectionAddress = (LPVOID)((uintptr_t)section->Misc.PhysicalAddress | imageOffset);
        LPVOID alignedAddress = AlignAddressDown(sectionAddress, module->pageSize);
        SIZE_T sectionSize = GetRealSectionSize(module, section);
        // Combine access flags of all sections that share a page
        // TODO(fancycode): We currently share flags of a trailing large section
        //   with the page of a first small section. This should be optimized.
        if (sectionData.alignedAddress == alignedAddress || (uintptr_t) sectionData.address + sectionData.size > (uintptr_t) alignedAddress) {
            // Section shares page with previous
            if ((section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0 || (sectionData.characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0) {
                sectionData.characteristics = (sectionData.characteristics | section->Characteristics) & ~IMAGE_SCN_MEM_DISCARDABLE;
            } else {
                sectionData.characteristics |= section->Characteristics;
            }
            sectionData.size = (((uintptr_t)sectionAddress) + ((uintptr_t) sectionSize)) - (uintptr_t) sectionData.address;
            continue;
        }

        if (!FinalizeSection(module, &sectionData)) {
            return FALSE;
        }
        sectionData.address = sectionAddress;
        sectionData.alignedAddress = alignedAddress;
        sectionData.size = sectionSize;
        sectionData.characteristics = section->Characteristics;
    }
    sectionData.last = TRUE;
    if (!FinalizeSection(module, &sectionData)) {
        return FALSE;
    }
    return TRUE;
}

static BOOL
ExecuteTLS(PMEMORYMODULE module)
{
    unsigned char *codeBase = module->codeBase;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_TLS);
    if (directory->VirtualAddress == 0) {
        return TRUE;
    }

    PIMAGE_TLS_DIRECTORY tls = (PIMAGE_TLS_DIRECTORY) (codeBase + directory->VirtualAddress);
    PIMAGE_TLS_CALLBACK* callback = (PIMAGE_TLS_CALLBACK *) tls->AddressOfCallBacks;
    if (callback) {
        while (*callback) {
            (*callback)((LPVOID) codeBase, DLL_PROCESS_ATTACH, NULL);
            callback++;
        }
    }
    return TRUE;
}

static BOOL
PerformBaseRelocation(PMEMORYMODULE module, ptrdiff_t delta)
{
    unsigned char *codeBase = module->codeBase;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (directory->Size == 0) {
        return (delta == 0);
    }

    PIMAGE_BASE_RELOCATION relocation = (PIMAGE_BASE_RELOCATION) (codeBase + directory->VirtualAddress);
    for (; relocation->VirtualAddress > 0; ) {
        DWORD i;
        unsigned char *dest = codeBase + relocation->VirtualAddress;
        unsigned short *relInfo = (unsigned short*) OffsetPointer(relocation, IMAGE_SIZEOF_BASE_RELOCATION);
        for (i=0; i<((relocation->SizeOfBlock-IMAGE_SIZEOF_BASE_RELOCATION) / 2); i++, relInfo++) {
            // the upper 4 bits define the type of relocation
            int type = *relInfo >> 12;
            // the lower 12 bits define the offset
            int offset = *relInfo & 0xfff;

            switch (type)
            {
            case IMAGE_REL_BASED_ABSOLUTE:
                // skip relocation
                break;

            case IMAGE_REL_BASED_HIGHLOW:
                // change complete 32 bit address
                {
                    DWORD *patchAddrHL = (DWORD *) (dest + offset);
                    *patchAddrHL += (DWORD) delta;
                }
                break;

#ifdef _WIN64
            case IMAGE_REL_BASED_DIR64:
                {
                    ULONGLONG *patchAddr64 = (ULONGLONG *) (dest + offset);
                    *patchAddr64 += (ULONGLONG) delta;
                }
                break;
#endif

            default:
                //printf("Unknown relocation: %d\n", type);
                break;
            }
        }

        // advance to next relocation block
        relocation = (PIMAGE_BASE_RELOCATION) OffsetPointer(relocation, relocation->SizeOfBlock);
    }
    return TRUE;
}

static BOOL
BuildImportTable(PMEMORYMODULE module)
{
    unsigned char *codeBase = module->codeBase;
    BOOL result = TRUE;

    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (directory->Size == 0) {
        return TRUE;
    }

    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR) (codeBase + directory->VirtualAddress);
    for (; !IsBadReadPtr(importDesc, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && importDesc->Name; importDesc++) {
        uintptr_t *thunkRef;
        FARPROC *funcRef;
        HCUSTOMMODULE *tmp;
        HCUSTOMMODULE handle = LoadLibraryA((LPCSTR) (codeBase + importDesc->Name));
        if (handle == NULL) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            result = FALSE;
            break;
        }

        tmp = (HCUSTOMMODULE *) realloc(module->modules, (module->numModules+1)*(sizeof(HCUSTOMMODULE)));
        if (tmp == NULL) {
            FreeLibrary(handle);
            SetLastError(ERROR_OUTOFMEMORY);
            result = FALSE;
            break;
        }
        module->modules = tmp;

        module->modules[module->numModules++] = handle;
        if (importDesc->OriginalFirstThunk) {
            thunkRef = (uintptr_t *) (codeBase + importDesc->OriginalFirstThunk);
            funcRef = (FARPROC *) (codeBase + importDesc->FirstThunk);
        } else {
            // no hint table
            thunkRef = (uintptr_t *) (codeBase + importDesc->FirstThunk);
            funcRef = (FARPROC *) (codeBase + importDesc->FirstThunk);
        }
        for (; *thunkRef; thunkRef++, funcRef++) {
            if (IMAGE_SNAP_BY_ORDINAL(*thunkRef)) {
                *funcRef = GetProcAddress(handle, (LPCSTR)IMAGE_ORDINAL(*thunkRef));
            } else {
                PIMAGE_IMPORT_BY_NAME thunkData = (PIMAGE_IMPORT_BY_NAME) (codeBase + (*thunkRef));
                *funcRef = GetProcAddress(handle, (LPCSTR)&thunkData->Name);
            }
            if (*funcRef == 0) {
                result = FALSE;
                break;
            }
        }

        if (!result) {
            FreeLibrary(handle);
            SetLastError(ERROR_PROC_NOT_FOUND);
            break;
        }
    }

    return result;
}

HMEMORYMODULE MemoryLoadLibrary(const void *data, size_t size)
{
    PMEMORYMODULE result = NULL;
#ifdef _WIN64
    POINTER_LIST *blockedMemory = NULL;
#endif

    if (!CheckSize(size, sizeof(IMAGE_DOS_HEADER))) {
        return NULL;
    }
    PIMAGE_DOS_HEADER dos_header = (PIMAGE_DOS_HEADER)data;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    if (!CheckSize(size, dos_header->e_lfanew + sizeof(IMAGE_NT_HEADERS))) {
        return NULL;
    }
    PIMAGE_NT_HEADERS old_header = (PIMAGE_NT_HEADERS)&((const unsigned char *)(data))[dos_header->e_lfanew];
    if (old_header->Signature != IMAGE_NT_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    if (old_header->FileHeader.Machine != HOST_MACHINE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    if (old_header->OptionalHeader.SectionAlignment & 1) {
        // Only support section alignments that are a multiple of 2
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(old_header);
    size_t optionalSectionSize = old_header->OptionalHeader.SectionAlignment;
    size_t lastSectionEnd = 0;
    for (DWORD i=0; i<old_header->FileHeader.NumberOfSections; i++, section++) {
        size_t endOfSection;
        if (section->SizeOfRawData == 0) {
            // Section without data in the DLL
            endOfSection = section->VirtualAddress + optionalSectionSize;
        } else {
            endOfSection = section->VirtualAddress + section->SizeOfRawData;
        }

        if (endOfSection > lastSectionEnd) {
            lastSectionEnd = endOfSection;
        }
    }

    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);
    size_t alignedImageSize = AlignValueUp(old_header->OptionalHeader.SizeOfImage, sysInfo.dwPageSize);
    if (alignedImageSize != AlignValueUp(lastSectionEnd, sysInfo.dwPageSize)) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    // reserve memory for image of library
    // XXX: is it correct to commit the complete memory region at once?
    //      calling DllEntry raises an exception if we don't...
    unsigned char* code = (unsigned char *)VirtualAlloc((LPVOID)(old_header->OptionalHeader.ImageBase),
        alignedImageSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (code == NULL) {
        // try to allocate memory at arbitrary position
        code = (unsigned char *)VirtualAlloc(NULL,
            alignedImageSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (code == NULL) {
            SetLastError(ERROR_OUTOFMEMORY);
            return NULL;
        }
    }

#ifdef _WIN64
    // Memory block may not span 4 GB boundaries.
    while ((((uintptr_t) code) >> 32) < (((uintptr_t) (code + alignedImageSize)) >> 32)) {
        POINTER_LIST *node = (POINTER_LIST*) malloc(sizeof(POINTER_LIST));
        if (!node) {
            VirtualFree(code, 0, MEM_RELEASE);
            FreePointerList(blockedMemory, VirtualFree);
            SetLastError(ERROR_OUTOFMEMORY);
            return NULL;
        }

        node->next = blockedMemory;
        node->address = code;
        blockedMemory = node;

        code = (unsigned char *)VirtualAlloc(NULL,
            alignedImageSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (code == NULL) {
            FreePointerList(blockedMemory, VirtualFree);
            SetLastError(ERROR_OUTOFMEMORY);
            return NULL;
        }
    }
#endif

    result = (PMEMORYMODULE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MEMORYMODULE));
    if (result == NULL) {
        VirtualFree(code, 0, MEM_RELEASE);
#ifdef _WIN64
        FreePointerList(blockedMemory, VirtualFree);
#endif
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    result->codeBase = code;
    result->isDLL = (old_header->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
    result->pageSize = sysInfo.dwPageSize;
#ifdef _WIN64
    result->blockedMemory = blockedMemory;
#endif

    if (!CheckSize(size, old_header->OptionalHeader.SizeOfHeaders)) {
        goto error;
    }

    // commit memory for headers
    unsigned char* headers = (unsigned char *)VirtualAlloc(code,
        old_header->OptionalHeader.SizeOfHeaders,
        MEM_COMMIT,
        PAGE_READWRITE);

    // copy PE header to code
    memcpy(headers, dos_header, old_header->OptionalHeader.SizeOfHeaders);
    result->headers = (PIMAGE_NT_HEADERS)&((const unsigned char *)(headers))[dos_header->e_lfanew];

    // update position
    result->headers->OptionalHeader.ImageBase = (uintptr_t)code;

    // copy sections from DLL file block to new memory location
    if (!CopySections((const unsigned char *) data, size, old_header, result)) {
        goto error;
    }

    // adjust base address of imported data
    ptrdiff_t locationDelta = (ptrdiff_t)(result->headers->OptionalHeader.ImageBase - old_header->OptionalHeader.ImageBase);
    if (locationDelta != 0) {
        result->isRelocated = PerformBaseRelocation(result, locationDelta);
    } else {
        result->isRelocated = TRUE;
    }

    // load required dlls and adjust function table of imports
    if (!BuildImportTable(result)) {
        goto error;
    }

    // mark memory pages depending on section headers and release
    // sections that are marked as "discardable"
    if (!FinalizeSections(result)) {
        goto error;
    }

    // TLS callbacks are executed BEFORE the main loading
    if (!ExecuteTLS(result)) {
        goto error;
    }

    // get entry point of loaded library
    if (result->headers->OptionalHeader.AddressOfEntryPoint != 0) {
        if (result->isDLL) {
            DllEntryProc DllEntry = (DllEntryProc)(LPVOID)(code + result->headers->OptionalHeader.AddressOfEntryPoint);
            // notify library about attaching to process
            BOOL successfull = (*DllEntry)((HINSTANCE)code, DLL_PROCESS_ATTACH, 0);
            if (!successfull) {
                SetLastError(ERROR_DLL_INIT_FAILED);
                goto error;
            }
            result->initialized = TRUE;
        } else {
            result->exeEntry = (ExeEntryProc)(LPVOID)(code + result->headers->OptionalHeader.AddressOfEntryPoint);
        }
    } else {
        result->exeEntry = NULL;
    }

    return (HMEMORYMODULE)result;

error:
    // cleanup
    MemoryFreeLibrary(result);
    return NULL;
}

static int _compare(const void *a, const void *b)
{
    const struct ExportNameEntry *p1 = (const struct ExportNameEntry*) a;
    const struct ExportNameEntry *p2 = (const struct ExportNameEntry*) b;
    return strcmp(p1->name, p2->name);
}

static int _find(const void *a, const void *b)
{
    LPCSTR *name = (LPCSTR *) a;
    const struct ExportNameEntry *p = (const struct ExportNameEntry*) b;
    return strcmp(*name, p->name);
}

FARPROC MemoryGetProcAddress(HMEMORYMODULE mod, LPCSTR name)
{
    PMEMORYMODULE module = (PMEMORYMODULE)mod;
    unsigned char *codeBase = module->codeBase;
    DWORD idx = 0;
    PIMAGE_DATA_DIRECTORY directory = GET_HEADER_DICTIONARY(module, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (directory->Size == 0) {
        // no export table found
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }

    PIMAGE_EXPORT_DIRECTORY exports = (PIMAGE_EXPORT_DIRECTORY) (codeBase + directory->VirtualAddress);
    if (exports->NumberOfNames == 0 || exports->NumberOfFunctions == 0) {
        // DLL doesn't export anything
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }

    if (HIWORD(name) == 0) {
        // load function by ordinal value
        if (LOWORD(name) < exports->Base) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL;
        }

        idx = LOWORD(name) - exports->Base;
    } else if (!exports->NumberOfNames) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    } else {
        const struct ExportNameEntry *found;

        // Lazily build name table and sort it by names
        if (!module->nameExportsTable) {
            DWORD *nameRef = (DWORD *) (codeBase + exports->AddressOfNames);
            WORD *ordinal = (WORD *) (codeBase + exports->AddressOfNameOrdinals);
            struct ExportNameEntry *entry = (struct ExportNameEntry*) malloc(exports->NumberOfNames * sizeof(struct ExportNameEntry));
            module->nameExportsTable = entry;
            if (!entry) {
                SetLastError(ERROR_OUTOFMEMORY);
                return NULL;
            }
            for (DWORD i=0; i<exports->NumberOfNames; i++, nameRef++, ordinal++, entry++) {
                entry->name = (const char *) (codeBase + (*nameRef));
                entry->idx = *ordinal;
            }
            qsort(module->nameExportsTable,
                    exports->NumberOfNames,
                    sizeof(struct ExportNameEntry), _compare);
        }

        // search function name in list of exported names with binary search
        found = (const struct ExportNameEntry*) bsearch(&name,
                module->nameExportsTable,
                exports->NumberOfNames,
                sizeof(struct ExportNameEntry), _find);
        if (!found) {
            // exported symbol not found
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL;
        }

        idx = found->idx;
    }

    if (idx > exports->NumberOfFunctions) {
        // name <-> ordinal number don't match
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }

    // AddressOfFunctions contains the RVAs to the "real" functions
    return (FARPROC)(LPVOID)(codeBase + (*(DWORD *) (codeBase + exports->AddressOfFunctions + (idx*4))));
}

void MemoryFreeLibrary(HMEMORYMODULE mod)
{
    PMEMORYMODULE module = (PMEMORYMODULE)mod;

    if (module == NULL) {
        return;
    }
    if (module->initialized) {
        // notify library about detaching from process
        DllEntryProc DllEntry = (DllEntryProc)(LPVOID)(module->codeBase + module->headers->OptionalHeader.AddressOfEntryPoint);
        (*DllEntry)((HINSTANCE)module->codeBase, DLL_PROCESS_DETACH, 0);
    }

    free(module->nameExportsTable);
    if (module->modules != NULL) {
        // free previously opened libraries
        int i;
        for (i=0; i<module->numModules; i++) {
            if (module->modules[i] != NULL) {
                FreeLibrary(module->modules[i]);
            }
        }

        free(module->modules);
    }

    if (module->codeBase != NULL) {
        // release memory of library
        VirtualFree(module->codeBase, 0, MEM_RELEASE);
    }

#ifdef _WIN64
    FreePointerList(module->blockedMemory, VirtualFree);
#endif
    HeapFree(GetProcessHeap(), 0, module);
}
