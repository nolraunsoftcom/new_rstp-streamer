#pragma once
#include <string>
#include "src/app/ports/IChannelRepository.h"

namespace nv::infra {

// 채널 목록 JSON 영속화. 쓰기는 임시파일+rename으로 원자적 (저장 중 크래시에도 파일 보존).
class JsonChannelRepository final : public nv::app::IChannelRepository {
public:
    explicit JsonChannelRepository(std::string filePath);
    std::vector<nv::domain::ChannelConfig> load() override;
    void save(const std::vector<nv::domain::ChannelConfig>& channels) override;

private:
    std::string m_path;
};

} // namespace nv::infra
