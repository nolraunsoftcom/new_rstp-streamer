#include "ChannelDialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>

namespace nv::ui {

ChannelDialog::ChannelDialog(QWidget* parent, const QString& name, const QString& url,
                             bool autoConnect)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? QStringLiteral("채널 추가") : QStringLiteral("채널 수정"));
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
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("채널 이름을 입력하세요."));
        return;
    }
    // F6: RFC 스킴 대소문자 무관 — Qt::CaseInsensitive로 RTSP://, Rtsp:// 등도 허용
    const QString u = url();
    if (!u.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive) &&
        !u.startsWith(QStringLiteral("rtsps://"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("URL은 rtsp:// 또는 rtsps://로 시작해야 합니다."));
        return;
    }
    QDialog::accept();
}

} // namespace nv::ui
