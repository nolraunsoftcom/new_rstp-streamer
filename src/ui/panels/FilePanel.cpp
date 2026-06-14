#include "FilePanel.h"
#include <QButtonGroup>
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
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include "src/infra/persist/RecordingPaths.h"
#include "src/ui/common/Style.h"

namespace nv::ui {

// 레거시 legacy viewer와 일치: 64×48
static constexpr int kThumbW = 64;
static constexpr int kThumbH = 48;

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
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    // ── 타입 토글 행: [스냅샷] [녹화]  stretch  [↻] [📁]
    auto* typeRow = new QHBoxLayout();
    typeRow->setSpacing(4);

    m_fileTypeGroup = new QButtonGroup(this);
    m_fileTypeGroup->setExclusive(true);

    const QString toggleStyle = QStringLiteral(
        "QPushButton { color: #333; background-color: #ffffff; border: 1px solid #bdbdbd; "
        "border-radius: 3px; font-size: 11px; padding: 0 12px; }"
        "QPushButton:checked { color: #111; background-color: #cfe8ff; border-color: #0078d4; }"
        "QPushButton:hover { background-color: #e8f2ff; }");

    const QStringList typeLabels = {QStringLiteral("스냅샷"), QStringLiteral("녹화")};
    for (int i = 0; i < typeLabels.size(); ++i) {
        auto* btn = new QPushButton(typeLabels[i], this);
        btn->setCheckable(true);
        btn->setFixedHeight(28);
        btn->setStyleSheet(toggleStyle);
        m_fileTypeGroup->addButton(btn, i);
        typeRow->addWidget(btn);
        if (i == 0) btn->setChecked(true);
    }
    typeRow->addStretch();

    const QString iconBtnStyle = QStringLiteral(
        "QPushButton { color: #333; background-color: #ffffff; border: 1px solid #bdbdbd; "
        "border-radius: 3px; font-size: 13px; }"
        "QPushButton:hover { background-color: #e8f2ff; border-color: #0078d4; }");

    m_refreshBtn = new QPushButton(QStringLiteral("↻"), this);
    m_refreshBtn->setFixedSize(28, 28);
    m_refreshBtn->setToolTip(QStringLiteral("새로고침"));
    m_refreshBtn->setStyleSheet(iconBtnStyle);
    connect(m_refreshBtn, &QPushButton::clicked, this, &FilePanel::refresh);
    typeRow->addWidget(m_refreshBtn);

    m_openDirBtn = new QPushButton(QStringLiteral("📁"), this);
    m_openDirBtn->setFixedSize(28, 28);
    m_openDirBtn->setToolTip(QStringLiteral("폴더 열기"));
    m_openDirBtn->setStyleSheet(iconBtnStyle);
    connect(m_openDirBtn, &QPushButton::clicked, this, [this]() {
        QDir().mkpath(m_baseDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_baseDir));
    });
    typeRow->addWidget(m_openDirBtn);

    lay->addLayout(typeRow);

    // 타입 토글 변경 → 필터 재적용
    connect(m_fileTypeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_currentFileType = id;
        refresh();
    });

    // ── 디렉토리 경로 라벨
    m_dirLabel = new QLabel(this);
    m_dirLabel->setText(m_baseDir);
    m_dirLabel->setStyleSheet(QStringLiteral(
        "color:#777; font-size:9px;"));
    m_dirLabel->setWordWrap(true);
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_dirLabel);

    // ── 파일 목록
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(kThumbW, kThumbH));   // 레거시: 64×48
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setWordWrap(true);
    m_list->setTextElideMode(Qt::ElideNone);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setSpacing(2);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { background-color: #ffffff; color: #222; border: 1px solid #d0d0d0; "
        "font-size: 11px; outline: none; }"
        "QListWidget::item { padding: 10px 8px; border-bottom: 1px solid #e5e5e5; }"
        "QListWidget::item:selected { background-color: #cfe8ff; color: #111; }"
        "QListWidget::item:hover { background-color: #f3f8ff; }"));

    // 더블클릭 → 열기
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
        openItem(m_list->row(item));
    });

    // 우클릭 → 컨텍스트 메뉴
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested,
            this, &FilePanel::showContextMenu);

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

    // 타입 토글에 따라 확장자 필터: 0=스냅샷(PNG), 1=녹화(MKV)
    const bool isVideo = (m_currentFileType == 1);
    const QStringList filters = isVideo
        ? QStringList{QStringLiteral("*.mkv"), QStringLiteral("*.mp4"), QStringLiteral("*.mov")}
        : QStringList{QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg")};

    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    // H4: 썸네일 캐시 정리 — PNG 파일만 해당, 현존 파일 키만 남긴다.
    if (!isVideo) {
        QHash<QString, QIcon> live;
        for (const QFileInfo& fi : files) {
            const QString key = fi.absoluteFilePath() + QLatin1Char('|')
                                + QString::number(fi.lastModified().toSecsSinceEpoch());
            auto it = m_thumbCache.constFind(key);
            if (it != m_thumbCache.constEnd()) live.insert(key, it.value());
        }
        m_thumbCache.swap(live);
    }

    const QSize iconSize(kThumbW, kThumbH);
    QIcon videoThumb;  // 비디오 플레이스홀더 — 한 번만 생성

    for (const QFileInfo& fi : files) {
        // 아이콘
        QIcon icon;
        if (isVideo) {
            if (videoThumb.isNull()) videoThumb = makeVideoThumb(iconSize);
            icon = videoThumb;
        } else {
            // D5: 축소 디코드 + 캐시
            icon = thumbnailFor(fi.absoluteFilePath(), fi.lastModified().toSecsSinceEpoch());
            if (icon.isNull()) {
                // 폴백: 녹색 단색 블록
                QPixmap pm(kThumbW, kThumbH);
                pm.fill(QColor(0x20, 0x90, 0x60));
                icon = QIcon(pm);
            }
        }

        // 파일 크기 표시
        const qint64 bytes = fi.size();
        QString sizeStr;
        if (bytes >= 1024LL * 1024 * 1024)
            sizeStr = QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
        else if (bytes >= 1024 * 1024)
            sizeStr = QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
        else if (bytes >= 1024)
            sizeStr = QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
        else
            sizeStr = QStringLiteral("%1 B").arg(bytes);

        const QString modified = fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        const QString label = QStringLiteral("%1\n%2\n%3")
            .arg(fi.fileName(), modified, sizeStr);

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

    // 축소 디코드: KeepAspectRatio로 64×48 내에 맞추고 직접 디코드한다(메모리/시간 절약).
    QImageReader reader(absPath);
    if (!reader.canRead()) {
        return icon;   // 쓰는 중/손상 — 폴백 아이콘, 캐시 안 함
    }
    const QSize orig = reader.size();
    if (orig.isValid()) {
        reader.setScaledSize(orig.scaled(QSize(kThumbW, kThumbH), Qt::KeepAspectRatio));
    } else {
        reader.setScaledSize(QSize(kThumbW, kThumbH));
    }
    reader.setAutoTransform(true);
    const QImage img = reader.read();

    if (!img.isNull()) {
        icon = QIcon(QPixmap::fromImage(img));
        m_thumbCache.insert(key, icon);   // 성공한 썸네일만 캐시(실패는 폴백 아이콘 사용)
    }
    return icon;
}

QIcon FilePanel::makeVideoThumb(const QSize& target)
{
    // 레거시 makeVideoThumb과 동일 — 필름 스트립 스타일 플레이스홀더
    QPixmap pm(target);
    pm.fill(QColor(QStringLiteral("#e5e5e5")));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // 필름 스트립 양옆 구멍
    p.setBrush(QColor(QStringLiteral("#bdbdbd")));
    p.setPen(Qt::NoPen);
    const int h = target.height();
    const int w = target.width();
    constexpr int holeW = 6, holeH = 4, gap = 2;
    for (int y = 4; y + holeH <= h - 4; y += holeH + gap) {
        p.drawRect(3, y, holeW, holeH);
        p.drawRect(w - 3 - holeW, y, holeW, holeH);
    }

    // 중앙 재생 삼각형
    p.setBrush(QColor(QStringLiteral("#0078d4")));
    const int cx = w / 2, cy = h / 2, r = h / 4;
    QPolygon tri;
    tri << QPoint(cx - r + 2, cy - r)
        << QPoint(cx - r + 2, cy + r)
        << QPoint(cx + r + 2, cy);
    p.drawPolygon(tri);
    return QIcon(pm);
}

void FilePanel::openItem(int row)
{
    if (row < 0 || row >= m_list->count()) return;
    const QString path = m_list->item(row)->data(Qt::UserRole).toString();
    if (!path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void FilePanel::showContextMenu(const QPoint& pos)
{
    auto* item = m_list->itemAt(pos);
    if (!item) return;

    const QString path = item->data(Qt::UserRole).toString();

    QMenu menu(this);
    menu.setStyleSheet(nv::ui::style::MENU);

    auto* openAction   = menu.addAction(QStringLiteral("열기"));
    auto* revealAction = menu.addAction(
#if defined(__APPLE__)
        QStringLiteral("Finder에서 보기")
#elif defined(_WIN32)
        QStringLiteral("탐색기에서 보기")
#else
        QStringLiteral("파일 관리자에서 보기")
#endif
    );
    menu.addSeparator();
    auto* deleteAction = menu.addAction(QStringLiteral("삭제"));

    auto* selected = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!selected) return;

    if (selected == openAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else if (selected == revealAction) {
#if defined(__APPLE__)
        QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#elif defined(_WIN32)
        QProcess::startDetached(QStringLiteral("explorer"),
                                {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
    } else if (selected == deleteAction) {
        const auto ret = QMessageBox::question(
            this,
            QStringLiteral("파일 삭제"),
            QStringLiteral("'%1' 파일을 삭제하시겠습니까?").arg(QFileInfo(path).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            QFile::remove(path);
            refresh();
        }
    }
}

} // namespace nv::ui
