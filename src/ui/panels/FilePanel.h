#pragma once
#include <QWidget>
#include <QFileSystemWatcher>
#include <QHash>
#include <QIcon>
#include <QString>

class QButtonGroup;
class QListWidget;
class QLabel;
class QPushButton;
class QTimer;

namespace nv::ui {

// 우측 "파일" 탭 — 녹화(MKV)/스냅샷(PNG) 목록 표시.
// RecordingPaths::baseDir()을 스캔해 파일명·크기·수정시각을 목록으로 표시.
// PNG는 썸네일, MKV는 비디오 아이콘. 더블클릭 → QDesktopServices::openUrl.
// 갱신: 새로고침 버튼 + QFileSystemWatcher(디렉토리 변경 감지) 자동 갱신.
//
// P2 추가:
//   - 타입 토글 (스냅샷/녹화 라디오, 확장자 필터): QButtonGroup, id 0=스냅샷, 1=녹화
//   - 폴더 열기 버튼 (📁): QDesktopServices::openUrl on baseDir
//   - 우클릭 컨텍스트 메뉴: 열기 / Finder에서 보기 / 삭제
//   - 썸네일 64×48 (레거시 일치)
class FilePanel : public QWidget {
    Q_OBJECT
public:
    explicit FilePanel(QWidget* parent = nullptr);

    // 파일 목록 강제 갱신 (녹화 중지/스냅샷 완료 시 main.cpp에서 호출 가능)
    Q_INVOKABLE void refresh();

private:
    void setupUi();
    void openItem(int row);
    void showContextMenu(const QPoint& pos);

    // PNG 썸네일 캐시: 경로+수정시각으로 키를 만들어 이미 축소 디코드한 아이콘을 재사용한다.
    // (D5: 매 refresh마다 모든 PNG를 풀 디코드하면 파일이 쌓일수록 UI가 멈춘다.)
    QIcon thumbnailFor(const QString& absPath, qint64 mtimeEpoch);

    // 비디오 플레이스홀더 아이콘 (필름스트립 + 재생 삼각형)
    QIcon makeVideoThumb(const QSize& target);

    QString      m_baseDir;
    QLabel*      m_dirLabel    = nullptr;
    QPushButton* m_refreshBtn  = nullptr;
    QPushButton* m_openDirBtn  = nullptr;
    QListWidget* m_list        = nullptr;
    QButtonGroup* m_fileTypeGroup = nullptr;  // 0=스냅샷, 1=녹화

    int m_currentFileType = 0;  // 0=스냅샷(PNG), 1=녹화(MKV)

    QFileSystemWatcher m_watcher;
    QHash<QString, QIcon> m_thumbCache;   // 키: "<absPath>|<mtimeEpoch>" → 축소 아이콘
};

} // namespace nv::ui
