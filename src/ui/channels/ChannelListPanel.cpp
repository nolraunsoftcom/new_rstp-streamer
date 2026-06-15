#include "ChannelListPanel.h"
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include "src/ui/common/Confirm.h"

namespace nv::ui {

namespace {
// InternalMove 드롭 후 새 행 순서를 콜백으로 알리는 QListWidget. InternalMove는 Qt 버전에
// 따라 rowsMoved 대신 remove+insert로 구현될 수 있어 dropEvent 종료 시점에 순서를 읽는다.
// Q_OBJECT 미사용(시그널/슬롯 없음, std::function 콜백) — moc 불필요.
class ReorderListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void()> onReordered;

protected:
    void dropEvent(QDropEvent* e) override {
        const int before = count();
        QListWidget::dropEvent(e);
        // 내부 이동만 처리(외부 드롭으로 행 수가 바뀌면 무시).
        if (count() == before && onReordered) onReordered();
    }
};
} // namespace

static constexpr int kHeaderHeight = 32;
static const QString kHeaderStyle = QStringLiteral(
    "background-color: #f3f3f3; border-bottom: 1px solid #d0d0d0;");
static const QString kBtnStyle = QStringLiteral(
    "QPushButton { color: #222; background-color: #f7f7f7; border: 1px solid #bdbdbd; font-size: 13px; }"
    "QPushButton:hover { background-color: #e8f2ff; border-color: #0078d4; }");

ChannelListPanel::ChannelListPanel(Callbacks cb, QWidget* parent)
    : QWidget(parent), m_cb(std::move(cb)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 헤더
    auto* header = new QWidget(this);
    header->setFixedHeight(kHeaderHeight);
    header->setStyleSheet(kHeaderStyle);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 0, 10, 0);
    auto* title = new QLabel(QStringLiteral("채널 목록"), header);
    title->setStyleSheet(QStringLiteral("color: #333; font-size: 11px; font-weight: bold;"));
    headerLayout->addWidget(title);
    layout->addWidget(header);

    // 채널 목록
    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(8, 8, 8, 8);
    bodyLayout->setSpacing(4);

    auto* listWidget = new ReorderListWidget(body);
    m_list = listWidget;
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 가로 스크롤 없이 긴 이름/URL은 줄바꿈 처리(생략표시 없음).
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setWordWrap(true);
    m_list->setTextElideMode(Qt::ElideNone);
    m_list->setResizeMode(QListView::Adjust);   // 폭 변경 시 항목 높이 재계산(줄바꿈 반영)
    // DnD 재배열(레거시 채널 리스트 재정렬 대응). InternalMove + 행간 드롭 인디케이터.
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setDropIndicatorShown(true);
    listWidget->onReordered = [this]() {
        if (!m_cb.reorderRequested) return;
        std::vector<std::string> order;
        order.reserve(static_cast<size_t>(m_list->count()));
        for (int i = 0; i < m_list->count(); ++i)
            order.push_back(m_list->item(i)->data(Qt::UserRole).toString().toStdString());
        m_cb.reorderRequested(std::move(order));
    };
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { background-color: #ffffff; color: #222; border: 1px solid #d0d0d0; }"
        "QListWidget::item { padding: 4px 8px; border-bottom: 1px solid #eeeeee; }"
        "QListWidget::item:selected { background-color: #cfe8ff; color: #111; }"));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = m_list->itemAt(pos);
        if (!item) return;
        const int row = m_list->row(item);
        if (row < 0 || row >= static_cast<int>(m_configs.size())) return;
        const std::string id = m_configs[static_cast<size_t>(row)].id;

        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background-color: #ffffff; color: #1f1f1f; border: 1px solid #c8c8c8; font-size: 12px; }"
            "QMenu::item { padding: 6px 20px; }"
            "QMenu::item:selected { background-color: #dbeafe; }"));
        auto* actEdit = menu.addAction(QStringLiteral("채널 수정"));
        auto* actRetry = menu.addAction(QStringLiteral("재시도"));
        menu.addSeparator();
        auto* actRemove = menu.addAction(QStringLiteral("채널 삭제"));
        auto* chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (chosen == actEdit) m_cb.editRequested(id);
        else if (chosen == actRetry) m_cb.retryRequested(id);
        else if (chosen == actRemove) {
            // U1: 삭제 확인 다이얼로그 (F5: confirmDelete 헬퍼)
            const QString chName = QString::fromStdString(
                m_configs[static_cast<size_t>(row)].name);
            if (nv::ui::confirmDelete(this, chName)) m_cb.removeRequested(id);
        }
    });
    bodyLayout->addWidget(m_list, 1);

    // 하단 버튼
    auto* footerLayout = new QHBoxLayout();
    footerLayout->setContentsMargins(0, 4, 0, 0);
    footerLayout->setSpacing(2);
    m_addBtn = new QPushButton(QStringLiteral("+"), body);
    m_addBtn->setFixedHeight(26);
    m_addBtn->setStyleSheet(kBtnStyle);
    connect(m_addBtn, &QPushButton::clicked, this, [this] { m_cb.addRequested(); });
    m_removeBtn = new QPushButton(QStringLiteral("-"), body);
    m_removeBtn->setFixedHeight(26);
    m_removeBtn->setStyleSheet(kBtnStyle);
    connect(m_removeBtn, &QPushButton::clicked, this, [this] {
        auto* item = m_list->currentItem();
        if (!item) return;
        const int row = m_list->row(item);
        if (row >= 0 && row < static_cast<int>(m_configs.size())) {
            // U1: 삭제 확인 다이얼로그 (F5: confirmDelete 헬퍼)
            const QString chName = QString::fromStdString(
                m_configs[static_cast<size_t>(row)].name);
            if (nv::ui::confirmDelete(this, chName))
                m_cb.removeRequested(m_configs[static_cast<size_t>(row)].id);
        }
    });
    footerLayout->addWidget(m_addBtn, 1);
    footerLayout->addWidget(m_removeBtn, 1);
    bodyLayout->addLayout(footerLayout);

    layout->addWidget(body, 1);
}

void ChannelListPanel::updateChannels(const std::vector<nv::domain::ChannelConfig>& configs) {
    m_configs = configs;
    m_list->clear();
    for (const auto& cfg : configs) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(QString::fromStdString(cfg.name),
                                         QString::fromStdString(cfg.url)));
        // DnD 재배열 후 행→채널 매핑용. 드롭 시 UserRole에서 id 순서를 읽는다.
        item->setData(Qt::UserRole, QString::fromStdString(cfg.id));
        m_list->addItem(item);
    }
}

// 레거시 VlcWidget::statusText() + applyChannelStatusLabel() 매핑
static QString channelStatusText(const QString& state) {
    if (state == QStringLiteral("Streaming"))    return QStringLiteral("연결됨");
    if (state == QStringLiteral("Connecting"))   return QStringLiteral("연결 중");
    if (state == QStringLiteral("SessionOpen"))  return QStringLiteral("연결 중");
    if (state == QStringLiteral("Reconnecting")) return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Stalled"))      return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Failed"))       return QStringLiteral("실패");
    if (state == QStringLiteral("Idle"))         return QStringLiteral("대기");
    return state;
}

static QString channelStatusColor(const QString& state) {
    if (state == QStringLiteral("Streaming"))    return QStringLiteral("#12823b");
    if (state == QStringLiteral("Connecting"))   return QStringLiteral("#666");
    if (state == QStringLiteral("SessionOpen"))  return QStringLiteral("#666");
    if (state == QStringLiteral("Reconnecting")) return QStringLiteral("#e8a838");
    if (state == QStringLiteral("Stalled"))      return QStringLiteral("#e8a838");
    if (state == QStringLiteral("Failed"))       return QStringLiteral("#d13438");
    return QStringLiteral("#666");
}

void ChannelListPanel::updateStatus(const QString& channelId, const QString& state,
                                    const QString& reason) {
    for (int i = 0; i < static_cast<int>(m_configs.size()); ++i) {
        if (QString::fromStdString(m_configs[static_cast<size_t>(i)].id) == channelId) {
            auto* item = m_list->item(i);
            if (item) {
                const QString displayState = channelStatusText(state);
                const QString color        = channelStatusColor(state);
                QString statusStr = displayState;
                if (reason != QStringLiteral("None") && !reason.isEmpty()) {
                    statusStr += QStringLiteral(" (%1)").arg(reason);
                }
                item->setText(QStringLiteral("%1  [%2]\n%3")
                                  .arg(QString::fromStdString(m_configs[static_cast<size_t>(i)].name),
                                       statusStr,
                                       QString::fromStdString(m_configs[static_cast<size_t>(i)].url)));
                item->setForeground(QColor(color));
            }
            break;
        }
    }
}

} // namespace nv::ui
