#pragma once
#include <QMessageBox>
#include <QString>
#include <QWidget>

namespace nv::ui {

// F5: 삭제 확인 다이얼로그 공통 헬퍼 — ChannelListPanel(2곳), GridView(1곳) 중복 제거
inline bool confirmDelete(QWidget* parent, const QString& name)
{
    return QMessageBox::question(
               parent,
               QStringLiteral("채널 삭제"),
               QStringLiteral("'%1' 채널을 삭제하시겠습니까?").arg(name),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) == QMessageBox::Yes;
}

} // namespace nv::ui
