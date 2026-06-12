#pragma once
#include <QWidget>

class QPlainTextEdit;

namespace nv::ui {

// 우측 패널 로그 탭 위젯. 최대 2000줄 롤링 유지. 레거시 viewer 로그뷰 대응.
class LogPanel : public QWidget {
    Q_OBJECT
public:
    explicit LogPanel(QWidget* parent = nullptr);

public slots:
    void appendLine(const QString& text);

private:
    static constexpr int kMaxLines = 2000;
    QPlainTextEdit* m_edit = nullptr;
};

} // namespace nv::ui
