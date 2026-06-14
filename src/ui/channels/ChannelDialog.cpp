#include "ChannelDialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include "src/ui/common/Confirm.h"

// P1: 레거시 ConnectionDialog.cpp DIALOG_STYLE 미러
static const QString DIALOG_STYLE = QStringLiteral(R"(
QDialog {
    background-color: #ffffff;
}
QGroupBox {
    color: #333;
    border: 1px solid #d0d0d0;
    border-radius: 4px;
    margin-top: 8px;
    padding-top: 8px;
    font-size: 11px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 4px;
    left: 8px;
}
QLabel {
    color: #222;
    font-size: 12px;
}
QLineEdit {
    background-color: #ffffff;
    border: 1px solid #bdbdbd;
    color: #222;
    padding: 4px 6px;
    border-radius: 3px;
    font-size: 12px;
}
QLineEdit:focus {
    border-color: #0078d4;
}
QLineEdit:disabled {
    background-color: #f3f3f3;
    color: #999;
    border-color: #d0d0d0;
}
QCheckBox {
    color: #222;
    font-size: 12px;
}
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    background-color: #ffffff;
    border: 1px solid #bdbdbd;
    border-radius: 2px;
}
QCheckBox::indicator:hover {
    border-color: #0078d4;
}
QCheckBox::indicator:checked {
    background-color: #0078d4;
    border-color: #0078d4;
}
QCheckBox::indicator:disabled {
    background-color: #f3f3f3;
    border-color: #d0d0d0;
}
QPushButton {
    color: #222;
    background-color: #f7f7f7;
    border: 1px solid #bdbdbd;
    padding: 5px 16px;
    font-size: 12px;
    border-radius: 3px;
}
QPushButton:hover {
    background-color: #e8f2ff;
    border-color: #0078d4;
}
QPushButton:default {
    border-color: #0078d4;
}
)");

namespace nv::ui {

ChannelDialog::ChannelDialog(QWidget* parent, const QString& name, const QString& url,
                             bool autoConnect)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? QStringLiteral("채널 추가") : QStringLiteral("채널 수정"));
    setModal(true);
    setMinimumWidth(520);   // P1: 레거시 ConnectionDialog::setMinimumWidth(520)
    setStyleSheet(DIALOG_STYLE);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // ── 채널 정보 그룹 (레거시 "채널 정보" QGroupBox) ──────────────────────
    auto* channelGroup = new QGroupBox(QStringLiteral("채널 정보"), this);
    auto* channelForm  = new QFormLayout(channelGroup);
    channelForm->setContentsMargins(10, 16, 10, 10);
    channelForm->setSpacing(8);
    channelForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_name = new QLineEdit(name, channelGroup);
    m_name->setMinimumWidth(300);   // P1: 레거시 setMinimumWidth(300)
    channelForm->addRow(QStringLiteral("채널 이름:"), m_name);

    m_url = new QLineEdit(url.isEmpty() ? QString() : url, channelGroup);
    m_url->setMinimumWidth(300);    // P1: 레거시 setMinimumWidth(300)
    m_url->setPlaceholderText(QStringLiteral("rtsp://<카메라 IP>:8900/live"));  // P1: 레거시 placeholder
    channelForm->addRow(QStringLiteral("원본 RTSP URL:"), m_url);

    mainLayout->addWidget(channelGroup);

    // ── 옵션 그룹 (레거시 "옵션" QGroupBox) ────────────────────────────────
    auto* optionsGroup  = new QGroupBox(QStringLiteral("옵션"), this);
    auto* optionsLayout = new QVBoxLayout(optionsGroup);
    optionsLayout->setContentsMargins(10, 16, 10, 10);
    optionsLayout->setSpacing(8);

    // P1: relay 필드 — 시각 패리티용. 기능은 channels.json useRelay로 결정되므로 비활성.
    auto* relayPathLayout = new QHBoxLayout();
    relayPathLayout->setContentsMargins(0, 0, 0, 0);
    relayPathLayout->setSpacing(8);

    auto* relayPathLabel = new QLabel(QStringLiteral("Relay path:"), optionsGroup);
    relayPathLabel->setMinimumWidth(82);    // P1: 레거시 setMinimumWidth(82)
    relayPathLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    relayPathLabel->setEnabled(false);
    relayPathLayout->addWidget(relayPathLabel);

    m_relayPath = new QLineEdit(optionsGroup);
    m_relayPath->setPlaceholderText(QStringLiteral("voxl1"));  // P1: 레거시 placeholder
    m_relayPath->setEnabled(false);  // 기능 불변: useRelay는 channels.json으로 적용
    m_relayPath->setToolTip(QStringLiteral("relay는 채널 설정(useRelay)으로 적용됩니다"));
    relayPathLayout->addWidget(m_relayPath, 1);
    optionsLayout->addLayout(relayPathLayout);

    m_autoConnect = new QCheckBox(QStringLiteral("자동 연결"), optionsGroup);
    m_autoConnect->setToolTip(QStringLiteral("앱 시작 시 이 채널에 자동으로 연결합니다."));
    m_autoConnect->setChecked(autoConnect);
    optionsLayout->addWidget(m_autoConnect);

    mainLayout->addWidget(optionsGroup);

    // ── 버튼 (레거시 "확인"/"취소") ─────────────────────────────────────────
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->button(QDialogButtonBox::Ok)->setText(QStringLiteral("확인"));      // P1: 레거시
    buttonBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("취소")); // P1: 레거시
    connect(buttonBox, &QDialogButtonBox::accepted, this, &ChannelDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

QString ChannelDialog::name() const { return m_name->text().trimmed(); }
QString ChannelDialog::url() const { return m_url->text().trimmed(); }
bool ChannelDialog::autoConnect() const { return m_autoConnect->isChecked(); }

void ChannelDialog::accept() {
    // U2: 빈 이름 또는 비 RTSP 스킴이면 경고 후 닫지 않음
    if (name().isEmpty()) {
        warnBox(this, QStringLiteral("입력 오류"),
                QStringLiteral("채널 이름을 입력하세요."));
        return;
    }
    // F6: RFC 스킴 대소문자 무관 — Qt::CaseInsensitive로 RTSP://, Rtsp:// 등도 허용
    const QString u = url();
    if (!u.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive) &&
        !u.startsWith(QStringLiteral("rtsps://"), Qt::CaseInsensitive)) {
        warnBox(this, QStringLiteral("입력 오류"),
                QStringLiteral("URL은 rtsp:// 또는 rtsps://로 시작해야 합니다."));
        return;
    }
    // 레거시 수준 검증 + 주입 입구 차단: StrictMode 파싱, 유효성, 호스트 존재, 제어문자 거부.
    const QUrl parsed(u, QUrl::StrictMode);
    bool hasControl = false;
    for (const QChar& ch : u) { if (ch.unicode() < 0x20) { hasControl = true; break; } }
    if (!parsed.isValid() || parsed.host().isEmpty() || hasControl) {
        warnBox(this, QStringLiteral("입력 오류"),
                QStringLiteral("유효한 RTSP URL이 아닙니다 (호스트 누락/제어문자/형식 오류)."));
        return;
    }
    QDialog::accept();
}

} // namespace nv::ui
