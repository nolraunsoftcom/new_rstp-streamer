#pragma once
#include <QApplication>

// 테스트 프로세스당 단일 QApplication.
//
// 기존엔 각 테스트 TU가 함수-로컬 `static QApplication`을 따로 만들어 (1) 한 프로세스에
// QApplication이 여러 개 생기고 (2) 프로그램 종료 시 static 소멸자에서 QApplication이
// 파괴되며 Windows에서 SegFault가 났다(테스트는 통과 후 종료에서 크래시).
//
// 해결: qApp이 이미 있으면 그걸 재사용하고, 없으면 1개만 생성해 **의도적으로 누수**한다
// (소멸자 미실행 → 종료 teardown 크래시 회피). 메모리는 프로세스 종료 시 OS가 회수.
namespace nvtest {
inline QApplication& app() {
    if (qApp) {
        return *qApp;
    }
    static int argc = 1;
    static char arg0[] = "nv_ui_tests";
    static char* argv[] = {arg0, nullptr};
    return *new QApplication(argc, argv);  // leaked by design
}
} // namespace nvtest
