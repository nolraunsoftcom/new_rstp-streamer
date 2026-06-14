#include "ChannelDialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QUrl>
#include "src/ui/common/Confirm.h"

namespace nv::ui {

ChannelDialog::ChannelDialog(QWidget* parent, const QString& name, const QString& url,
                             bool autoConnect)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? QStringLiteral("채널 추가") : QStringLiteral("채널 수정"));
    setStyleSheet(QStringLiteral("QLineEdit { background-color: white; }"));
    auto* form = new QFormLayout(this);
    m_name = new QLineEdit(name, this);
    m_url = new QLineEdit(url.isEmpty() ? QStringLiteral("rtsp://") : url, this);
    m_url->setMinimumWidth(320);
    form->addRow(QStringLiteral("이름"), m_name);
    form->addRow(QStringLiteral("RTSP URL"), m_url);

    // 구분선 + 연결 옵션 섹션
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    form->addRow(line);
    form->addRow(new QLabel(QStringLiteral("연결 옵션 설정"), this));
    m_autoConnect = new QCheckBox(QStringLiteral("자동 연결"), this);
    m_autoConnect->setToolTip(QStringLiteral("앱 시작 시 이 채널에 자동으로 연결합니다."));
    m_autoConnect->setChecked(autoConnect);
    form->addRow(m_autoConnect);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
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
