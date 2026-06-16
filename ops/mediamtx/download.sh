#!/bin/bash
# MediaMTX 고정 버전 다운로드 (설계: 버전 고정, 업그레이드는 명시적 결정으로만)
set -euo pipefail
VERSION="v1.19.1"   # 설정 문법(rtspTransports)과 호환되는 버전. 구버전(v1.9.3)은 필드명이 달라 거부됨.
DIR="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  PKG="mediamtx_${VERSION}_darwin_arm64.tar.gz" ;;
  Darwin-x86_64) PKG="mediamtx_${VERSION}_darwin_amd64.tar.gz" ;;
  Linux-x86_64)  PKG="mediamtx_${VERSION}_linux_amd64.tar.gz" ;;
  *) echo "unsupported platform"; exit 1 ;;
esac
if [[ -x "$DIR/mediamtx" ]]; then echo "already present: $DIR/mediamtx"; exit 0; fi
curl -fL "https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/${PKG}" \
  | tar -xz -C "$DIR" mediamtx
echo "downloaded: $DIR/mediamtx (${VERSION})"
