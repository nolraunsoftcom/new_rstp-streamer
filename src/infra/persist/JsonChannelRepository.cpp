#include "JsonChannelRepository.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
        if (obj.contains("channels") && obj.value("channels").isArray())
            return parseArray(obj.value("channels").toArray());
    }

    return {};
}

bool JsonChannelRepository::save(const std::vector<ChannelConfig>& channels) {
    QJsonArray arr;
    for (const auto& c : channels) {
        QJsonObject o;
        o["id"] = QString::fromStdString(c.id);
        o["name"] = QString::fromStdString(c.name);
        o["url"] = QString::fromStdString(c.url);
        o["gridIndex"] = c.gridIndex;
        arr.append(o);
    }
    QJsonObject envelope;
    envelope["version"] = 1;
    envelope["channels"] = arr;
    QSaveFile f(QString::fromStdString(m_path));   // 원자적 쓰기
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(envelope).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace nv::infra
