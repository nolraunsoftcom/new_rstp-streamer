#ifdef _WIN32

#include "src/infra/relay/WindowsRelayService.h"

// Windows 헤더 (최소화)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdio>
#include <sstream>
#include <string>

namespace nv::infra {

namespace {

// popen/_popen으로 명령 실행 후 stdout 캡처 + 종료 코드 반환.
int runCapture(const std::string& cmd, std::string* out) {
    if (out) out->clear();
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return -1;
    if (out) {
        std::array<char, 512> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            *out += buf.data();
        }
    }
    return _pclose(pipe);
}

// 종료 코드만 반환 (stdout 버림).
int runSilent(const std::string& cmd) {
    return runCapture(cmd + " >NUL 2>&1", nullptr);
}

// mediamtx.exe를 실행파일(new_viewer.exe) 폴더의 절대경로로 해석한다.
// 상대 이름("mediamtx.exe")을 그대로 sc create binPath에 넣으면 SCM이 앱 폴더가 아닌
// System32 기준으로 찾아 실패한다. 번들된 mediamtx를 절대경로로 가리켜야 한다.
// 앱 폴더에 없으면 원래 값(PATH 폴백) 유지.
std::string resolveExeDir(const std::string& exe) {
    if (exe.find('\\') != std::string::npos || exe.find('/') != std::string::npos)
        return exe;   // 이미 경로 포함
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return exe;
    std::wstring path(buf, n);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return exe;
    const std::wstring candidate = path.substr(0, slash + 1) + L"mediamtx.exe";
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        return exe;   // 앱 폴더에 없음 → PATH 폴백
    const int len = WideCharToMultiByte(CP_UTF8, 0, candidate.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return exe;
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, candidate.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

WindowsRelayService::WindowsRelayService(std::string mediamtxExe, std::string serviceName)
    : m_exe(resolveExeDir(mediamtxExe)), m_svcName(std::move(serviceName)) {}

int WindowsRelayService::runSc(const std::string& args, std::string* out) const {
    return runCapture("sc " + args, out);
}

bool WindowsRelayService::startViaSchtasks(const std::string& configPath, std::string& detail) {
    // 관리자 권한 없음 → OnLogon schtasks 폴백.
    // 사용자 세션 로그온마다 자동 기동. M5 인스톨러가 SCM 서비스로 교체할 예정.
    const std::string tr = "\\\"" + m_exe + "\\\" \\\"" + configPath + "\\\"";
    // 기존 태스크 삭제 (무시)
    runSilent("schtasks /delete /tn \"" + m_svcName + "\" /f");
    // 새 태스크 생성
    int rc = runSilent(
        "schtasks /create /tn \"" + m_svcName + "\""
        " /tr \"" + tr + "\""
        " /sc onlogon /f");
    if (rc != 0) {
        detail = "fallback:schtasks create failed rc=" + std::to_string(rc);
        return false;
    }
    // 즉시 실행
    rc = runSilent("schtasks /run /tn \"" + m_svcName + "\"");
    if (rc != 0) {
        detail = "fallback:schtasks run failed rc=" + std::to_string(rc);
        return false;
    }
    detail = "fallback:schtasks";
    return true;
}

bool WindowsRelayService::ensureRunning(const std::string& configPath) {
    // 멱등: 이미 같은 설정으로 실행 중이면 재시작하지 않는다. viewer가 매 기동마다 ensureUp→
    // 이 함수를 호출하는데, 실행 중인 relay를 재등록하면 장비 RTSP 세션이 매번 churn돼
    // 보호막이 무효화된다(1h 스트레스에서 viewer재시작 N회=장비close N회로 발견).
    // SCM 서비스 경로(sc qc)에서 configPath와 m_exe를 확인한다.
    {
        std::string qcOut;
        int qcrc = runSc("qc \"" + m_svcName + "\"", &qcOut);
        if (qcrc == 0 &&
            qcOut.find(configPath) != std::string::npos &&
            qcOut.find(m_exe) != std::string::npos &&
            status().running) {
            return true;   // 이미 동일 설정으로 가동 중 — 재시작 금지
        }
        // schtasks 폴백 경로: schtasks /query /v /tn 으로 TR(Task to Run) 확인
        if (qcrc != 0) {
            std::string stOut;
            int strc = runCapture(
                "schtasks /query /tn \"" + m_svcName + "\" /v /fo list 2>NUL",
                &stOut);
            if (strc == 0 &&
                stOut.find(configPath) != std::string::npos &&
                stOut.find(m_exe) != std::string::npos &&
                status().running) {
                return true;   // schtasks 폴백도 동일 설정으로 가동 중 — 재시작 금지
            }
        }
    }

    // 1) 이미 서비스가 설치됐는지 확인
    std::string queryOut;
    int qrc = runSc("query \"" + m_svcName + "\"", &queryOut);
    const bool installed = (qrc == 0);

    if (!installed) {
        // 2) sc create 시도 (관리자 권한 필요)
        //    binPath에 실행파일+인수를 넣으면 서비스 시작 시 mediamtx configPath로 기동된다.
        const std::string binPath =
            "\\\"" + m_exe + "\\\" \\\"" + configPath + "\\\"";
        int crc = runSc(
            "create \"" + m_svcName + "\""
            " binPath= \"" + binPath + "\""
            " start= auto"
            " DisplayName= \"NewViewer MediaMTX Relay\"",
            nullptr);
        if (crc != 0) {
            // 관리자 권한 없음 — schtasks 폴백
            std::string detail;
            return startViaSchtasks(configPath, detail);
        }
        // sc description 설정 (실패 무시)
        runSc("description \"" + m_svcName + "\" \"NewViewer MediaMTX relay service\"",
              nullptr);
    }

    // 3) 서비스 기동 (이미 기동 중이면 무시)
    int src = runSc("start \"" + m_svcName + "\"", nullptr);
    // 1056 = ERROR_SERVICE_ALREADY_RUNNING → 정상
    (void)src;

    return status().running;
}

nv::app::RelayServiceStatus WindowsRelayService::status() const {
    nv::app::RelayServiceStatus s;

    std::string queryOut;
    int rc = runSc("query \"" + m_svcName + "\"", &queryOut);
    s.installed = (rc == 0);
    s.detail    = queryOut.substr(0, 400);

    if (s.installed) {
        // STATE 행: "STATE              : 4  RUNNING"
        s.running = (queryOut.find("RUNNING") != std::string::npos);
    } else {
        // schtasks 폴백으로 기동된 경우: 프로세스 이름으로 간이 확인
        std::string tasklist;
        runCapture("tasklist /fi \"imagename eq mediamtx.exe\" /fo csv /nh 2>NUL",
                   &tasklist);
        s.running = (tasklist.find("mediamtx.exe") != std::string::npos);
        if (s.running) s.detail = "fallback:schtasks running";
    }

    return s;
}

bool WindowsRelayService::stop() {
    // SCM 서비스 정지
    runSc("stop \"" + m_svcName + "\"", nullptr);
    // SCM 서비스 삭제
    runSc("delete \"" + m_svcName + "\"", nullptr);
    // schtasks 폴백 정리
    runSilent("schtasks /delete /tn \"" + m_svcName + "\" /f");
    // taskkill (남은 프로세스)
    runSilent("taskkill /im mediamtx.exe /f");
    return true;
}

} // namespace nv::infra

#endif // _WIN32
