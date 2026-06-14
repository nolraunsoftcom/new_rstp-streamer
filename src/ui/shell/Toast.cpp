#include "Toast.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace nv::ui {

// static 멤버 정의
QPointer<Toast> Toast::s_current;

Toast::Toast(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("toast"));
    setAttribute(Qt::WA_StyledBackground, true);

    // 레거시 showToast 스타일시트 그대로
    setStyleSheet(QStringLiteral(
        "QFrame#toast { background-color: #ffffff; border: 1px solid #c8c8c8; "
        "border-radius: 6px; }"
        "QLabel { background-color: transparent; color: #222; }"
        "QPushButton { color: #222; background-color: #f7f7f7; border: 1px solid #bdbdbd; "
        "border-radius: 3px; padding: 5px 11px; font-size: 12px; }"
        "QPushButton:hover { background-color: #e8f2ff; border-color: #0078d4; }"));
}

// static
Toast* Toast::show(QWidget* parent,
                   const QString& title,
                   const QString& detail,
                   Level /*level*/,
                   int timeoutMs)
{
    // 이전 토스트 교체 (레거시 단일 인스턴스 보장)
    if (!s_current.isNull()) {
        s_current->deleteLater();
        s_current = nullptr;
    }

    if (!parent) return nullptr;

    auto* toast = new Toast(parent);
    s_current = toast;

    // 너비: 레거시와 동일한 공식
    const int toastWidth = qMin(380, qMax(260, parent->width() - 32));
    toast->setFixedWidth(toastWidth);

    auto* layout = new QVBoxLayout(toast);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(5);

    // 닫기 람다 — QPointer로 토스트 수명 안전하게 추적
    QPointer<Toast> toastPtr(toast);
    auto closeToast = [toastPtr]() {
        if (!toastPtr.isNull()) {
            if (s_current == toastPtr.data()) s_current = nullptr;
            toastPtr->deleteLater();
        }
    };

    // ── 헤더 행 (제목 + × 버튼) ──────────────────────────────────────────
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);

    auto* titleLabel = new QLabel(title, toast);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: bold;"));
    titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    headerRow->addWidget(titleLabel, 1);

    auto* closeBtn = new QPushButton(QStringLiteral("\xc3\x97"), toast);  // UTF-8 × (U+00D7)
    closeBtn->setFixedSize(22, 22);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: #666; background-color: transparent; border: none; "
        "border-radius: 3px; padding: 0; font-size: 16px; }"
        "QPushButton:hover { color: #111; background-color: #e5e5e5; }"));
    QObject::connect(closeBtn, &QPushButton::clicked, toast, closeToast);
    headerRow->addWidget(closeBtn);
    layout->addLayout(headerRow);

    // ── 상세 레이블 ──────────────────────────────────────────────────────
    if (!detail.isEmpty()) {
        auto* detailLabel = new QLabel(detail, toast);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet(QStringLiteral("color: #555; font-size: 12px;"));
        detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(detailLabel);
    }

    toast->adjustSize();
    toast->positionSelf();
    toast->QFrame::show();
    toast->raise();

    // 자동소멸 타이머
    QTimer::singleShot(timeoutMs, toast, closeToast);

    return toast;
}

// static
void Toast::reposition(QWidget* parent)
{
    if (s_current.isNull()) return;
    if (s_current->parentWidget() != parent) return;
    s_current->positionSelf();
}

void Toast::positionSelf()
{
    auto* p = parentWidget();
    if (!p) return;

    // 레거시 positionToast: margin=14, 상태바(우리는 커스텀 상태바이므로 별도 보정 없음)
    constexpr int margin = 14;
    const int x = qMax(margin, p->width() - width() - margin);
    const int y = qMax(margin, p->height() - height() - margin);
    move(x, y);
}

} // namespace nv::ui
