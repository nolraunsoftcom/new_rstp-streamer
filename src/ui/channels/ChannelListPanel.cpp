#include "ChannelListPanel.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace nv::ui {

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

    m_list = new QListWidget(body);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
        else if (chosen == actRemove) m_cb.removeRequested(id);
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
        if (row >= 0 && row < static_cast<int>(m_configs.size()))
            m_cb.removeRequested(m_configs[static_cast<size_t>(row)].id);
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
        m_list->addItem(QStringLiteral("%1\n%2")
                            .arg(QString::fromStdString(cfg.name),
                                 QString::fromStdString(cfg.url)));
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

void ChannelListPanel::updateStatus(const QString& channelId, const QString& state) {
    for (int i = 0; i < static_cast<int>(m_configs.size()); ++i) {
        if (QString::fromStdString(m_configs[static_cast<size_t>(i)].id) == channelId) {
            auto* item = m_list->item(i);
            if (item) {
                const QString displayState = channelStatusText(state);
                const QString color        = channelStatusColor(state);
                item->setText(QStringLiteral("%1  [%2]\n%3")
                                  .arg(QString::fromStdString(m_configs[static_cast<size_t>(i)].name),
                                       displayState,
                                       QString::fromStdString(m_configs[static_cast<size_t>(i)].url)));
                item->setForeground(QColor(color));
            }
            break;
        }
    }
}

} // namespace nv::ui
