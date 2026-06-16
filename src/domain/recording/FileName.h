#pragma once
#include <string>

// 순수 도메인 — Qt/FFmpeg 의존 없음. 녹화/스냅샷 파일명 살균(app·infra 공용).
namespace nv::domain {

// 파일명에 안전하지 않은 문자를 '_'로 치환한다. 빈 이름은 "channel".
// UTF-8 바이트 단위 처리 — 금지 문자는 모두 ASCII이고 멀티바이트 연속바이트(0x80~0xBF)는
// >0x20이라 오검출 없음. app(std::string)·infra(Qt) 양쪽이 같은 규칙을 쓰도록 도메인에 둔다.
inline std::string sanitizeFileName(const std::string& name) {
    std::string s = name.empty() ? "channel" : name;
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || u < 0x20) {
            c = '_';
        }
    }
    return s;
}

} // namespace nv::domain
