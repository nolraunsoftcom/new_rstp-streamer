#include "MainWindow.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "src/ui/grid/VideoTileWidget.h"

namespace nv::ui {

namespace {
const char* kStageNames[] = {"장비도달", "Relay수신", "RTSP세션", "패킷수신", "디코딩", "표시"};
// StageState: 0 Unknown, 1 Ok, 2 Failed, 3 NotApplicable
const char* kStageMark[] = {"○", "●", "✗", "—"};
const char* kStageColor[] = {"gray", "limegreen", "red", "gray"};
} // namespace

MainWindow::MainWindow(nv::infra::LatestFrameSlot& slot, Commands commands)
    : m_commands(std::move(commands)) {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* top = new QHBoxLayout();
    m_url = new QLineEdit(QStringLiteral("rtsp://169.254.4.1:8900/live"), central);
    auto* btnConnect = new QPushButton(QStringLiteral("연결"), central);
    auto* btnDisconnect = new QPushButton(QStringLiteral("해제"), central);
    auto* btnRetry = new QPushButton(QStringLiteral("재시도"), central);
    top->addWidget(m_url, 1);
    top->addWidget(btnConnect);
    top->addWidget(btnDisconnect);
    top->addWidget(btnRetry);
    root->addLayout(top);

    m_tile = new VideoTileWidget(slot, central);
    root->addWidget(m_tile, 1);

    auto* diag = new QHBoxLayout();
    for (auto* name : kStageNames) {
        auto* lbl = new QLabel(QStringLiteral("○ %1").arg(QString::fromUtf8(name)), central);
        m_stageLabels.push_back(lbl);
        diag->addWidget(lbl);
    }
    diag->addStretch(1);
    root->addLayout(diag);

    m_flow = new QLabel(QStringLiteral("패킷 — (수신 이력 없음)"), central);
    root->addWidget(m_flow);

    m_status = new QLabel(QStringLiteral("Idle"), central);
    root->addWidget(m_status);

    setCentralWidget(central);
    setWindowTitle(QStringLiteral("new_viewer M1"));
    resize(800, 640);

    connect(btnConnect, &QPushButton::clicked, this,
            [this] { m_commands.connectTo(m_url->text().toStdString()); });
    connect(btnDisconnect, &QPushButton::clicked, this, [this] { m_commands.disconnect(); });
    connect(btnRetry, &QPushButton::clicked, this, [this] { m_commands.retry(); });
}

void MainWindow::onSnapshot(QString state, int attempts, QString reason, QList<int> stages,
                            double pps, qlonglong msSince) {
    m_status->setText(QStringLiteral("%1 | 시도 %2 | %3").arg(state).arg(attempts).arg(reason));
    for (int i = 0; i < stages.size() && i < m_stageLabels.size(); ++i) {
        const int s = stages[i];
        m_stageLabels[i]->setText(QStringLiteral("%1 %2")
                                      .arg(QString::fromUtf8(kStageMark[s]))
                                      .arg(QString::fromUtf8(kStageNames[i])));
        m_stageLabels[i]->setStyleSheet(QStringLiteral("color:%1").arg(kStageColor[s]));
    }
    if (msSince < 0) {
        m_flow->setText(QStringLiteral("패킷 — (수신 이력 없음)"));
        m_flow->setStyleSheet(QStringLiteral("color:gray"));
    } else {
        m_flow->setText(QStringLiteral("패킷 %1/s · 마지막 %2초 전")
                            .arg(pps, 0, 'f', 1)
                            .arg(msSince / 1000.0, 0, 'f', 1));
        const char* color = (msSince < 1000) ? "limegreen" : (msSince < 3000) ? "orange" : "red";
        m_flow->setStyleSheet(QStringLiteral("color:%1; font-weight:bold").arg(color));
    }
}

} // namespace nv::ui
