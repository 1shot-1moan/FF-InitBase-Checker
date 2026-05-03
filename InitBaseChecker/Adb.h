#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <mutex>

class Adb
{
private:
    std::string _path;
    uint32_t    _cachedRwAddr = 0;
    std::vector<uint32_t> _cachedSegments;

    std::string RunCapture(const std::string& args, DWORD timeoutMs = 10000)
    {
        std::string cmd = "\"" + _path + "\" " + args;
        std::vector<char> cl(cmd.begin(), cmd.end());
        cl.push_back('\0');

        HANDLE hRead = nullptr, hWrite = nullptr;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.hStdOutput = hWrite;
        si.hStdError  = hWrite;
        si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};

        if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(hRead); CloseHandle(hWrite);
            return "";
        }
        CloseHandle(hWrite);

        std::string result;
        char buf[4096]; DWORD n;
        DWORD start = GetTickCount();
        while (GetTickCount() - start < timeoutMs)
        {
            DWORD avail = 0;
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
            {
                if (ReadFile(hRead, buf, sizeof(buf)-1, &n, nullptr) && n > 0)
                { buf[n] = '\0'; result += buf; }
            }
            else {
                DWORD exitCode;
                if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) break;
                Sleep(10);
            }
        }

        TerminateProcess(pi.hProcess, 0);
        CloseHandle(hRead);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return result;
    }

    uint32_t ParseMapsAddress(const std::string& output)
    {
        if (output.empty()) return 0;
        uint32_t textAddr = 0;
        _cachedRwAddr = 0;
        _cachedSegments.clear();
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t dash = line.find('-');
            if (dash == std::string::npos) continue;
            try {
                uint32_t addr = (uint32_t)std::stoul(line.substr(0, dash), nullptr, 16);
                if (addr <= 0x10000) continue;
                _cachedSegments.push_back(addr);
                if (line.find("rw") != std::string::npos && !_cachedRwAddr) _cachedRwAddr = addr;
                if (line.find("r-x") != std::string::npos && !textAddr) textAddr = addr;
            } catch (...) { continue; }
        }
        return textAddr;
    }

public:
    explicit Adb(const std::string& path) : _path(path) {}

    bool Start()
    {
        RunCapture("kill-server");
        Sleep(300);
        // Connect to BlueStacks ADB TCP ports (try all common ones)
        RunCapture("connect 127.0.0.1:5555", 3000);
        RunCapture("connect 127.0.0.1:5556", 3000);
        RunCapture("connect 127.0.0.1:5554", 3000);
        Sleep(300);
        RunCapture("devices");
        Sleep(300);
        // Initialize BlueStacks root — required so su -c works in FindModule
        RunCapture("shell \"getprop ro.secure ; /boot/android/android/system/xbin/bstk/su\"", 4000);
        Sleep(500);
        return true;
    }

    void Kill()
    {
        RunCapture("kill-server");
    }

    std::string RunAdbCommandWithOutput(const std::string& command)
    {
        return RunCapture(command);
    }

    uint32_t FindModule(const std::string& process, const std::string& module)
    {
        // BlueStacks 5 runs Android as root — try direct shell first (faster, no su hang)
        std::string cmd = "shell \"cat /proc/$(pidof " + process + ")/maps | grep " + module + "\"";
        std::string output = RunCapture(cmd);
        uint32_t addr = ParseMapsAddress(output);
        if (addr > 0x10000) return addr;

        // Fallback: su -c (older BlueStacks / Nougat builds)
        cmd = "shell \"su -c 'cat /proc/$(pidof " + process + ")/maps | grep " + module + "'\"";
        output = RunCapture(cmd);
        addr = ParseMapsAddress(output);
        if (addr > 0x10000) return addr;

        // Fallback: bstk su path
        cmd = "shell \"/boot/android/android/system/xbin/bstk/su -c 'cat /proc/$(pidof " + process + ")/maps | grep " + module + "'\"";
        output = RunCapture(cmd);
        return ParseMapsAddress(output);
    }

    std::vector<uint32_t> GetCachedSegments() const { return _cachedSegments; }
    uint32_t FindRwModule(const std::string&, const std::string&) { return _cachedRwAddr; }
};
