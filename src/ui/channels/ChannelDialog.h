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

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_url = nullptr;
};

} // namespace nv::ui
