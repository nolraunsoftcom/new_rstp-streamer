#pragma once
#include <QFrame>
#include <QList>
#include <QPointer>
#include <QString>
#include <functional>

// Toast: 레거시 showToast 미러.
// 흰 배경·#c8c8c8 테두리·6px 라운드·닫기× 버튼·액션 버튼·자동소멸(기본 3500ms)·우하단 배치.
// 단일 인스턴스 — show() 호출 시 이전 토스트를 삭제하고 새 토스트를 표시한다.
// 부모(parent) 위젯 위에 자식 프레임으로 올라오므로 부모의 resizeEvent에서
// repositionToast()를 호출해 위치를 갱신해야 한다.
//
// 사용법:
//   Toast::show(centralWidget(), "제목", "상세", Toast::Level::Info, 3500);
//   Toast::show(centralWidget(), "스냅샷 저장됨", name, Info, 3500,
//               {{"폴더", openDir}, {"열기", openFile}});  // 좌→우 순서
//   Toast::reposition(centralWidget());  // resizeEvent에서 호출

namespace nv::ui {

class Toast : public QFrame {
    Q_OBJECT
public:
    enum class Level { Info, Warn, Error };

    // 액션 버튼 — label과 클릭 시 콜백. 클릭하면 콜백 실행 후 토스트가 닫힌다.
    struct Action {
        QString label;
        std::function<void()> callback;
    };

    // 기존 토스트를 교체하고 새 토스트를 parent 위에 표시한다.
    // parent: 토스트를 올릴 위젯 (centralWidget() 권장).
    // actions: 우하단 정렬 액션 버튼 (리스트 순서대로 좌→우). 비어있으면 버튼 없음.
    // 반환값: 생성된 Toast 포인터 (QPointer로 추적해도 됨).
    static Toast* show(QWidget* parent,
                       const QString& title,
                       const QString& detail,
                       Level level = Level::Info,
                       int timeoutMs = 3500,
                       const QList<Action>& actions = {});

    // parent 위의 현재 살아있는 토스트를 우하단으로 재배치한다.
    // MainWindow::resizeEvent에서 호출해야 창 크기 변경 시 위치가 유지된다.
    static void reposition(QWidget* parent);

private:
    explicit Toast(QWidget* parent);

    // parent 내에서 살아있는 Toast를 추적하는 전역 포인터.
    // 단일 토스트 보장: 새 토스트가 표시될 때 이전 것이 있으면 파괴한다.
    static QPointer<Toast> s_current;

    void positionSelf();
};

} // namespace nv::ui
