#!/usr/bin/env bash
# 文件范围来自 Git，各语言的配置负责排版与第三方排除规则。
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=(-i)
SWIFT_MODE=(--cache ignore --quiet)
PATHS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check) MODE=(--dry-run --Werror); SWIFT_MODE+=(--lint) ;;
        --help|-h)
            echo "用法：bash scripts/format.sh [--check] [--] [文件或目录 ...]"
            exit 0
            ;;
        --)
            shift
            PATHS+=("$@")
            break
            ;;
        -*) echo "未知选项：$1" >&2; exit 2 ;;
        *) PATHS+=("$1") ;;
    esac
    shift
done

if [ "${#PATHS[@]}" -eq 0 ]; then
    PATHS=('*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' '*.swift')
fi

FILES=()
SWIFT_FILES=()
while IFS= read -r -d '' file; do
    [ -f "$file" ] && [ ! -L "$file" ] || continue
    case "$file" in
        *.c|*.h|*.cc|*.cpp|*.hpp|*.m|*.mm) FILES+=("./$file") ;;
        *.swift) SWIFT_FILES+=("./$file") ;;
    esac
done < <(git ls-files --cached --others --exclude-standard --deduplicate -z -- "${PATHS[@]}")

if [ "${#FILES[@]}" -eq 0 ] && [ "${#SWIFT_FILES[@]}" -eq 0 ]; then
    echo "指定范围内没有 C、C++、Objective-C 或 Swift 源文件。" >&2
    exit 2
fi

# 混合提交先检查所需工具，防止执行一部分后才发现另一种工具缺失。
if [ "${#FILES[@]}" -gt 0 ] && ! command -v clang-format >/dev/null 2>&1; then
    echo "缺少 clang-format；macOS 可运行 brew install clang-format。" >&2
    exit 1
fi
if [ "${#SWIFT_FILES[@]}" -gt 0 ] && ! command -v swiftformat >/dev/null 2>&1; then
    echo "缺少 swiftformat；macOS 可运行 brew install swiftformat。" >&2
    exit 1
fi

if [ "${#FILES[@]}" -gt 0 ]; then
    clang-format --style=file --fail-on-incomplete-format "${MODE[@]}" "${FILES[@]}"
fi
if [ "${#SWIFT_FILES[@]}" -gt 0 ]; then
    swiftformat "${SWIFT_FILES[@]}" --config .swiftformat "${SWIFT_MODE[@]}"
fi
