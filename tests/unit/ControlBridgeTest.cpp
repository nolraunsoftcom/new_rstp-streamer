#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include "src/app/ChannelSnapshot.h"
#include "src/domain/connection/ConnectionStateMachine.h"
#include "src/domain/health/StreamHealth.h"
#include "src/ui/shell/ControlBridge.h"

// 프로세스 단일 QApplication(공유·누수) — tests/helpers/QtTestApp.h 참고.
#include "tests/helpers/QtTestApp.h"
namespace {
QApplication& getApp() { return nvtest::app(); }
} // namespace

// ────────────────────────────────────────────────────────────────────────────
// A2: ControlBridge::publish()가 snapshotChanged 시그널을 7개 인자로 올바르게
//     직렬화하는지 QSignalSpy로 검증.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("ControlBridge::publish — Streaming 상태 스냅샷 직렬화") {
    (void)getApp();

    nv::ui::ControlBridge bridge;
    QSignalSpy spy(&bridge, &nv::ui::ControlBridge::snapshotChanged);
    REQUIRE(spy.isValid());

    // 스냅샷 구성: Streaming, 재시도 2회, 6단계 모두 Ok, pps=30.0, ms=50, reason=None
    nv::app::ChannelSnapshot snap;
    snap.state   = nv::domain::ConnState::Streaming;
    snap.attempts = 2;
    snap.reason  = nv::domain::DiagnosisReason::None;
    snap.packetsPerSec    = 30.0;
    snap.msSinceLastPacket = 50;
    snap.bitrateKbps      = 4096.0;
    snap.droppedFrames    = 3;
    snap.decodedFrames    = 3156;
    snap.displayedFrames  = 1570;
    snap.readBytesTotal   = 3879731;
    snap.health.markReached(nv::domain::HealthStage::Presenting);  // 전 단계 Ok

    bridge.publish(QStringLiteral("ch1"), snap);

    REQUIRE(spy.count() == 1);
    const QList<QVariant>& args = spy.at(0);
    REQUIRE(args.size() == 12);

    // arg0: channelId
    CHECK(args.at(0).toString() == QStringLiteral("ch1"));
    // arg1: state == "Streaming"
    CHECK(args.at(1).toString() == QStringLiteral("Streaming"));
    // arg2: attempts
    CHECK(args.at(2).toInt() == 2);
    // arg3: stages — 6개 요소, 모두 Ok(1)
    const QList<int> stages = args.at(3).value<QList<int>>();
    REQUIRE(stages.size() == 6);
    for (int i = 0; i < 6; ++i)
        CHECK(stages.at(i) == static_cast<int>(nv::domain::StageState::Ok));
    // arg4: pps
    CHECK(args.at(4).toDouble() == 30.0);
    // arg5: msSinceLastPacket
    CHECK(args.at(5).toLongLong() == 50LL);
    // arg6: reason
    CHECK(args.at(6).toString() == QStringLiteral("None"));
    // arg7: bitrateKbps, arg8: droppedFrames
    CHECK(args.at(7).toDouble() == 4096.0);
    CHECK(args.at(8).toLongLong() == 3LL);
    // arg9: decoded, arg10: displayed, arg11: readBytes
    CHECK(args.at(9).toLongLong() == 3156LL);
    CHECK(args.at(10).toLongLong() == 1570LL);
    CHECK(args.at(11).toLongLong() == 3879731LL);
}

TEST_CASE("ControlBridge::publish — Failed 상태 + reason 직렬화") {
    (void)getApp();

    nv::ui::ControlBridge bridge;
    QSignalSpy spy(&bridge, &nv::ui::ControlBridge::snapshotChanged);
    REQUIRE(spy.isValid());

    // 스냅샷 구성: Failed, reason=NoPackets, PacketFlow 단계 실패
    nv::app::ChannelSnapshot snap;
    snap.state   = nv::domain::ConnState::Failed;
    snap.attempts = 5;
    snap.reason  = nv::domain::DiagnosisReason::NoPackets;
    snap.packetsPerSec    = 0.0;
    snap.msSinceLastPacket = -1;
    snap.health.markReached(nv::domain::HealthStage::RtspSession);
    snap.health.markFailed(nv::domain::HealthStage::PacketFlow,
                           nv::domain::DiagnosisReason::NoPackets);

    bridge.publish(QStringLiteral("ch2"), snap);

    REQUIRE(spy.count() == 1);
    const QList<QVariant>& args = spy.at(0);
    REQUIRE(args.size() == 12);

    CHECK(args.at(0).toString() == QStringLiteral("ch2"));
    CHECK(args.at(1).toString() == QStringLiteral("Failed"));
    CHECK(args.at(2).toInt() == 5);

    const QList<int> stages = args.at(3).value<QList<int>>();
    REQUIRE(stages.size() == 6);
    // DeviceReach(0), RelayIntake(1), RtspSession(2) → Ok
    CHECK(stages.at(0) == static_cast<int>(nv::domain::StageState::Ok));
    CHECK(stages.at(1) == static_cast<int>(nv::domain::StageState::Ok));
    CHECK(stages.at(2) == static_cast<int>(nv::domain::StageState::Ok));
    // PacketFlow(3) → Failed
    CHECK(stages.at(3) == static_cast<int>(nv::domain::StageState::Failed));
    // Decoding(4), Presenting(5) → Unknown(0)
    CHECK(stages.at(4) == static_cast<int>(nv::domain::StageState::Unknown));
    CHECK(stages.at(5) == static_cast<int>(nv::domain::StageState::Unknown));

    CHECK(args.at(4).toDouble() == 0.0);
    CHECK(args.at(5).toLongLong() == -1LL);
    CHECK(args.at(6).toString() == QStringLiteral("NoPackets"));
    // arg7~11: bitrate/dropped/decoded/displayed/readBytes 모두 기본 0
    CHECK(args.at(7).toDouble() == 0.0);
    CHECK(args.at(8).toLongLong() == 0LL);
    CHECK(args.at(9).toLongLong() == 0LL);
    CHECK(args.at(10).toLongLong() == 0LL);
    CHECK(args.at(11).toLongLong() == 0LL);
}
