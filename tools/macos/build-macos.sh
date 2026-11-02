#!/bin/bash
# 在 macOS 上构建、测试并打包 OpenCDC（Apple Silicon 或 Intel）。
#
# 用法（在终端中，从任意目录）：
#   bash <源码目录>/tools/macos/build-macos.sh            # 构建 + 单元测试 + 打包 + 真实工程语料库
#   bash <源码目录>/tools/macos/build-macos.sh --quick    # 不运行语料库（需要从 GitHub 下载开源工程）
#
# 需要：Xcode 命令行工具（xcode-select --install）和 Homebrew（https://brew.sh）。
# 缺少的 cmake、ninja、qt、bash 由 Homebrew 安装。
# 日志写到 <源码目录>/build-logs/：完整日志 macos-<时间>.log，摘要 macos-summary.txt。

SRC=$(cd "$(dirname "$0")/../.." && pwd)
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
cd "$SRC" || exit 1
mkdir -p build-logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="build-logs/macos-$STAMP.log"
SUMMARY="build-logs/macos-summary.txt"
exec > >(tee "$LOG") 2>&1

: > "$SUMMARY"
note() { echo "$*" | tee -a "$SUMMARY"; }
step() { echo; echo "======== $* ========"; }
fail() { note "失败: $*"; note "完整日志: $SRC/$LOG"; exit 1; }

note "OpenCDC macOS 构建 $STAMP"
note "源码: $SRC"
step "环境"
sw_vers 2>/dev/null
uname -m
note "系统: $(sw_vers -productVersion 2>/dev/null) $(uname -m), $(sysctl -n hw.ncpu) 核"
if ! xcode-select -p >/dev/null 2>&1; then
    fail "没有 Xcode 命令行工具。请在终端运行 xcode-select --install，安装完成后重新运行本脚本"
fi
clang_major() { clang++ --version 2>/dev/null | head -1 | sed -E 's/.*clang version ([0-9]+).*/\1/'; }
note "编译器: $(clang++ --version | head -1)"
# slang（SystemVerilog 前端）需要 C++20 的 std::views::join 等，Apple clang 16（Xcode 16）起才有
if [ "$(clang_major)" -lt 16 ] 2>/dev/null; then
    note "Apple clang $(clang_major) 太旧，需要 16 或更新（Xcode 16 的命令行工具）。正在查找命令行工具的更新..."
    LABEL=$(softwareupdate --list 2>&1 | sed -n 's/^\* Label: \(Command Line Tools.*\)$/\1/p' | sort -V | tail -1)
    if [ -n "$LABEL" ]; then
        note "找到更新: $LABEL。安装需要管理员密码（输入时不显示字符）："
        sudo softwareupdate -i "$LABEL" --verbose || fail "安装 $LABEL"
        hash -r
    fi
    if [ "$(clang_major)" -lt 16 ] 2>/dev/null; then
        fail "命令行工具仍是 Apple clang $(clang_major)。请打开 系统设置 > 通用 > 软件更新 安装 Command Line Tools；或在终端运行 sudo rm -rf /Library/Developer/CommandLineTools 后运行 xcode-select --install 重新安装，然后重新运行本脚本"
    fi
    note "编译器已更新: $(clang++ --version | head -1)"
fi

step "依赖（Homebrew）"
BREW=$(command -v brew || true)
[ -z "$BREW" ] && [ -x /opt/homebrew/bin/brew ] && BREW=/opt/homebrew/bin/brew
[ -z "$BREW" ] && [ -x /usr/local/bin/brew ] && BREW=/usr/local/bin/brew
if [ -z "$BREW" ]; then
    fail "没有 Homebrew。请按 https://brew.sh 的说明安装，然后重新运行本脚本"
fi
eval "$("$BREW" shellenv)"
for pkg in cmake ninja qt bash; do
    if ! brew list --versions "$pkg" >/dev/null 2>&1; then
        echo "安装 $pkg ..."
        brew install "$pkg" || fail "brew install $pkg"
    fi
done
QT_PREFIX=$(brew --prefix qt)
note "cmake $(cmake --version | head -1 | awk '{print $3}'), ninja $(ninja --version), Qt $(brew list --versions qt | awk '{print $2}') ($QT_PREFIX)"

BUILD="$SRC/build/macos"
# 编译器或最低系统版本变了时，旧的配置缓存不能再用
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=14.0" "$BUILD/CMakeCache.txt"; then
    rm -rf "$BUILD"
fi
COMPILER_ID="$(clang++ --version | head -1)"
if [ -f "$BUILD/CMakeCache.txt" ] && [ "$(cat "$BUILD/.opencdc-compiler" 2>/dev/null)" != "$COMPILER_ID" ]; then
    rm -rf "$BUILD"
fi
mkdir -p "$BUILD" && echo "$COMPILER_ID" > "$BUILD/.opencdc-compiler"
step "配置"
cmake --preset macos -DCMAKE_PREFIX_PATH="$QT_PREFIX" || fail "配置（cmake --preset macos）"

step "构建（Release）"
cmake --build "$BUILD" --config Release -j "$(sysctl -n hw.ncpu)" 2>&1 | tee build-logs/macos-build-$STAMP.log
[ "${PIPESTATUS[0]}" = 0 ] || fail "构建"
WARN=$(grep -c "warning:" build-logs/macos-build-$STAMP.log || true)
note "构建: 通过（编译警告 $WARN 条，见 build-logs/macos-build-$STAMP.log）"

step "单元测试与命令行测试（ctest）"
(cd "$BUILD" && ctest -C Release -j "$(sysctl -n hw.ncpu)" --output-on-failure) 2>&1 | tee build-logs/macos-ctest-$STAMP.log
CTEST_RC=${PIPESTATUS[0]}
note "测试: $(grep -E 'tests passed|tests failed' build-logs/macos-ctest-$STAMP.log | tail -1)"
grep -E "^\s+[0-9]+ - .*\((Failed|Timeout|SEGFAULT|Subprocess|Exception|Not Run)" build-logs/macos-ctest-$STAMP.log | tee -a "$SUMMARY"

step "命令行冒烟测试"
"$BUILD/bin/Release/opencdc" --version | tee -a "$SUMMARY"
"$BUILD/bin/Release/opencdc" analyze examples/smoke/smoke.ocdc.json -q | grep -E "^(Clocks|CDC|Rules)" | tee -a "$SUMMARY"

step "打包（cpack）"
export COPYFILE_DISABLE=1 # no ._ AppleDouble files in the archives
rm -rf "$BUILD/_CPack_Packages"
(cd "$BUILD" && cpack -C Release) 2>&1 | tee build-logs/macos-cpack-$STAMP.log
[ "${PIPESTATUS[0]}" = 0 ] || fail "打包"
mkdir -p dist
for f in "$BUILD"/OpenCDC-*-macos-*.dmg "$BUILD"/OpenCDC-*-macos-*.tar.gz; do
    [ -f "$f" ] && cp "$f" dist/ && note "安装包: dist/$(basename "$f") ($(du -h "$f" | awk '{print $1}'))"
done

CORPUS_RC=0
if [ $QUICK = 0 ]; then
    step "真实工程语料库（从 GitHub 下载固定版本的开源工程）"
    BASH5="$(brew --prefix)/bin/bash"
    "$BASH5" tools/ci/sv-corpus.sh "$BUILD/bin/Release" "$SRC/build/corpus-sv" 2>&1 | tee build-logs/macos-sv-corpus-$STAMP.log
    SV_RC=${PIPESTATUS[0]}
    "$BASH5" tools/ci/vhdl-corpus.sh "$BUILD/bin/Release" "$SRC/build/corpus-vhdl" 2>&1 | tee build-logs/macos-vhdl-corpus-$STAMP.log
    VHDL_RC=${PIPESTATUS[0]}
    note "语料库: SystemVerilog $(grep -c '^ok' build-logs/macos-sv-corpus-$STAMP.log) 项通过（退出码 $SV_RC），VHDL $(grep -c '^ok' build-logs/macos-vhdl-corpus-$STAMP.log) 项通过（退出码 $VHDL_RC）"
    grep "::error" build-logs/macos-*-corpus-$STAMP.log | cut -c1-300 | tee -a "$SUMMARY"
    [ $SV_RC = 0 ] && [ $VHDL_RC = 0 ] || CORPUS_RC=1
fi

echo
if [ "$CTEST_RC" = 0 ] && [ $CORPUS_RC = 0 ]; then
    note "结果: 全部通过"
else
    note "结果: 有失败（见上面和 $SRC/$LOG）"
fi
note "完整日志: $SRC/$LOG"
