#include "LogPanel.h"
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace nv::ui {

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_edit = new QPlainTextEdit(this);
    m_edit->setReadOnly(true);
    m_edit->setMaximumBlockCount(kMaxLines);
    m_edit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #ffffff; color: #222; border: none; "
        "font-family: monospace; font-size: 11px; padding: 4px; }"));
    layout->addWidget(m_edit);
}

void LogPanel::appendLine(const QString& text) {
    m_edit->appendPlainText(text);
}

} // namespace nv::ui
