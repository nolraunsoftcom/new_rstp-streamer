#ifdef _WIN32

#include "src/infra/relay/WindowsRelayService.h"

// Windows 헤더 (최소화)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

namespace nv::infra {

namespace {

// UTF-8(std::string) → 와이드(std::wstring). Windows API는 와이드로 호출해야 한글 경로 안전.
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        w.data(), len);
    return w;
}

// mediamtx.exe를 실행파일(new_viewer.exe) 폴더의 절대경로로 해석한다.
// 상대 이름("mediamtx.exe")이면 앱 폴더의 번들 mediamtx를 절대경로로 가리킨다(없으면 그대로).
std::string resolveExeDir(const std::string& exe) {
    if (exe.find('\\') != std::string::npos || exe.find('/') != std::string::npos)
        return exe;
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return exe;
    std::wstring path(buf, n);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return exe;
    const std::wstring candidate = path.substr(0, slash + 1) + L"mediamtx.exe";
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        return exe;
    const int len = WideCharToMultiByte(CP_UTF8, 0, candidate.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return exe;
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, candidate.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// mediamtx.exe 프로세스가 떠 있는지 (Toolhelp 스냅샷). 멱등 기동/상태 판정용.
bool mediamtxRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"mediamtx.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// 떠 있는 mediamtx.exe를 모두 강제 종료 (stop/self-heal용).
void killMediamtx() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"mediamtx.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

WindowsRelayService::WindowsRelayService(std::string mediamtxExe, std::string serviceName)
    : m_exe(resolveExeDir(mediamtxExe)), m_svcName(std::move(serviceName)) {}

bool WindowsRelayService::ensureRunning(const std::string& configPath) {
    // 멱등: 이미 떠 있으면 재기동하지 않는다(보호막 — viewer 재시작이 장비 세션을 churn하지
    // 않게). config 파일은 호출 전에 갱신되며 mediamtx가 파일 변경을 감지해 핫리로드한다.
    if (mediamtxRunning()) {
        return true;
    }

    // mediamtx.exe <configPath> 를 분리된 자식 프로세스로 기동한다.
    // 기존엔 sc/schtasks를 _popen으로 호출했으나, 콘솔 없는 GUI 앱에서 _popen이 CRT
    // invalid_parameter fast-fail(0xc0000409)로 앱을 죽였다. CreateProcessW로 직접 실행하면:
    //   • _popen 미사용 → 크래시 없음
    //   • 사용자 계정으로 실행 → %LOCALAPPDATA% config 읽기 가능(SCM SYSTEM 계정 문제 해소)
    //   • 와이드 경로 → 한글 경로(영상관리시스템) 안전
    //   • 관리자 권한 불필요
    //   • CREATE_NO_WINDOW → 콘솔 창 안 뜸. 자식은 부모(viewer) 종료 후에도 계속 실행됨.
    const std::wstring exeW = toWide(m_exe);
    const std::wstring cfgW = toWide(configPath);
    std::wstring cmdline = L"\"" + exeW + L"\" \"" + cfgW + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');   // CreateProcessW는 lpCommandLine을 수정할 수 있어 가변 버퍼 필요

    const BOOL ok = CreateProcessW(
        exeW.c_str(),       // lpApplicationName (절대경로)
        cmdBuf.data(),      // lpCommandLine
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);

    if (!ok) {
        std::fprintf(stderr, "[WindowsRelayService] CreateProcessW 실패 (err=%lu) exe=%s\n",
                     GetLastError(), m_exe.c_str());
        return false;
    }
    // 핸들을 닫아도 프로세스는 계속 실행된다(분리). 부모 종료 후에도 mediamtx 유지.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

nv::app::RelayServiceStatus WindowsRelayService::status() const {
    nv::app::RelayServiceStatus s;
    s.running   = mediamtxRunning();
    s.installed = s.running;   // 별도 설치 개념 없음(자식 프로세스 방식)
    s.detail    = s.running ? "mediamtx running (child process)" : "mediamtx not running";
    return s;
}

bool WindowsRelayService::stop() {
    killMediamtx();
    return true;
}

} // namespace nv::infra

#endif // _WIN32
