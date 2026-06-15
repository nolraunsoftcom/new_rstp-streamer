#pragma once
#include <QDialog>
class QCheckBox;
class QLabel;
class QLineEdit;

namespace nv::ui {

class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelDialog(QWidget* parent = nullptr, const QString& name = {},
                           const QString& url = {}, bool autoConnect = false,
                           bool useRelay = false);
    QString name() const;
    QString url() const;
    bool autoConnect() const;
    bool useRelay() const;   // MediaMTX relay 경유 여부

protected:
    void accept() override;   // U2: 빈 이름·비 RTSP URL 거부

private:
    QLineEdit* m_name        = nullptr;
    QLineEdit* m_url         = nullptr;
    QCheckBox* m_autoConnect = nullptr;
    QCheckBox* m_useRelay    = nullptr;  // "MediaMTX relay 사용"
    QLabel*    m_relayPath   = nullptr;  // relay 경유 경로 표시(라벨, path=채널ID 자동)
};

} // namespace nv::ui
