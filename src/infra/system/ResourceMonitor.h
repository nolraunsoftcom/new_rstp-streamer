#pragma once

#include <QtGlobal>

namespace nv::infra {

struct ResourceSnapshot {
    bool systemCpuValid = false;
    bool systemMemoryValid = false;

    double systemCpuPercent = 0.0;
    quint64 systemMemoryUsedBytes = 0;
    quint64 systemMemoryTotalBytes = 0;
};

// 시스템 CPU/메모리 샘플러. 레거시 viewer/src/ResourceMonitor.cpp 이식 (네임스페이스 nv::infra).
class ResourceMonitor {
public:
    ResourceSnapshot sample();

private:
    bool m_hasSystemCpuSample = false;
    quint64 m_lastSystemIdleTicks = 0;
    quint64 m_lastSystemTotalTicks = 0;

    static bool readSystemCpuTicks(quint64* idleTicks, quint64* totalTicks);
    static bool readSystemMemory(quint64* usedBytes, quint64* totalBytes);
};

} // namespace nv::infra
