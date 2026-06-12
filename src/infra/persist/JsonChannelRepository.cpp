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

std::vector<ChannelConfig> JsonChannelRepository::load() {
    QFile f(QString::fromStdString(m_path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return {};

    std::vector<ChannelConfig> out;
    for (const auto& v : doc.array()) {
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

void JsonChannelRepository::save(const std::vector<ChannelConfig>& channels) {
    QJsonArray arr;
    for (const auto& c : channels) {
        QJsonObject o;
        o["id"] = QString::fromStdString(c.id);
        o["name"] = QString::fromStdString(c.name);
        o["url"] = QString::fromStdString(c.url);
        o["gridIndex"] = c.gridIndex;
        arr.append(o);
    }
    QSaveFile f(QString::fromStdString(m_path));   // 원자적 쓰기
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.commit();
}

} // namespace nv::infra
