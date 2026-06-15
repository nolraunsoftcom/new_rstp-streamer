#include "HttpRelayControlApi.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <cstdio>

namespace nv::infra {

HttpRelayControlApi::HttpRelayControlApi(std::string apiBase,
                                         std::chrono::milliseconds timeout)
    : m_apiBase(std::move(apiBase)), m_timeout(timeout) {}

std::vector<nv::app::RelayPathHealth> HttpRelayControlApi::pathsHealth() {
    std::fprintf(stderr, "[relayapi] pathsHealth start %s\n", m_apiBase.c_str());
    // QNetworkAccessManager는 QCoreApplication event loop가 존재해야 정상 동작한다.
    // 워커 스레드(RelayCoordinator)에서 호출된다.
    QNetworkAccessManager mgr;
    // 로컬(127.0.0.1) 조회엔 프록시가 불필요. Windows에서 기본 프록시 탐지는 WinHTTP/COM
    // 경로를 타는데, COM 미초기화 워커 스레드에서 fail-fast(0xc0000409) 가능 → NoProxy로 차단.
    mgr.setProxy(QNetworkProxy::NoProxy);
    mgr.setTransferTimeout(static_cast<int>(m_timeout.count()));

    const QUrl url(QString::fromStdString(m_apiBase + "/v3/paths/list"));
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = mgr.get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(static_cast<int>(m_timeout.count()));

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    timer.stop();

    // 타임아웃 또는 미완료
    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        std::fprintf(stderr, "[HttpRelayControlApi] timeout: %s\n",
                     url.toString().toStdString().c_str());
        return {};
    }

    // 네트워크 오류
    if (reply->error() != QNetworkReply::NoError) {
        std::fprintf(stderr, "[HttpRelayControlApi] network error: %s\n",
                     reply->errorString().toStdString().c_str());
        reply->deleteLater();
        return {};
    }

    // HTTP 상태 확인
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus != 200) {
        std::fprintf(stderr, "[HttpRelayControlApi] HTTP %d from %s\n", httpStatus,
                     url.toString().toStdString().c_str());
        reply->deleteLater();
        return {};
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    // JSON 파싱
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (doc.isNull()) {
        std::fprintf(stderr, "[HttpRelayControlApi] JSON parse error: %s\n",
                     parseErr.errorString().toStdString().c_str());
        return {};
    }

    const QJsonObject root = doc.object();
    const QJsonArray items = root.value("items").toArray();

    std::vector<nv::app::RelayPathHealth> result;
    result.reserve(static_cast<size_t>(items.size()));

    for (const QJsonValue& val : items) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();

        nv::app::RelayPathHealth h;
        h.name      = obj.value("name").toString().toStdString();
        h.ready     = obj.value("ready").toBool(false);
        // hasSource: "source" 필드가 존재하고 null이 아님
        h.hasSource = obj.contains("source") && !obj.value("source").isNull();

        result.push_back(std::move(h));
    }

    return result;
}

} // namespace nv::infra
