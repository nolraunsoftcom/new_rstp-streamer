#pragma once
#include <QDialog>
class QCheckBox;
class QLineEdit;

namespace nv::ui {

class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelDialog(QWidget* parent = nullptr, const QString& name = {},
                           const QString& url = {}, bool autoConnect = false);
    QString name() const;
    QString url() const;
    bool autoConnect() const;

protected:
    void accept() override;   // U2: 빈 이름·비 RTSP URL 거부

private:
    QLineEdit* m_name        = nullptr;
    QLineEdit* m_url         = nullptr;
    QCheckBox* m_autoConnect = nullptr;
    QLineEdit* m_relayPath   = nullptr;  // P1 시각 패리티용 — 비활성(기능은 channels.json useRelay)
};

} // namespace nv::ui
