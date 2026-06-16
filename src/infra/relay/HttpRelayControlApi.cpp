#include "HttpRelayControlApi.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTcpSocket>
#include <QUrl>

#include <cstdio>

namespace nv::infra {

HttpRelayControlApi::HttpRelayControlApi(std::string apiBase,
                                         std::chrono::milliseconds timeout)
    : m_apiBase(std::move(apiBase)), m_timeout(timeout) {}

std::vector<nv::app::RelayPathHealth> HttpRelayControlApi::pathsHealth() {
    std::fprintf(stderr, "[relayapi] pathsHealth start %s\n", m_apiBase.c_str());
    // 기존엔 QNetworkAccessManager를 썼으나 워커 스레드 + Windows에서 프록시 탐지(WinHTTP/COM)·
    // 백엔드 스레드 기계장치 때문에 fail-fast(0xc0000409)가 간헐 발생했다(relay 채널일 때만 폴링).
    // localhost 단일 GET이므로 가벼운 QTcpSocket 동기 호출로 교체 — 이벤트루프/프록시/백엔드
    // 불필요, 어느 스레드에서도 안전.
    const int tmo = static_cast<int>(m_timeout.count());
    const QUrl base(QString::fromStdString(m_apiBase));
    const QString host = base.host().isEmpty() ? QStringLiteral("127.0.0.1") : base.host();
    const quint16 port = static_cast<quint16>(base.port(9997));

    QTcpSocket sock;
    sock.connectToHost(host, port);
    if (!sock.waitForConnected(tmo)) {
        std::fprintf(stderr, "[relayapi] connect failed %s:%u\n", host.toUtf8().constData(), port);
        return {};
    }

    const QByteArray request =
        "GET /v3/paths/list HTTP/1.0\r\n"
        "Host: " + host.toUtf8() + "\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n";
    sock.write(request);
    if (!sock.waitForBytesWritten(tmo)) { return {}; }

    // HTTP/1.0 + Connection: close → 서버가 본문 후 연결을 닫는다. 닫힐 때까지 읽는다.
    QByteArray resp;
    while (sock.state() == QAbstractSocket::ConnectedState) {
        if (!sock.waitForReadyRead(tmo)) break;   // 타임아웃 또는 원격 닫힘
        resp += sock.readAll();
    }
    resp += sock.readAll();   // 닫힘 직전 잔여 바이트
    sock.abort();

    const int hdrEnd = resp.indexOf("\r\n\r\n");
    if (hdrEnd < 0) {
        std::fprintf(stderr, "[relayapi] no HTTP header end (resp=%lld bytes)\n",
                     static_cast<long long>(resp.size()));
        return {};
    }
    const QByteArray statusLine = resp.left(resp.indexOf("\r\n"));
    if (!statusLine.contains(" 200")) {
        std::fprintf(stderr, "[relayapi] non-200: %s\n", statusLine.constData());
        return {};
    }
    const QByteArray body = resp.mid(hdrEnd + 4);

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
