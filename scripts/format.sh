#!/usr/bin/env bash
# clang-format 的文件范围来自 Git，第三方与生成物由 .clang-format-ignore 排除。
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=(-i)
PATHS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check) MODE=(--dry-run --Werror) ;;
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

if ! command -v clang-format >/dev/null 2>&1; then
    echo "缺少 clang-format；macOS 可运行 brew install clang-format。" >&2
    exit 1
fi

if [ "${#PATHS[@]}" -eq 0 ]; then
    PATHS=('*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm')
fi

FILES=()
while IFS= read -r -d '' file; do
    case "$file" in
        *.c|*.h|*.cc|*.cpp|*.hpp|*.m|*.mm)
            [ ! -f "$file" ] || FILES+=("$file")
            ;;
    esac
done < <(git ls-files --cached --others --exclude-standard --deduplicate -z -- "${PATHS[@]}")

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "指定范围内没有 C、C++ 或 Objective-C 源文件。" >&2
    exit 2
fi

clang-format --style=file --fail-on-incomplete-format "${MODE[@]}" "${FILES[@]}"
