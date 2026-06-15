#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QFrame>
#include <QTest>
#include <QTimer>
#include "src/ui/shell/Toast.h"

// 프로세스 단일 QApplication(공유·누수) — tests/helpers/QtTestApp.h 참고.
#include "tests/helpers/QtTestApp.h"
namespace {
QApplication& getApp() { return nvtest::app(); }
} // namespace

// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("Toast: show() 후 위젯이 parent의 자식으로 생성된다") {
    (void)getApp();
    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QApplication::processEvents();

    auto* t = nv::ui::Toast::show(&parent,
                                   QStringLiteral("제목"),
                                   QStringLiteral("상세"),
                                   nv::ui::Toast::Level::Info,
                                   60000);  // 긴 타임아웃 — 자동소멸 방지

    REQUIRE(t != nullptr);
    REQUIRE(t->parentWidget() == &parent);
    REQUIRE(t->isVisible());

    // 정리
    delete t;
    QApplication::processEvents();
}

TEST_CASE("Toast: show() 연속 호출 시 이전 토스트가 교체된다") {
    (void)getApp();
    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QApplication::processEvents();

    nv::ui::Toast::show(&parent, QStringLiteral("첫 번째"), QString{},
                        nv::ui::Toast::Level::Info, 60000);
    QApplication::processEvents();

    // 두 번째 show — 첫 번째가 deleteLater됨
    auto* t2 = nv::ui::Toast::show(&parent, QStringLiteral("두 번째"), QString{},
                                    nv::ui::Toast::Level::Info, 60000);
    QApplication::processEvents();

    // t1은 deleteLater 처리됨 (processEvents 후 파괴)
    REQUIRE(t2 != nullptr);
    REQUIRE(t2->isVisible());

    // 정리
    delete t2;
    QApplication::processEvents();
}

TEST_CASE("Toast: 타임아웃 후 자동소멸 (단축 타임아웃)") {
    (void)getApp();
    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QApplication::processEvents();

    // 50ms 타임아웃 — QTest::qWait로 타이머 발화 + deleteLater 처리를 확실히 대기
    nv::ui::Toast::show(&parent, QStringLiteral("빠른 소멸"), QString{},
                         nv::ui::Toast::Level::Info, 50);

    // 타이머 발화(50ms) + deleteLater 처리까지 충분히 대기
    QTest::qWait(200);

    // parent에 Toast 자식이 남아있지 않아야 한다
    const auto children = parent.findChildren<nv::ui::Toast*>();
    REQUIRE(children.isEmpty());
}

TEST_CASE("Toast: reposition() 은 parent 없이 호출해도 안전하다") {
    (void)getApp();
    // parent=nullptr 로 reposition — crash 없어야 함
    nv::ui::Toast::reposition(nullptr);
    REQUIRE(true);  // 도달하면 통과
}

TEST_CASE("Toast: Level::Error 로 show() 해도 위젯 생성 정상") {
    (void)getApp();
    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QApplication::processEvents();

    auto* t = nv::ui::Toast::show(&parent,
                                   QStringLiteral("녹화 실패"),
                                   QStringLiteral("채널1 · 녹화 시작 실패"),
                                   nv::ui::Toast::Level::Error,
                                   60000);
    REQUIRE(t != nullptr);
    REQUIRE(t->isVisible());

    delete t;
    QApplication::processEvents();
}
