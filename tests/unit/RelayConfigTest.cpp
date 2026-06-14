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
    CHECK(yml.find("source: \"rtsp://169.254.4.1:8900/live\"") != std::string::npos);
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
TEST_CASE("주입 방어: deviceUrl의 개행+runOnReady가 설정에 주입되지 않는다") {
    // \n(0x0A)은 제어문자(<0x20) → yamlQuote가 제거 → runOnReady가 독립 YAML 키 라인으로 주입되지 않음
    const std::string malicious = "rtsp://d/live\nrunOnReady: touch /tmp/pwned";
    auto yml = RelayConfig::generate({{"ch1", malicious, true}});
    // runOnReady가 YAML 키(줄 시작 패턴 \nrunOnReady:)로 독립 존재하지 않아야 한다
    CHECK(yml.find("\nrunOnReady:") == std::string::npos);
    // source 값은 이중따옴표로 묶인 단일 스칼라 — 개행이 제거된 형태
    CHECK(yml.find("source: \"rtsp://d/liverunOnReady: touch /tmp/pwned\"") != std::string::npos);
    // validate도 통과해야 한다 (runOnReady가 독립 키로 없으므로)
    auto r = RelayConfig::validate(yml);
    CHECK(r.ok);
}
TEST_CASE("주입 방어: deviceUrl의 따옴표가 이스케이프된다") {
    const std::string withQuote = "rtsp://d/live?param=\"value\"";
    auto yml = RelayConfig::generate({{"ch1", withQuote, true}});
    // 이중따옴표는 \" 로 이스케이프되어야 한다
    CHECK(yml.find("source: \"rtsp://d/live?param=\\\"value\\\"\"") != std::string::npos);
    CHECK(RelayConfig::validate(yml).ok);
}
TEST_CASE("validate: runOnReady 포함 yml은 거부") {
    // sourceOnDemand:no + rtspTransports:[tcp] 는 충족하되 runOnReady 를 주입
    const std::string injected =
        "rtspTransports: [tcp]\n"
        "pathDefaults:\n"
        "  sourceOnDemand: no\n"
        "paths:\n"
        "  ch1:\n"
        "    runOnReady: evil_cmd\n"
        "    sourceOnDemand: no\n";
    auto r = RelayConfig::validate(injected);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("runOnReady") != std::string::npos);
}
