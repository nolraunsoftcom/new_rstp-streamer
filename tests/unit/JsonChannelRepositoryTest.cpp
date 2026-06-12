#include <catch2/catch_test_macros.hpp>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
    (void)repo.save(in);

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
    { QFile f(p); (void)f.open(QIODevice::WriteOnly); f.write("{not json!!"); }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    CHECK(repo.load().empty());
}

// ── P1 로드 필터 3분기 ─────────────────────────────────────────────────────

TEST_CASE("비배열 루트 JSON → 빈 목록") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/obj.json";
    { QFile f(p); (void)f.open(QIODevice::WriteOnly); f.write("{\"key\":1}"); }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    CHECK(repo.load().empty());
}

TEST_CASE("빈 id 항목은 로드 결과에서 제외된다") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/noid.json";
    {
        QFile f(p); (void)f.open(QIODevice::WriteOnly);
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
        QFile f(p); (void)f.open(QIODevice::WriteOnly);
        f.write(R"([{"id":"ch1","name":"cam","url":"rtsp://x"}])");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    auto out = repo.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].gridIndex == -1);
}

// ── M2b: 스키마 버전 envelope ───────────────────────────────────────────────

TEST_CASE("envelope 형식 저장→로드 라운드트립") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/channels_env.json").toStdString();

    nv::infra::JsonChannelRepository repo(path);
    std::vector<ChannelConfig> in = {
        {"ch1", "카메라1", "rtsp://169.254.4.1:8900/live", 0},
        {"ch2", "무전기",  "rtsp://127.0.0.1:8554/radio",  3},
    };
    (void)repo.save(in);

    // 저장된 파일에 version 키가 있어야 한다
    {
        QFile f(QString::fromStdString(path));
        REQUIRE(f.open(QIODevice::ReadOnly));
        const auto doc = QJsonDocument::fromJson(f.readAll());
        REQUIRE(doc.isObject());
        CHECK(doc.object().value("version").toInt() == 1);
        CHECK(doc.object().contains("channels"));
    }

    // 재로드 결과가 원본과 일치
    nv::infra::JsonChannelRepository repo2(path);
    auto out = repo2.load();
    REQUIRE(out.size() == 2);
    CHECK(out[0] == in[0]);
    CHECK(out[1] == in[1]);
}

TEST_CASE("구버전 최상위 배열도 로드된다(하위호환)") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/legacy.json";
    {
        QFile f(p); (void)f.open(QIODevice::WriteOnly);
        f.write(R"([
            {"id":"ch1","name":"레거시","url":"rtsp://legacy/stream","gridIndex":2}
        ])");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    auto out = repo.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "ch1");
    CHECK(out[0].name == "레거시");
    CHECK(out[0].gridIndex == 2);
}

TEST_CASE("version 2 파일은 로드 거부(빈 목록) + 이후 save 거부") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString p = dir.path() + "/future.json";
    {
        QFile f(p); (void)f.open(QIODevice::WriteOnly);
        f.write(R"({"version":2,"channels":[
            {"id":"ch1","name":"미래","url":"rtsp://future/s","gridIndex":0}
        ]})");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    // 미래 버전 → 빈 목록 반환
    auto out = repo.load();
    CHECK(out.empty());
    // save 거부 → 파일 내용 보존 (version=2 그대로여야 함)
    std::vector<ChannelConfig> newData = {{"ch2", "새채널", "rtsp://new/s", 1}};
    CHECK_FALSE(repo.save(newData));
    // 파일은 원본 v2 내용 그대로 보존돼야 함
    {
        QFile f(p);
        REQUIRE(f.open(QIODevice::ReadOnly));
        const auto doc = QJsonDocument::fromJson(f.readAll());
        REQUIRE(doc.isObject());
        CHECK(doc.object().value("version").toInt() == 2);
    }
}

TEST_CASE("version 키 누락 객체도 channels 있으면 로드") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/noversion.json";
    {
        QFile f(p); (void)f.open(QIODevice::WriteOnly);
        f.write(R"({"channels":[
            {"id":"ch1","name":"노버전","url":"rtsp://x/s","gridIndex":0}
        ]})");
    }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    auto out = repo.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == "ch1");
}
