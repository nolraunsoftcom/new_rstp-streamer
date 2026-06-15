#include "ChannelInfoDialog.h"
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace nv::ui {

namespace {
QString recStateText(nv::domain::RecordingState st)
{
    switch (st) {
    case nv::domain::RecordingState::Recording: return QStringLiteral("녹화 중");
    case nv::domain::RecordingState::Starting:  return QStringLiteral("녹화 시작 중");
    case nv::domain::RecordingState::Stopping:  return QStringLiteral("녹화 종료 중");
    case nv::domain::RecordingState::Idle:      return QStringLiteral("대기");
    }
    return QStringLiteral("대기");
}

// 값 라벨(선택 복사 가능, 단색)
QLabel* valueLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet(QStringLiteral("color:#222; background:transparent;"));
    l->setWordWrap(true);
    return l;
}
} // namespace

ChannelInfoDialog::ChannelInfoDialog(const nv::domain::ChannelConfig& cfg,
                                     bool streaming,
                                     nv::domain::RecordingState recState,
                                     QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("채널 정보 - %1").arg(QString::fromStdString(cfg.name)));
    setMinimumWidth(420);
    setStyleSheet(QStringLiteral(
        "QDialog { background-color:#ffffff; } "
        "QLabel { font-size:12px; }"));

    const QString playUrl = cfg.useRelay
        ? QString::fromStdString(nv::domain::relayUrlFor(cfg.id))
        : QString::fromStdString(cfg.url);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(8);

    auto addRow = [&](const QString& key, const QString& val) {
        auto* k = new QLabel(key, this);
        k->setStyleSheet(QStringLiteral("color:#666; font-size:12px; background:transparent;"));
        form->addRow(k, valueLabel(val, this));
    };

    addRow(QStringLiteral("채널명"), QString::fromStdString(cfg.name));
    addRow(QStringLiteral("연결 상태"),
           streaming ? QStringLiteral("연결됨") : QStringLiteral("끊김/대기"));
    addRow(QStringLiteral("녹화 상태"), recStateText(recState));
    addRow(QStringLiteral("연결 모드"),
           cfg.useRelay ? QStringLiteral("Relay 경유") : QStringLiteral("직결"));
    addRow(QStringLiteral("재생 URL"), playUrl);
    addRow(QStringLiteral("원본(장비) URL"), QString::fromStdString(cfg.url));
    addRow(QStringLiteral("자동 연결"),
           cfg.autoConnect ? QStringLiteral("예") : QStringLiteral("아니오"));

    outer->addLayout(form);
}

} // namespace nv::ui
