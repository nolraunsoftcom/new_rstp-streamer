#pragma once
#include <string>
#include "src/app/ports/IChannelRepository.h"

namespace nv::infra {

// 채널 목록 JSON 영속화. 쓰기는 임시파일+rename으로 원자적 (저장 중 크래시에도 파일 보존).
class JsonChannelRepository final : public nv::app::IChannelRepository {
public:
    explicit JsonChannelRepository(std::string filePath);
    std::vector<nv::domain::ChannelConfig> load() override;
    bool save(const std::vector<nv::domain::ChannelConfig>& channels) override;

private:
    std::string m_path;
    bool m_refuseSave = false;   // R7: 미래 버전 파일 로드 시 set → save 무동작으로 데이터 보호
};

} // namespace nv::infra
