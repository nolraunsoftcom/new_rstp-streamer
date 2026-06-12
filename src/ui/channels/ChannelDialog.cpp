#include "ChannelDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>

namespace nv::ui {

ChannelDialog::ChannelDialog(QWidget* parent, const QString& name, const QString& url)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? QStringLiteral("채널 추가") : QStringLiteral("채널 수정"));
    auto* form = new QFormLayout(this);
    m_name = new QLineEdit(name, this);
    m_url = new QLineEdit(url.isEmpty() ? QStringLiteral("rtsp://") : url, this);
    m_url->setMinimumWidth(320);
    form->addRow(QStringLiteral("이름"), m_name);
    form->addRow(QStringLiteral("RTSP URL"), m_url);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

QString ChannelDialog::name() const { return m_name->text().trimmed(); }
QString ChannelDialog::url() const { return m_url->text().trimmed(); }

void ChannelDialog::accept() {
    // U2: 빈 이름 또는 비 RTSP 스킴이면 경고 후 닫지 않음
    if (name().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("채널 이름을 입력하세요."));
        return;
    }
    const QString u = url();
    if (!u.startsWith(QStringLiteral("rtsp://")) &&
        !u.startsWith(QStringLiteral("rtsps://"))) {
        QMessageBox::warning(this, QStringLiteral("입력 오류"),
                             QStringLiteral("URL은 rtsp:// 또는 rtsps://로 시작해야 합니다."));
        return;
    }
    QDialog::accept();
}

} // namespace nv::ui
