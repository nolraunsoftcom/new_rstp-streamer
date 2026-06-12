#pragma once
#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace nv::infra {

// 현재 프로세스 RSS(MB). 소크 모니터링용 (설계 M1 수용 기준: 메모리 그래프 평탄).
inline double processRssMb() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0.0;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0.0;
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

} // namespace nv::infra
