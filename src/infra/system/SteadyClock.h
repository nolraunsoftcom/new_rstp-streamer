#pragma once
#include "src/app/ports/IClock.h"

namespace nv::infra {

class SteadyClock final : public nv::app::IClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
};

} // namespace nv::infra
