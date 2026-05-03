#pragma once
// ═══════════════════════════════════════════════════════════════════
//  PEBWalk.h — Manual PEB module/export resolution (no winternl.h)
// ═══════════════════════════════════════════════════════════════════

#include <Windows.h>
#include <cstdint>

// ── UNICODE_STRING — defined here so we don't need winternl.h ──
typedef struct _SK_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} SK_UNICODE_STRING;

// ── Full LDR_DATA_TABLE_ENTRY layout ──
typedef struct _SK_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY        InLoadOrderLinks;
    LIST_ENTRY        InMemoryOrderLinks;
    LIST_ENTRY        InInitializationOrderLinks;
    PVOID             DllBase;
    PVOID             EntryPoint;
    ULONG             SizeOfImage;
    SK_UNICODE_STRING FullDllName;
    SK_UNICODE_STRING BaseDllName;
    ULONG             Flags;
    SHORT             LoadCount;
    SHORT             TlsIndex;
    LIST_ENTRY        HashLinks;
    PVOID             SectionPointer;
    ULONG             CheckSum;
    ULONG             TimeDateStamp;
} SK_LDR_DATA_TABLE_ENTRY;

// ── PEB_LDR_DATA ──
typedef struct _SK_PEB_LDR_DATA {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} SK_PEB_LDR_DATA;

// ── Minimal PEB ──
typedef struct _SK_PEB {
    BYTE             Reserved1[2];
    BYTE             BeingDebugged;
    BYTE             Reserved2[1];
    PVOID            Reserved3[2];
    SK_PEB_LDR_DATA* Ldr;
} SK_PEB;

namespace PEB
{
    constexpr uint32_t Hash(const wchar_t* str)
    {
        uint32_t h = 5381;
        while (*str) {
            wchar_t c = (*str >= L'A' && *str <= L'Z') ? (*str + 32) : *str;
            h = ((h << 5) + h) ^ (uint32_t)c;
            str++;
        }
        return h;
    }

    constexpr uint32_t HashA(const char* str)
    {
        uint32_t h = 5381;
        while (*str) { h = ((h << 5) + h) ^ (uint32_t)*str++; }
        return h;
    }

    inline uint32_t HashRuntime(const wchar_t* str)
    {
        uint32_t h = 5381;
        while (*str) {
            wchar_t c = (*str >= L'A' && *str <= L'Z') ? (*str + 32) : *str;
            h = ((h << 5) + h) ^ (uint32_t)c;
            str++;
        }
        return h;
    }

    inline HMODULE GetModule(uint32_t nameHash)
    {
#ifdef _WIN64
        SK_PEB* peb = (SK_PEB*)__readgsqword(0x60);
#else
        SK_PEB* peb = (SK_PEB*)__readfsdword(0x30);
#endif
        LIST_ENTRY* head = &peb->Ldr->InLoadOrderModuleList;
        LIST_ENTRY* curr = head->Flink;
        while (curr != head)
        {
            SK_LDR_DATA_TABLE_ENTRY* e =
                CONTAINING_RECORD(curr, SK_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
            if (e->BaseDllName.Buffer && e->BaseDllName.Length > 0)
                if (HashRuntime(e->BaseDllName.Buffer) == nameHash)
                    return (HMODULE)e->DllBase;
            curr = curr->Flink;
        }
        return nullptr;
    }

    inline void* GetExport(HMODULE hMod, uint32_t exportHash)
    {
        if (!hMod) return nullptr;
        BYTE* base = (BYTE*)hMod;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        IMAGE_DATA_DIRECTORY* dir =
            &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir->VirtualAddress) return nullptr;
        IMAGE_EXPORT_DIRECTORY* exp =
            (IMAGE_EXPORT_DIRECTORY*)(base + dir->VirtualAddress);
        DWORD* names = (DWORD*)(base + exp->AddressOfNames);
        DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
        WORD*  ords  = (WORD*) (base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames; i++) {
            const char* name = (const char*)(base + names[i]);
            if (HashA(name) == exportHash)
                return (void*)(base + funcs[ords[i]]);
        }
        return nullptr;
    }

    inline void* Resolve(uint32_t moduleHash, uint32_t exportHash)
    {
        HMODULE h = GetModule(moduleHash);
        return h ? GetExport(h, exportHash) : nullptr;
    }
}
