#pragma once
#include <QDialog>
class QLineEdit;

namespace nv::ui {

class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelDialog(QWidget* parent = nullptr, const QString& name = {},
                           const QString& url = {});
    QString name() const;
    QString url() const;

protected:
    void accept() override;   // U2: 빈 이름·비 RTSP URL 거부

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_url = nullptr;
};

} // namespace nv::ui
