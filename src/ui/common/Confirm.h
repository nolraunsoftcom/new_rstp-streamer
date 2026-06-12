#pragma once
#include <QMessageBox>
#include <QString>
#include <QWidget>

namespace nv::ui {

// F5: 삭제 확인 다이얼로그 공통 헬퍼 — ChannelListPanel(2곳), GridView(1곳) 중복 제거
// 아이콘 없는 깔끔한 모양을 위해 static question() 대신 직접 구성한다.
inline bool confirmDelete(QWidget* parent, const QString& name)
{
    QMessageBox box(parent);
    box.setIcon(QMessageBox::NoIcon);
    box.setWindowTitle(QStringLiteral("채널 삭제"));
    box.setText(QStringLiteral("'%1' 채널을 삭제하시겠습니까?").arg(name));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

// 아이콘 없는 경고 다이얼로그 — QMessageBox::warning 대체
inline void warnBox(QWidget* parent, const QString& title, const QString& text)
{
    QMessageBox box(parent);
    box.setIcon(QMessageBox::NoIcon);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

} // namespace nv::ui
