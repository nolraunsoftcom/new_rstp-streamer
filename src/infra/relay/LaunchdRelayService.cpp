#ifdef __APPLE__

#include "src/infra/relay/LaunchdRelayService.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>  // getuid()

namespace nv::infra {

namespace {

// $HOME/Library/LaunchAgents/ ディレクトリを返す
std::string launchAgentsDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/Library/LaunchAgents";
}

// コマンドを実行し stdout を文字列として返す。エラー時は空文字列。
std::string runCapture(const std::string& cmd) {
    std::array<char, 512> buf{};
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result += buf.data();
    }
    pclose(pipe);
    return result;
}

// コマンドを実行し終了コードを返す。stdout/stderr は /dev/null へ。
int runSilent(const std::string& cmd) {
    return std::system((cmd + " >/dev/null 2>&1").c_str());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

LaunchdRelayService::LaunchdRelayService(std::string mediamtxBin, std::string label)
    : m_bin(std::move(mediamtxBin)), m_label(std::move(label)) {}

std::string LaunchdRelayService::plistPath() const {
    return launchAgentsDir() + "/" + m_label + ".plist";
}

bool LaunchdRelayService::ensureRunning(const std::string& configPath) {
    const std::string plist = plistPath();
    const std::string dir   = launchAgentsDir();

    // 1) LaunchAgents 디렉토리 생성 (없으면)
    runSilent("mkdir -p \"" + dir + "\"");

    // 2) plist 작성 (덮어쓰기 — configPath가 바뀔 수 있으므로 항상 갱신)
    {
        std::ofstream f(plist);
        if (!f.is_open()) return false;
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"\n"
          << "  \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n"
          << "<dict>\n"
          << "  <key>Label</key>\n"
          << "  <string>" << m_label << "</string>\n"
          << "  <key>ProgramArguments</key>\n"
          << "  <array>\n"
          << "    <string>" << m_bin << "</string>\n"
          << "    <string>" << configPath << "</string>\n"
          << "  </array>\n"
          << "  <key>RunAtLoad</key><true/>\n"
          << "  <key>KeepAlive</key><true/>\n"
          << "  <key>StandardOutPath</key>\n"
          << "  <string>/tmp/" << m_label << ".stdout.log</string>\n"
          << "  <key>StandardErrorPath</key>\n"
          << "  <string>/tmp/" << m_label << ".stderr.log</string>\n"
          << "</dict>\n"
          << "</plist>\n";
    }

    // 3) 기존 서비스 언로드 (stale 정리 — 실패는 무시)
    const uid_t uid = getuid();
    const std::string domain = "gui/" + std::to_string(uid);
    runSilent("launchctl bootout " + domain + "/" + m_label);
    // 언로드가 반영될 때까지 잠깐 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 4) 로드 + 기동
    //    macOS 10.15+ 권장: bootstrap + kickstart
    //    구버전 폴백: load -w
    int rc = runSilent("launchctl bootstrap " + domain + " \"" + plist + "\"");
    if (rc != 0) {
        // 폴백: launchctl load -w
        rc = runSilent("launchctl load -w \"" + plist + "\"");
    }
    if (rc != 0) return false;

    // 5) kickstart — 즉시 기동 보장 (-k: kill & restart if already running)
    runSilent("launchctl kickstart -k " + domain + "/" + m_label);

    // 6) 기동 확인 (최대 2초 대기)
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (status().running) return true;
    }
    return status().running;
}

nv::app::RelayServiceStatus LaunchdRelayService::status() const {
    nv::app::RelayServiceStatus s;
    s.installed = false;
    s.running   = false;

    // installed: plist 파일 존재 여부
    {
        std::ifstream f(plistPath());
        s.installed = f.is_open();
    }

    // running: launchctl print 출력에 "pid" 행이 있으면 기동 중
    const uid_t uid = getuid();
    const std::string domain = "gui/" + std::to_string(uid);
    const std::string out = runCapture(
        "launchctl print " + domain + "/" + m_label + " 2>&1");

    if (!out.empty() && out.find("Could not find") == std::string::npos
        && out.find("No such process") == std::string::npos) {
        // "pid = <number>" 이 있으면 실행 중
        s.running = (out.find("pid = ") != std::string::npos);
        s.detail  = out.substr(0, 200); // 처음 200자만 저장
    }

    return s;
}

bool LaunchdRelayService::stop() {
    const uid_t uid = getuid();
    const std::string domain = "gui/" + std::to_string(uid);

    // bootout — 서비스를 완전히 제거 (KeepAlive 포함)
    runSilent("launchctl bootout " + domain + "/" + m_label);

    // plist 삭제
    const std::string plist = plistPath();
    std::remove(plist.c_str());

    // 잔여 mediamtx 프로세스가 없는지 짧게 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}

} // namespace nv::infra

#endif // __APPLE__
