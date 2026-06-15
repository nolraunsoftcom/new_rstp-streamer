#include "ChannelInfoDialog.h"
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace nv::ui {

namespace {
// 레거시 ChannelInfoDialog::formatBytes 미러 (1024 기반, B/KiB/MiB/GiB).
QString formatBytes(long long bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    const double kib = static_cast<double>(bytes) / 1024.0;
    if (kib < 1024.0) return QStringLiteral("%1 KiB").arg(kib, 0, 'f', 0);
    const double mib = kib / 1024.0;
    if (mib < 1024.0) return QStringLiteral("%1 MiB").arg(mib, 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(mib / 1024.0, 0, 'f', 2);
}

QString formatBitrate(double kbps)
{
    if (kbps >= 1000.0) return QStringLiteral("%1 Mb/s").arg(kbps / 1000.0, 0, 'f', 2);
    return QStringLiteral("%1 kb/s").arg(kbps, 0, 'f', 0);
}

QLabel* bottomLabel(QWidget* parent)
{
    auto* l = new QLabel(parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet(QStringLiteral("color:#333; background:transparent;"));
    return l;
}
} // namespace

ChannelInfoDialog::ChannelInfoDialog(const nv::domain::ChannelConfig& cfg,
                                     StatsProvider provider, QWidget* parent)
    : QDialog(parent), m_cfg(cfg), m_provider(std::move(provider))
{
    setWindowTitle(QStringLiteral("채널 정보 - %1").arg(QString::fromStdString(cfg.name)));
    setMinimumSize(560, 540);
    setStyleSheet(QStringLiteral(
        "QDialog { background-color:#ffffff; color:#222; }"
        "QTreeWidget { background-color:#ffffff; color:#222; border:none; "
        "alternate-background-color:#f7f7f7; }"
        "QTreeWidget::item { padding:3px 2px; }"
        "QHeaderView::section { background-color:#f3f3f3; color:#333; border:none; padding:5px; }"
        "QLabel { color:#333; background-color:transparent; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels(QStringList{QStringLiteral("항목"), QStringLiteral("값")});
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    // 레거시와 동일 그룹/항목 구성
    auto* audio = addGroup(QStringLiteral("오디오"));
    addMetric(audio, QStringLiteral("audio.decoded"), QStringLiteral("디코드"));
    addMetric(audio, QStringLiteral("audio.played"),  QStringLiteral("재생"));
    addMetric(audio, QStringLiteral("audio.lost"),    QStringLiteral("손실"));

    auto* video = addGroup(QStringLiteral("비디오"));
    addMetric(video, QStringLiteral("video.decoded"),     QStringLiteral("디코드"));
    addMetric(video, QStringLiteral("video.displayed"),   QStringLiteral("출력"));
    addMetric(video, QStringLiteral("video.lost"),        QStringLiteral("손실"));
    addMetric(video, QStringLiteral("video.output_fps"),  QStringLiteral("출력 FPS"));
    addMetric(video, QStringLiteral("video.nominal_fps"), QStringLiteral("트랙 FPS"));
    addMetric(video, QStringLiteral("video.recent_lost"), QStringLiteral("최근 손실"));

    auto* input = addGroup(QStringLiteral("입력/읽기"));
    addMetric(input, QStringLiteral("input.read_bytes"),  QStringLiteral("미디어 데이터 크기"));
    addMetric(input, QStringLiteral("input.bitrate"),     QStringLiteral("입력 비트 전송속도"));
    addMetric(input, QStringLiteral("demux.read_bytes"),  QStringLiteral("Demux한 데이터 크기"));
    addMetric(input, QStringLiteral("demux.bitrate"),     QStringLiteral("내용물 비트 전송속도"));
    addMetric(input, QStringLiteral("demux.corrupted"),   QStringLiteral("버림 (깨졌음)"));
    addMetric(input, QStringLiteral("demux.discontinuity"), QStringLiteral("누락 (중지됨)"));
    addMetric(input, QStringLiteral("demux.recent_corrupted"), QStringLiteral("최근 버림"));
    addMetric(input, QStringLiteral("demux.recent_discontinuity"), QStringLiteral("최근 누락"));

    auto* output = addGroup(QStringLiteral("스트림 출력"));
    addMetric(output, QStringLiteral("stream.sent_packets"), QStringLiteral("전송 패킷"));
    addMetric(output, QStringLiteral("stream.sent_bytes"),   QStringLiteral("전송 바이트"));
    addMetric(output, QStringLiteral("stream.bitrate"),      QStringLiteral("전송 비트 전송속도"));

    m_tree->expandAll();
    layout->addWidget(m_tree, 1);

    const QString playUrl = m_cfg.useRelay
        ? QString::fromStdString(nv::domain::relayUrlFor(m_cfg.id))
        : QString::fromStdString(m_cfg.url);
    m_playUrl = bottomLabel(this);
    m_playUrl->setText(QStringLiteral("재생 URL: %1").arg(playUrl));
    m_sourceUrl = bottomLabel(this);
    m_sourceUrl->setText(QStringLiteral("원본 URL: %1").arg(QString::fromStdString(m_cfg.url)));
    m_relayLabel = bottomLabel(this);
    m_relayLabel->setText(m_cfg.useRelay
        ? QStringLiteral("Relay: enabled (%1)").arg(QString::fromStdString(m_cfg.id))
        : QStringLiteral("Relay: disabled"));
    layout->addWidget(m_playUrl);
    layout->addWidget(m_sourceUrl);
    layout->addWidget(m_relayLabel);

    // 1초 주기 라이브 갱신(레거시 refresh 타이머 미러). 다이얼로그 파괴 시 타이머도 함께 정리.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &ChannelInfoDialog::refresh);
    timer->start(1000);
    refresh();
}

QTreeWidgetItem* ChannelInfoDialog::addGroup(const QString& title)
{
    auto* group = new QTreeWidgetItem(m_tree, QStringList{title, QString()});
    group->setFirstColumnSpanned(true);
    return group;
}

void ChannelInfoDialog::addMetric(QTreeWidgetItem* group, const QString& key, const QString& label)
{
    auto* item = new QTreeWidgetItem(group, QStringList{label, QStringLiteral("--")});
    m_items.insert(key, item);
}

void ChannelInfoDialog::refresh()
{
    const Stats s = m_provider ? m_provider() : Stats{};
    auto set = [this](const QString& key, const QString& value) {
        if (auto* it = m_items.value(key, nullptr); it && it->text(1) != value)
            it->setText(1, value);
    };

    // 오디오 — 비디오 전용 뷰어라 해당 없음(레거시도 0)
    set(QStringLiteral("audio.decoded"), QStringLiteral("0 블록"));
    set(QStringLiteral("audio.played"),  QStringLiteral("0 버퍼"));
    set(QStringLiteral("audio.lost"),    QStringLiteral("0 버퍼"));

    // 비디오
    set(QStringLiteral("video.decoded"),     QStringLiteral("%1 블록").arg(s.decodedFrames));
    set(QStringLiteral("video.displayed"),   QStringLiteral("%1 프레임").arg(s.displayedFrames));
    set(QStringLiteral("video.lost"),        QStringLiteral("%1 프레임").arg(s.droppedFrames));
    set(QStringLiteral("video.output_fps"),  QStringLiteral("%1 fps").arg(s.outputFps, 0, 'f', 1));
    set(QStringLiteral("video.nominal_fps"), QStringLiteral("0.0 fps"));   // 트랙 fps 미보고
    set(QStringLiteral("video.recent_lost"), QStringLiteral("0 프레임")); // delta 미추적

    // 입력/읽기 — 우리 파이프라인은 demux 계층 수신을 측정(입력 계층은 0, 레거시도 0)
    set(QStringLiteral("input.read_bytes"),  QStringLiteral("0 B"));
    set(QStringLiteral("input.bitrate"),     QStringLiteral("0 kb/s"));
    set(QStringLiteral("demux.read_bytes"),  formatBytes(s.readBytesTotal));
    set(QStringLiteral("demux.bitrate"),     formatBitrate(s.bitrateKbps));
    set(QStringLiteral("demux.corrupted"),   QStringLiteral("0"));
    set(QStringLiteral("demux.discontinuity"), QStringLiteral("0"));
    set(QStringLiteral("demux.recent_corrupted"), QStringLiteral("0"));
    set(QStringLiteral("demux.recent_discontinuity"), QStringLiteral("0"));

    // 스트림 출력 — 뷰어는 재스트리밍하지 않음(레거시도 0)
    set(QStringLiteral("stream.sent_packets"), QStringLiteral("0"));
    set(QStringLiteral("stream.sent_bytes"),   QStringLiteral("0 B"));
    set(QStringLiteral("stream.bitrate"),      QStringLiteral("0 kb/s"));
}

} // namespace nv::ui
