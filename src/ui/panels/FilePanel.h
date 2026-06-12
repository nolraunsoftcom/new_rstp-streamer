#pragma once
#include <QWidget>
#include <QFileSystemWatcher>

class QListWidget;
class QLabel;
class QPushButton;
class QTimer;

namespace nv::ui {

// 우측 "파일" 탭 — 녹화(MKV)/스냅샷(PNG) 목록 표시.
// RecordingPaths::baseDir()을 스캔해 파일명·크기·수정시각을 목록으로 표시.
// PNG는 썸네일, MKV는 비디오 아이콘. 더블클릭 → QDesktopServices::openUrl.
// 갱신: 새로고침 버튼 + QFileSystemWatcher(디렉토리 변경 감지) 자동 갱신.
class FilePanel : public QWidget {
    Q_OBJECT
public:
    explicit FilePanel(QWidget* parent = nullptr);

    // 파일 목록 강제 갱신 (녹화 중지/스냅샷 완료 시 main.cpp에서 호출 가능)
    Q_INVOKABLE void refresh();

private:
    void setupUi();
    void openItem(int row);

    QString      m_baseDir;
    QLabel*      m_dirLabel    = nullptr;
    QPushButton* m_refreshBtn  = nullptr;
    QListWidget* m_list        = nullptr;

    QFileSystemWatcher m_watcher;
};

} // namespace nv::ui
