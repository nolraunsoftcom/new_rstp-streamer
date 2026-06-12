#include "FilePanel.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include "src/infra/persist/RecordingPaths.h"

namespace nv::ui {

// MKV 파일 용 단순 비디오 아이콘 (텍스트 기반 폴백)
static constexpr int kThumbSize = 48;

FilePanel::FilePanel(QWidget* parent)
    : QWidget(parent)
{
    m_baseDir = QString::fromStdString(nv::infra::RecordingPaths::baseDir());
    setupUi();

    // QFileSystemWatcher — 디렉토리 변경 감지 시 자동 갱신
    m_watcher.addPath(m_baseDir);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &FilePanel::refresh);

    refresh();
}

void FilePanel::setupUi()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // 헤더: 디렉토리 경로 + 새로고침 버튼
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);

    m_dirLabel = new QLabel(this);
    m_dirLabel->setText(m_baseDir);
    m_dirLabel->setStyleSheet(QStringLiteral(
        "color:#777; font-size:9px;"));
    m_dirLabel->setWordWrap(true);
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerRow->addWidget(m_dirLabel, 1);

    m_refreshBtn = new QPushButton(QStringLiteral("↺"), this);
    m_refreshBtn->setFixedSize(24, 24);
    m_refreshBtn->setToolTip(QStringLiteral("목록 새로고침"));
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color:#555; background:#fff; border:1px solid #bbb; "
        "border-radius:3px; font-size:14px; }"
        "QPushButton:hover { background:#e8f2ff; }"));
    connect(m_refreshBtn, &QPushButton::clicked, this, &FilePanel::refresh);
    headerRow->addWidget(m_refreshBtn);

    lay->addLayout(headerRow);

    // 파일 목록
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(kThumbSize, kThumbSize));
    m_list->setSpacing(2);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { border:1px solid #ddd; background:#fff; font-size:11px; }"
        "QListWidget::item { padding:4px; border-bottom:1px solid #f0f0f0; }"
        "QListWidget::item:selected { background:#dbeafe; color:#111; }"));
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
        const int row = m_list->row(item);
        openItem(row);
    });
    lay->addWidget(m_list, 1);

    // 파일 수 라벨
    auto* countLabel = new QLabel(this);
    countLabel->setObjectName(QStringLiteral("countLabel"));
    countLabel->setStyleSheet(QStringLiteral("color:#999; font-size:10px;"));
    lay->addWidget(countLabel);
}

void FilePanel::refresh()
{
    m_list->clear();

    QDir dir(m_baseDir);
    if (!dir.exists()) {
        dir.mkpath(m_baseDir);
    }

    // MKV + PNG 파일만, 수정시각 내림차순
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << QStringLiteral("*.mkv") << QStringLiteral("*.png"),
        QDir::Files, QDir::Time);

    // H4: 썸네일 캐시 정리 — 현존 파일 키만 남긴다.
    // 파일 갱신·삭제 시 구 항목이 남아 캐시가 무한 성장하는 것을 방지한다.
    {
        QHash<QString, QIcon> live;
        for (const QFileInfo& fi : files) {
            if (fi.suffix().toLower() != QStringLiteral("png")) continue;
            const QString key = fi.absoluteFilePath() + QLatin1Char('|')
                                + QString::number(fi.lastModified().toSecsSinceEpoch());
            auto it = m_thumbCache.constFind(key);
            if (it != m_thumbCache.constEnd()) live.insert(key, it.value());
        }
        m_thumbCache.swap(live);
    }

    for (const QFileInfo& fi : files) {
        const bool isPng = fi.suffix().toLower() == QStringLiteral("png");

        // 아이콘
        QIcon icon;
        if (isPng) {
            // D5: 풀 디코드 대신 QImageReader::setScaledSize로 썸네일 크기로만 디코드하고,
            // 경로+수정시각 캐시로 이미 만든 썸네일을 재사용한다.
            icon = thumbnailFor(fi.absoluteFilePath(), fi.lastModified().toSecsSinceEpoch());
        }
        if (icon.isNull()) {
            // 텍스트 아이콘 폴백 (MKV 또는 PNG 로드 실패)
            QPixmap pm(kThumbSize, kThumbSize);
            pm.fill(isPng ? QColor(0x20, 0x90, 0x60) : QColor(0x00, 0x60, 0xA0));
            icon = QIcon(pm);
        }

        // 파일 크기 표시
        const qint64 bytes = fi.size();
        QString sizeStr;
        if (bytes >= 1024 * 1024)
            sizeStr = QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
        else if (bytes >= 1024)
            sizeStr = QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
        else
            sizeStr = QStringLiteral("%1 B").arg(bytes);

        const QString modified = fi.lastModified().toString(QStringLiteral("MM-dd HH:mm:ss"));
        const QString label = QStringLiteral("%1\n%2  %3")
            .arg(fi.fileName(), sizeStr, modified);

        auto* item = new QListWidgetItem(icon, label);
        item->setData(Qt::UserRole, fi.absoluteFilePath());
        item->setToolTip(fi.absoluteFilePath());
        m_list->addItem(item);
    }

    // 파일 수 갱신
    if (auto* lbl = findChild<QLabel*>(QStringLiteral("countLabel"))) {
        lbl->setText(QStringLiteral("파일 %1개").arg(files.size()));
    }

    // watcher 경로가 사라졌다면 재등록
    if (!m_watcher.directories().contains(m_baseDir)) {
        m_watcher.addPath(m_baseDir);
    }
}

QIcon FilePanel::thumbnailFor(const QString& absPath, qint64 mtimeEpoch)
{
    // 캐시 키: 경로 + 수정시각. 파일이 같은 경로로 갱신되면 mtime이 달라져 재디코드된다.
    const QString key = absPath + QLatin1Char('|') + QString::number(mtimeEpoch);
    const auto it = m_thumbCache.constFind(key);
    if (it != m_thumbCache.constEnd()) return it.value();

    QIcon icon;

    // 스냅샷이 쓰는 중이거나 0바이트인 PNG를 읽으면 "libpng error: Read Error" 스팸이
    // 발생한다. 디코드 전에 유효성을 확인한다: 0바이트/존재하지 않으면 스킵, QImageReader가
    // 헤더만으로 읽을 수 없다고 판단하면(canRead()==false) 스킵. 둘 다 폴백 아이콘 사용.
    const QFileInfo fi(absPath);
    if (!fi.exists() || fi.size() == 0) {
        return icon;   // 캐시하지 않음 — 완성되면 mtime 변경으로 재시도
    }

    // 축소 디코드: 원본을 풀 디코드하지 않고 썸네일 크기로 직접 디코드한다(메모리/시간 절약).
    QImageReader reader(absPath);
    if (!reader.canRead()) {
        return icon;   // 쓰는 중/손상 — 폴백 아이콘, 캐시 안 함
    }
    reader.setScaledSize(QSize(kThumbSize, kThumbSize));
    reader.setAutoTransform(true);
    const QImage img = reader.read();

    if (!img.isNull()) {
        icon = QIcon(QPixmap::fromImage(img));
        m_thumbCache.insert(key, icon);   // 성공한 썸네일만 캐시(실패는 폴백 아이콘 사용)
    }
    return icon;
}

void FilePanel::openItem(int row)
{
    if (row < 0 || row >= m_list->count()) return;
    const QString path = m_list->item(row)->data(Qt::UserRole).toString();
    if (!path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

} // namespace nv::ui
