#!/usr/bin/env bash
# check-path-encoding — 守「窄路径 = UTF-8」这条库级约定（P0 / 遗留项 W4）。
#
# 为什么需要一个 grep 守门，而不是靠代码评审：
#
# 这条约定在类型上是**看不见**的。`std::string` 既可以装 UTF-8 也可以装 GBK，
# `fs::path(窄串)` 和 `path.string()` 编译得好好的，只是在 Windows 上按系统
# ANSI 代码页编解码。两个方向的错误互相抵消，于是纯 ASCII 路径（= 全部现有
# 测试与大部分部署）一切正常，**问题只在非 ASCII 路径上暴露**，而且形态是
# 「构造抛 std::system_error」或「seam 报 EINVAL」这类离现场很远的症状。
#
# 对照 RocksDB：它把每个路径 API 都塞进 RX_* 宏（RX_CreateFile / RX_FN），
# 于是「绕过转换层」在结构上就写不出来。我们选的是普通函数 + typedef，
# 可读性好得多，代价就是没有那层强制力——这个脚本把它补回来。
#
# 用法：scripts/check-path-encoding.sh [根目录，默认 .]
# 退出码：0 = 干净，1 = 有违规。

set -uo pipefail
root="${1:-.}"
cd "$root" || exit 2

# 允许清单：转换层自身，以及 io 后端（它就是做编码转换的地方）。
ALLOW='include/bitcask/detail/path_utf8.hpp|include/bitcask/detail/file_util.hpp|src/io/win32_file.cpp|src/io/posix_file.cpp'

scan() {  # scan <正则> <说明>
    local pat="$1" msg="$2" hits
    hits=$(grep -rnE "$pat" --include='*.cpp' --include='*.hpp' --include='*.h' \
             src include c_api 2>/dev/null \
           | grep -vE "^($ALLOW):" \
           | grep -vE '^\s*//' \
           | grep -vE ':[0-9]+:\s*//')
    if [[ -n "$hits" ]]; then
        echo "❌ $msg"
        echo "$hits" | sed 's/^/   /'
        echo
        return 1
    fi
    return 0
}

rc=0

scan '(std::)?filesystem::path\s*\([a-z_]' \
     '从窄串构造 fs::path —— Windows 上按 ANSI 代码页解码。改用 bitcask::detail::from_utf8()。' || rc=1

scan '\.string\(\)' \
     'path::string() —— Windows 上按 ANSI 代码页编码。改用 bitcask::detail::to_utf8()。' || rc=1

scan 'std::fopen\s*\(' \
     'std::fopen 按 CRT 的 ANSI 代码页解释窄路径。改用 bitcask::detail::fopen_utf8()。' || rc=1

scan 'std::remove\s*\(\s*[a-z_]' \
     'std::remove 同上。改用 bitcask::detail::remove_utf8() 或 io::remove_file()。' || rc=1

if [[ $rc -eq 0 ]]; then
    echo "✅ 窄路径编码约定：无违规"
else
    echo "详见 include/bitcask/detail/path_utf8.hpp 的文件头注释。"
fi
exit $rc
