// InternalMemory.h — Matches NEW teacher GTCInternalMemory.cs
// FIX: Try ALL vCPUs (0-3) when Cast fails on core 0
// The game process may be running on any vCPU core
// SECURITY: GTCBst.dll exports resolved via PEBWalk (no GetProcAddress calls)
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <cstdio>
#include "PEBWalk.h"
#include "XorStr.h"

// Function pointer types — match GTCBst.dll exports exactly
typedef void*(__stdcall* fn_CPU)(void* pVM, int cpuId);
typedef int(__stdcall* fn_InternalRead)(void* pVM, uintptr_t address, void* buffer, size_t size);
typedef int(__stdcall* fn_Cast)(void* pVCpu, uintptr_t address, uintptr_t* physAddress);
typedef int(__stdcall* fn_InternalWrite)(void* pVM, uintptr_t address, void* buffer, size_t size);

// Debug logging disabled for safety

namespace Mem
{
    inline void* vm = nullptr;
    inline HMODULE dll = nullptr;
    inline fn_CPU pCPU = nullptr;
    inline fn_InternalRead pRead = nullptr;
    inline fn_Cast pCast = nullptr;
    inline fn_InternalWrite pWrite = nullptr;

    // Which vCPU core works for the game process
    inline int workingCore = -1;

    // Cache
    inline std::unordered_map<uint64_t, uint64_t> cache;

    // Debug counters
    inline int translateCalls = 0;
    inline int translateFails = 0;

    inline bool Init(void* pVM)
    {
        // SECURITY: Use PEBWalk instead of GetProcAddress.
        // Anti-cheats hook GetProcAddress and log every call — PEBWalk bypasses hooks.
        // GTCBst.dll is already loaded by AotBst.dll before we run, so it's in PEB LDR.

        // Hash of L"GTCBst.dll" — no plaintext module name in binary
        constexpr uint32_t hGTC     = PEB::Hash(L"scriptkittens.dll");
        constexpr uint32_t hCPU     = PEB::HashA("CPU");
        constexpr uint32_t hRead    = PEB::HashA("InternalRead");
        constexpr uint32_t hCast    = PEB::HashA("Cast");
        constexpr uint32_t hWrite   = PEB::HashA("InternalWrite");

        dll = PEB::GetModule(hGTC);
        if (!dll)
        {
            // Fallback: if not in PEB yet, force load it (first run)
            // Use XorStr so "ScriptKittens.dll" isn't plaintext in binary
            dll = LoadLibraryA(XS("scriptkittens.dll"));
        }
        if (!dll) return false;

        pCPU   = (fn_CPU)          PEB::GetExport(dll, hCPU);
        pRead  = (fn_InternalRead) PEB::GetExport(dll, hRead);
        pCast  = (fn_Cast)         PEB::GetExport(dll, hCast);
        pWrite = (fn_InternalWrite)PEB::GetExport(dll, hWrite);

        if (!pCPU || !pRead || !pCast || !pWrite) return false;

        vm = pVM;
        workingCore = -1;
        cache.clear();
        translateCalls = 0;
        translateFails = 0;

        // Test all vCPU cores
        for (int core = 0; core < 8; core++)
        {
            void* cpu = pCPU(vm, core);
            if (!cpu) break;
        }

        return true;
    }

    // Try to translate using a specific vCPU core
    inline bool TryCast(int core, uintptr_t virt, uintptr_t& physOut)
    {
        void* cpu = pCPU(vm, core);
        if (!cpu) return false;
        physOut = 0;
        int status = pCast(cpu, virt, &physOut);
        return (status == 0 && physOut != 0);
    }

    // Find which vCPU core can translate the given address
    inline int FindWorkingCore(uintptr_t testAddr)
    {
        for (int core = 0; core < 8; core++)
        {
            void* cpu = pCPU(vm, core);
            if (!cpu) break;
            uintptr_t phys = 0;
            int status = pCast(cpu, testAddr, &phys);
            if (status == 0 && phys != 0)
            {
                return core;
            }
        }
        return -1;
    }

    // Translate — tries working core first, then scans all cores if needed
    inline bool Translate(uint64_t virt, uint64_t& phys)
    {
        phys = 0;
        translateCalls++;

        // Check cache
        auto it = cache.find(virt);
        if (it != cache.end()) { phys = it->second; return true; }

        if (!vm || !pCast || !pCPU) {
            translateFails++;
            return false;
        }

        uintptr_t physOut = 0;

        // Try the known working core first
        if (workingCore >= 0)
        {
            if (TryCast(workingCore, (uintptr_t)virt, physOut))
            {
                phys = (uint64_t)physOut;
                cache[virt] = phys;
                return true;
            }
        }

        // Working core failed — scan ALL cores
        for (int core = 0; core < 8; core++)
        {
            if (core == workingCore) continue; // already tried
            void* cpu = pCPU(vm, core);
            if (!cpu) break;

            physOut = 0;
            int status = pCast(cpu, (uintptr_t)virt, &physOut);
            if (status == 0 && physOut != 0)
            {
                if (workingCore != core) {
                    workingCore = core;
                }
                phys = (uint64_t)physOut;
                cache[virt] = phys;
                return true;
            }
        }

        translateFails++;
        return false;
    }

    template<typename T>
    inline bool Read(uint64_t addr, T& out)
    {
        out = {};
        uint64_t phys;
        if (!Translate(addr, phys)) return false;
        int status = pRead(vm, (uintptr_t)phys, &out, sizeof(T));
        if (status != 0) {
            // Physical read failed — stale cache or vCPU core switched
            // Reset working core + clear this entry, then retry on all cores
            cache.erase(addr);
            workingCore = -1;
            if (!Translate(addr, phys)) return false;
            status = pRead(vm, (uintptr_t)phys, &out, sizeof(T));
            if (status != 0) return false;
        }
        return true;
    }

    template<typename T>
    inline bool Write(uint64_t addr, const T& val)
    {
        uint64_t phys;
        if (!Translate(addr, phys)) return false;
        return pWrite(vm, (uintptr_t)phys, (void*)&val, sizeof(T)) == 0;
    }

    // Thread-safe read — does NOT touch shared cache or workingCore.
    // Safe to call from Fly thread (or any non-Data thread).
    template<typename T>
    inline bool ReadNoCache(uint64_t addr, T& out)
    {
        out = {};
        if (!vm || !pCast || !pCPU || !pRead) return false;
        int tryCore = workingCore;
        if (tryCore >= 0)
        {
            void* cpu = pCPU(vm, tryCore);
            if (cpu)
            {
                uintptr_t phys = 0;
                if (pCast(cpu, (uintptr_t)addr, &phys) == 0 && phys != 0)
                    return pRead(vm, phys, &out, sizeof(T)) == 0;
            }
        }
        for (int core = 0; core < 8; core++)
        {
            if (core == tryCore) continue;
            void* cpu = pCPU(vm, core);
            if (!cpu) break;
            uintptr_t phys = 0;
            if (pCast(cpu, (uintptr_t)addr, &phys) == 0 && phys != 0)
                return pRead(vm, phys, &out, sizeof(T)) == 0;
        }
        return false;
    }

    // Thread-safe write — does NOT touch shared cache or workingCore.
    // Safe to call from Fly thread (or any non-Data thread).
    template<typename T>
    inline bool WriteNoCache(uint64_t addr, const T& val)
    {
        if (!vm || !pCast || !pCPU || !pWrite) return false;
        int tryCore = workingCore;
        if (tryCore >= 0)
        {
            void* cpu = pCPU(vm, tryCore);
            if (cpu)
            {
                uintptr_t phys = 0;
                if (pCast(cpu, (uintptr_t)addr, &phys) == 0 && phys != 0)
                    return pWrite(vm, phys, (void*)&val, sizeof(T)) == 0;
            }
        }
        for (int core = 0; core < 8; core++)
        {
            if (core == tryCore) continue;
            void* cpu = pCPU(vm, core);
            if (!cpu) break;
            uintptr_t phys = 0;
            if (pCast(cpu, (uintptr_t)addr, &phys) == 0 && phys != 0)
                return pWrite(vm, phys, (void*)&val, sizeof(T)) == 0;
        }
        return false;
    }

    inline void Flush()
    {
        cache.clear();
        workingCore = -1; // re-scan cores on next translate
    }

    inline void LogStats() {}
}
