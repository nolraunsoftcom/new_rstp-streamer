#include <catch2/catch_test_macros.hpp>
#include <QTemporaryDir>
#include "src/infra/persist/JsonChannelRepository.h"

using nv::domain::ChannelConfig;

TEST_CASE("저장→로드 라운드트립") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/channels.json").toStdString();

    nv::infra::JsonChannelRepository repo(path);
    std::vector<ChannelConfig> in = {
        {"ch1", "카메라1", "rtsp://169.254.4.1:8900/live", 0},
        {"ch2", "무전기", "rtsp://127.0.0.1:8554/radio", 3},
    };
    repo.save(in);

    nv::infra::JsonChannelRepository repo2(path);   // 새 인스턴스로 로드
    auto out = repo2.load();
    REQUIRE(out.size() == 2);
    CHECK(out[0] == in[0]);
    CHECK(out[1] == in[1]);
}

TEST_CASE("파일 없음 → 빈 목록 (첫 실행)") {
    QTemporaryDir dir;
    nv::infra::JsonChannelRepository repo((dir.path() + "/none.json").toStdString());
    CHECK(repo.load().empty());
}

TEST_CASE("손상된 JSON → 빈 목록 + 크래시 없음") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/bad.json";
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("{not json!!"); }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    CHECK(repo.load().empty());
}

// ── P1 로드 필터 3분기 ─────────────────────────────────────────────────────

TEST_CASE("비배열 루트 JSON → 빈 목록") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/obj.json";
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("{\"key\":1}"); }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    CHECK(repo.load().empty());
}

TEST_CASE("빈 id 항목은 로드 결과에서 제외된다") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/noid.json";
    {
        QFile f(p); f.open(QIODevice::WriteOnly);
        f.write(R"([
            {"id":"ch1","name":"good","url":"rtsp://a","gridIndex":0},
            {"id":"","name":"bad","url":"rtsp://b","gridIndex":1}
        ])");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    auto out = repo.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "ch1");
}

TEST_CASE("gridIndex 누락 항목은 -1로 로드된다") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/nogrid.json";
    {
        QFile f(p); f.open(QIODevice::WriteOnly);
        f.write(R"([{"id":"ch1","name":"cam","url":"rtsp://x"}])");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    auto out = repo.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].gridIndex == -1);
}
