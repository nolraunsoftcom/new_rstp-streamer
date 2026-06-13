#include <catch2/catch_test_macros.hpp>
#include <QTemporaryDir>
#include <QFile>
#include <fstream>
#include <sstream>
#include "src/infra/relay/MediaMtxConfigWriter.h"

TEST_CASE("MediaMtxConfigWriter: 기록 후 동일 내용 읽힘") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/mediamtx.yml").toStdString();
    const std::string yml = "rtsp:\n  address: :8554\n";

    nv::infra::MediaMtxConfigWriter writer;
    REQUIRE(writer.write(path, yml));

    std::ifstream ifs(path);
    REQUIRE(ifs.is_open());
    std::ostringstream ss;
    ss << ifs.rdbuf();
    CHECK(ss.str() == yml);
}

TEST_CASE("MediaMtxConfigWriter: 상위 디렉토리 없으면 생성") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/sub/dir/mediamtx.yml").toStdString();
    const std::string yml = "rtsp:\n  address: :8554\n";

    nv::infra::MediaMtxConfigWriter writer;
    REQUIRE(writer.write(path, yml));

    QFile f(QString::fromStdString(path));
    CHECK(f.exists());
}

TEST_CASE("MediaMtxConfigWriter: asCallback이 write와 동일 동작") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/via_callback.yml").toStdString();
    const std::string yml = "rtsp:\n  address: :8554\n";

    nv::infra::MediaMtxConfigWriter writer;
    auto cb = writer.asCallback();
    REQUIRE(cb(path, yml));

    QFile f(QString::fromStdString(path));
    CHECK(f.exists());

    REQUIRE(f.open(QIODevice::ReadOnly));
    const std::string content = f.readAll().toStdString();
    CHECK(content == yml);
}
