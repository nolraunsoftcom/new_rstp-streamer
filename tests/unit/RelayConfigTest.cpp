#include <catch2/catch_test_macros.hpp>
#include "src/domain/relay/RelayConfig.h"
using namespace nv::domain;

TEST_CASE("RelayConfig: relay 채널만 path 생성, sourceOnDemand:no/tcp 하드코딩") {
    std::vector<RelayPath> ch{
        {"ch1","rtsp://169.254.4.1:8900/live", true},
        {"ch2","rtsp://169.254.4.1:8901/live", false},  // 직결 — 제외
    };
    const std::string yml = RelayConfig::generate(ch);
    CHECK(yml.find("rtspAddress: 127.0.0.1:8554") != std::string::npos);
    CHECK(yml.find("apiAddress: 127.0.0.1:9997") != std::string::npos);
    CHECK(yml.find("rtspTransports: [tcp]") != std::string::npos);
    CHECK(yml.find("ch1:") != std::string::npos);
    CHECK(yml.find("source: rtsp://169.254.4.1:8900/live") != std::string::npos);
    CHECK(yml.find("sourceOnDemand: no") != std::string::npos);
    CHECK(yml.find("ch2:") == std::string::npos);
    CHECK(RelayConfig::validate(yml).ok);
}
TEST_CASE("RelayConfig: api/rtsp 활성화 + 불필요 프로토콜 비활성") {
    auto yml = RelayConfig::generate({{"ch1","rtsp://d/live",true}});
    CHECK(yml.find("api: yes") != std::string::npos);
}
TEST_CASE("RelayConfig: sourceOnDemand:yes 주입 시 validate 실패(보호막 무효)") {
    auto r = RelayConfig::validate("paths:\n  ch1:\n    source: rtsp://d\n    sourceOnDemand: yes\n");
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("sourceOnDemand") != std::string::npos);
}
TEST_CASE("RelayConfig: tcp 누락 시 validate 실패") {
    auto r = RelayConfig::validate("paths:\n  ch1:\n    sourceOnDemand: no\n");
    CHECK_FALSE(r.ok);
}
TEST_CASE("RelayConfig: relay 채널 없으면 paths는 비어도 전역설정은 유효") {
    auto yml = RelayConfig::generate({{"ch2","rtsp://d/live",false}});
    CHECK(yml.find("ch2:") == std::string::npos);
    CHECK(RelayConfig::validate(yml).ok);   // 전역 sourceOnDemand 기본 + tcp는 충족
}
