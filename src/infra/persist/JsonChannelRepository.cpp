#include "JsonChannelRepository.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

namespace nv::infra {

using nv::domain::ChannelConfig;

JsonChannelRepository::JsonChannelRepository(std::string filePath)
    : m_path(std::move(filePath)) {}

static std::vector<ChannelConfig> parseArray(const QJsonArray& arr) {
    std::vector<ChannelConfig> out;
    for (const auto& v : arr) {
        const auto o = v.toObject();
        ChannelConfig c;
        c.id = o.value("id").toString().toStdString();
        c.name = o.value("name").toString().toStdString();
        c.url = o.value("url").toString().toStdString();
        c.gridIndex = o.value("gridIndex").toInt(-1);
        // listIndex 누락(구버전 파일) 시 gridIndex로 마이그레이션 — 기존 순서 유지.
        c.listIndex = o.value("listIndex").toInt(c.gridIndex);
        c.autoConnect = o.value("autoConnect").toBool(false);
        c.useRelay = o.value("useRelay").toBool(false);
        if (!c.id.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<ChannelConfig> JsonChannelRepository::load() {
    QFile f(QString::fromStdString(m_path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());

    // 구버전: 최상위가 배열이면 그대로 읽음 (하위호환)
    if (doc.isArray()) return parseArray(doc.array());

    // 신버전: 최상위가 객체이고 "channels" 배열이 있으면 그것을 읽음
    if (doc.isObject()) {
        const auto obj = doc.object();
        const int ver = obj.value("version").toInt(1);
        if (ver > 2) {
            std::fprintf(stderr, "[JsonChannelRepository] 미래 버전(v%d) 파일 — 로드/저장 거부(데이터 보호)\n", ver);
            m_refuseSave = true;   // 이후 save()를 무동작으로 막아 덮어쓰기 방지
            return {};
        }
        if (obj.contains("channels") && obj.value("channels").isArray())
            return parseArray(obj.value("channels").toArray());
    }

    return {};
}

bool JsonChannelRepository::save(const std::vector<ChannelConfig>& channels) {
    if (m_refuseSave) return false;   // R7: 미래 버전 파일 보호 — 덮어쓰기 거부
    QJsonArray arr;
    for (const auto& c : channels) {
        QJsonObject o;
        o["id"] = QString::fromStdString(c.id);
        o["name"] = QString::fromStdString(c.name);
        o["url"] = QString::fromStdString(c.url);
        o["gridIndex"] = c.gridIndex;
        o["listIndex"] = c.listIndex;
        o["autoConnect"] = c.autoConnect;
        o["useRelay"] = c.useRelay;
        arr.append(o);
    }
    QJsonObject envelope;
    envelope["version"] = 2;
    envelope["channels"] = arr;
    QSaveFile f(QString::fromStdString(m_path));   // 원자적 쓰기
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(envelope).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace nv::infra
