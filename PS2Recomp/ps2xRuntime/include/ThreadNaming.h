#pragma once

#include <string_view>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C"
{
    typedef void *HANDLE;
    typedef void *HMODULE;
    typedef const wchar_t *PCWSTR;
    typedef long HRESULT;

    __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const wchar_t *lpModuleName);
    __declspec(dllimport) void *WINAPI GetProcAddress(HMODULE hModule, const char *lpProcName);
    __declspec(dllimport) HANDLE WINAPI GetCurrentThread(void);
}
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#elif defined(__SWITCH__)
// GOW-Port: ps2_switch_runtime.cpp. libnx starts every thread on the process's default core and Horizon does not
// time-slice threads of one priority, so the runtime's busy threads are spread over the three application cores.
// priorityBoost: steps above the default thread priority (Horizon preempts lower-priority threads on the same core).
extern "C" void ps2xSwitchPinCurrentThread(int core, int priorityBoost);
#endif

namespace ThreadNaming
{
    inline void SetCurrentThreadName(std::string_view name)
    {
#if defined(_WIN32) 
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel32, "SetThreadDescription"));

        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }

#elif defined(__APPLE__)
        pthread_setname_np(name.data());
        // GOW-Port: the runtime's named threads (EE, GS worker / stage / GL) carry the game's frame: the highest
        // quality of service keeps them on the performance cores (a lower class may put them on the efficiency cores)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.data());
#elif defined(__SWITCH__)
        // EE + IOP and the GS frontend on core 1, VIF1/VU1 on core 2; the presentation thread and the GL backend on
        // core 0. The EE spends most of a frame spinning in vid::WaitForDMAComplete (profile 2026-10-16: ~85 % of its
        // samples), so the GS frontend shares its core one priority step above it: it runs whenever it has work and
        // the spin only gets what is left. On core 0 it starved behind the GL backend and the presentation.
        if (name == "GameThread")
            ps2xSwitchPinCurrentThread(1, 0);
        else if (name == "GSStage")
            ps2xSwitchPinCurrentThread(1, 1);
        else if (name == "GSWorker")
            ps2xSwitchPinCurrentThread(2, 0);
        else
            ps2xSwitchPinCurrentThread(0, 0);
#endif
    }
}
