# termrenderer 开发者手册

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 适用平台 | 开发基线：Ubuntu 24.04 (aarch64/amd64)；兼顾 macOS、Windows |
| 前置知识 | C 语言、Makefile、终端/伪终端概念 |

本手册面向**从零开始**的开发者：以一台干净的 Ubuntu 系统为起点，经过依赖安装、
libvterm 内置库构建、项目编译，最终产出静态二进制并通过测试。也给出 macOS /
Windows 的移植要点。

---

## 1. 环境基线

### 1.1 推荐的干净环境

- Ubuntu 24.04 LTS（18.04+ 均可，24.04 已验证）
- 普通用户 + sudo 权限
- 可访问公网（apt、GitHub、Debian 源）

### 1.2 确认系统状态

```bash
uname -a
cat /etc/os-release        # 确认 Ubuntu 版本
gcc --version              # 确认编译器
make --version
```

> 若 `gcc`/`make` 不存在，见 2.1 节安装。

### 1.3 架构说明

本手册所有命令在 **aarch64 (ARM64)** 与 **x86_64 (AMD64)** 上均可运行；
仅"预编译静态库"需要按架构区分（见 2.3 节）。

---

## 2. 安装编译依赖

### 2.1 基础工具链

```bash
sudo apt update
sudo apt install -y build-essential pkg-config git curl
```

`build-essential` 提供 `gcc`、`make`、`libc-dev`。

### 2.2 运行/链接依赖

项目需要 FreeType（字形）、libpng（PNG）、zlib/bzip2（FreeType 传递依赖）。

```bash
sudo apt install -y \
    libfreetype6-dev \
    libpng-dev \
    zlib1g-dev \
    libbz2-dev \
    libbrotli-dev
```

验证 pkg-config 能发现这些库：

```bash
pkg-config --cflags freetype2   # 应输出 -I/usr/include/freetype2
pkg-config --cflags libpng      # 应输出 -I/usr/include/libpng16
pkg-config --static --libs freetype2 libpng   # 静态链接所需的全部 -l 参数
```

> **为什么需要 `--static` 的库？** 项目支持 `make STATIC=1` 产出无动态依赖的
> 单文件二进制。静态链接需要 `.a` 归档文件。Ubuntu 的 `-dev` 包默认附带 `.a`
> （如 `/usr/lib/.../libfreetype.a`），无需额外安装。

### 2.3 获取项目与内置 libvterm

`lib/libvterm.a` 与 `include/vterm*.h` 已随项目提交（vendored），**通常无需重建**。
如需从源码重建（例如升级版本、换架构），按以下步骤：

```bash
# 下载 libvterm 源码（neovim 维护，MIT）
cd /tmp
git clone --depth 1 https://github.com/neovim/libvterm.git
cd libvterm

# 环境无 libtool 时，直接用 gcc 编译静态库
gcc -O2 -fPIC -Iinclude -I. -c src/*.c
ar rcs libvterm.a *.o

# 将产物复制到项目
cp libvterm.a /path/to/termrenderer/lib/
cp include/vterm.h include/vterm_keycodes.h /path/to/termrenderer/include/
```

> 若系统装有 `libtool`，也可直接 `make` 生成 `libvterm.la`，但上述 gcc 直编
> 更简洁且结果相同。

---

## 3. 编译

### 3.1 一键构建（推荐）

```bash
cd termrenderer
./build.sh
```

脚本逻辑：
1. 检测平台（Linux/Darwin/Windows）。
2. Linux/macOS：优先 `make STATIC=1`；若静态库缺失自动回退动态构建。
3. Windows（MinGW）：以 `OS=windows` 调用 make。

### 3.2 手动构建（动态）

```bash
cd termrenderer
make
```

等价于：

```bash
cc -O2 -Wall -Wextra -Iinclude -Isrc -I/usr/include/freetype2 -I/usr/include/libpng16 \
   -c -o src/main.o src/main.c
# ... 其余 .o 同理
cc -Llib -o termrenderer src/*.o lib/libvterm.a \
   -lfreetype -lpng16 -lutil
```

### 3.3 静态构建

```bash
make clean
make STATIC=1
```

验证静态性：

```bash
file termrenderer
# 期望输出包含 "statically linked"，且
ldd termrenderer   # 期望输出 "not a dynamic executable"
```

### 3.4 清理

```bash
make clean
```

删除所有 `.o` 与产物二进制。

---

## 4. 运行与测试

### 4.1 冒烟测试

```bash
# 输出到 stderr 的构建信息
./termrenderer -- echo hello world
# 期望：wrote out.png (880x480)
```

### 4.2 网络命令渲染

```bash
./termrenderer --cols 100 --rows 30 --timeout 15000 --fontsize 20 \
  --output /tmp/google.png -- curl -sS -I https://www.google.com
```

### 4.3 程序化验证（不依赖人眼）

渲染结果是否真的包含文字，可用下面的 Python 脚本（仅 stdlib）验证 PNG 非空、
存在文字像素区域：

```bash
python3 - <<'EOF'
import zlib, struct

def load_png(path):
    data = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        ln  = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        if typ == b'IHDR':
            w, h = struct.unpack('>II', data[pos+8:pos+16])
        elif typ == b'IDAT':
            idat += data[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp, stride = 4, w*4 + 1
    rows, prev = [], bytearray(w*4)
    for y in range(h):
        ft = raw[y*stride]
        line = bytearray(raw[y*stride+1:y*stride+1+w*4])
        out = bytearray(w*4)
        for i in range(w*4):
            a = out[i-bpp] if i >= bpp else 0
            b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
            if   ft == 0: v = line[i]
            elif ft == 1: v = (line[i]+a) & 255
            elif ft == 2: v = (line[i]+b) & 255
            elif ft == 3: v = (line[i]+((a+b)>>1)) & 255
            elif ft == 4:
                p = a+b-c; pa,pb,pc = abs(p-a),abs(p-b),abs(p-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                v = (line[i]+pr) & 255
            out[i] = v
        rows.append(out); prev = out
    return w, h, rows

w, h, rows = load_png('/tmp/google.png')
minx,miny,maxx,maxy = w,h,-1,-1
for y in range(h):
    for x in range(0, w*4, 4):
        r,g,b = rows[y][x], rows[y][x+1], rows[y][x+2]
        if r or g or b:
            cx = x//4
            minx = min(minx,cx); maxx = max(maxx,cx)
            miny = min(miny,y); maxy = max(maxy,y)
print(f"size {w}x{h}")
print(f"text content bbox: x[{minx}..{maxx}] y[{miny}..{maxy}]")
assert maxx > minx and maxy > miny, "image appears blank"
print("PASS: image contains rendered text")
EOF
```

### 4.4 平台层独立测试（POSIX）

不依赖 libvterm/FreeType 的最小链路测试，验证 PTY 与 drain 逻辑：

```bash
gcc -O0 -g -Iinclude -Isrc -o /tmp/pty_check \
    /dev/stdin src/platform_posix.c -lutil <<'EOF'
#include <stdio.h>
#include <string.h>
#include "platform.h"

static void feed(void *ctx, const char *buf, int len) {
    fwrite(buf, 1, (size_t)len, (FILE*)ctx);
}

int main(void) {
    TrProc p;
    if (tr_proc_spawn(&p, "echo pty-ok", 24, 80) < 0) return 1;
    if (tr_proc_drain(&p, stdout, 5000, feed) < 0) return 2;
    tr_proc_close(&p);
    return 0;
}
EOF
/tmp/pty_check
# 期望输出：pty-ok（带 \r\n）
```

### 4.5 单元级检查清单

| 检查项 | 命令 | 期望 |
|---|---|---|
| 构建成功 | `./build.sh` | `build complete` |
| 静态链接 | `file termrenderer` | `statically linked` |
| 冒烟 | `./termrenderer -- echo hi` | `wrote out.png` |
| 文字内容 | 4.3 节脚本 | `PASS` |
| 字体缺失降级 | `./termrenderer --font /nonexistent.ttf -- echo hi` | 报错退出 |

---

## 5. 跨平台移植指南

### 5.1 平台选择逻辑

`src/platform.h` 通过宏区分：

| 宏 | 条件 | 生效文件 |
|---|---|---|
| `TR_PLATFORM_WINDOWS` | `_WIN32` | `platform_windows.c` |
| `TR_PLATFORM_MACOS` | `__APPLE__` | `platform_posix.c`（macOS 分支） |
| `TR_PLATFORM_LINUX` | 其他 | `platform_posix.c` |

Makefile 用 `OS=posix|windows` 选择源文件与链接库：

```makefile
ifeq ($(OS),windows)
  PLATFORM_SRC = src/platform_windows.c
  EXE = termrenderer.exe
  LIBS = -lws2_32 -luser32
else
  PLATFORM_SRC = src/platform_posix.c
  EXE = termrenderer
  LIBS = -lutil
endif
```

### 5.2 macOS 要点

- 头文件差异：`<pty.h>` → `<util.h>`（`platform_posix.c` 已用 `#if` 处理）。
- 字体路径：已内置 Menlo / Monaco 候选（`platform_posix.c:176-182`）。
- 无需 `-lutil`（macOS 已并入 libSystem），但保留无害。
- 静态库 `.a` 若缺失，`build.sh` 会自动回退动态构建。

### 5.3 Windows 要点（源码层面）

- 需要 **Win10 1809+**（ConPTY 引入）。
- 纯 Windows API，无 POSIX 依赖；`tr_proc_drain` 用 `WaitForSingleObject`。
- 命令经 `cmd.exe /c` 执行。
- 字体候选：Consolas / Courier New / Lucida Console。

### 5.4 Windows 交叉编译（工具链准备）

> 在当前 Ubuntu 上直接交叉编译 Windows 版，需要 MinGW-w64 交叉工具链 +
> Windows 版 FreeType/libpng/zlib。

```bash
# 1. 安装 MinGW-w64 交叉编译器
sudo apt install -y gcc-mingw-w64-x86-64

# 2. 准备 Windows 版依赖（任选其一）
#    a) 在 Windows 上用 MSYS2 安装：
#         pacman -S mingw-w64-x86_64-{gcc,freetype,libpng,zlib}
#         # 然后把 lib/*.a 与头文件拷回项目交叉库目录
#    b) 用 vcpkg 交叉编译后拷贝产物

# 3. 交叉编译
make OS=windows CC=x86_64-w64-mingw32-gcc EXE=termrenderer.exe \
     CFLAGS="-O2 -Wall -Wextra -Iinclude -Isrc -I<mswin-include>"
```

> 注意：Windows 静态链接 ConPTY 无需额外库；但 FreeType/libpng 的 `.a`
> 必须是为 MinGW 构建的（不能直接使用 Linux 的 `.a`）。

### 5.5 新增平台时的步骤

1. 新建 `platform_<os>.c`，实现 `platform.h` 全部 5 个函数。
2. Makefile 增加 `OS` 分支（源文件、EXE、额外链接库）。
3. `build.sh` 增加平台检测分支。
4. `platform.h` 增加对应宏（如已有 `TR_PLATFORM_*`）。

---

## 6. 开发调试技巧

### 6.1 无 gdb 环境的崩溃定位

若系统无 gdb，可临时下载便携版（无需 sudo）：

```bash
cd /tmp
apt-get download gdb libbabeltrace1 libsource-highlight4t64 \
                 libdebuginfod-common libelf1t64 libdw1t64 libdebuginfod1t64
for d in *.deb; do dpkg-deb -x "$d" gdbsrc/; done
LD_LIBRARY_PATH=gdbsrc/usr/lib/aarch64-linux-gnu \
  gdbsrc/usr/bin/gdb -batch -ex run -ex bt --args ./termrenderer ...
```

> aarch64 需替换为对应架构路径；此方法也常用于交叉调试。

### 6.2 检查 PTY 是否真正读到了数据

在 `tr_proc_drain` 的 `feed` 回调里加 `fprintf(stderr, ...)` 即可确认
数据流是否到达 libvterm。

### 6.3 验证 libvterm 屏幕内容

用 `vterm_screen_get_cell` 枚举 24×80 网格，打印非空单元格的码点与颜色，
用于隔离"解析问题"与"渲染问题"。

### 6.4 strace 观察系统调用

```bash
strace -f -e trace=execve,read,write,ioctl,fork -o /tmp/trace.log ./termrenderer -- echo hi
```

---

## 7. 常见问题（FAQ / 排障）

### 7.1 输出图片是全黑的

排查顺序：
1. **png.c 行缓冲**：确认每行 `rows[y]` 是独立分配（早期共享缓冲 bug）。
2. **初始化顺序**：确认 `vterm_set_utf8` → `vterm_obtain_screen` →
   `vterm_screen_reset`。
3. **颜色转换**：确认经 `vterm_screen_convert_color_to_rgb` 后读 `.rgb`。

### 7.2 段错误（SIGSEGV）在 vterm_input_write 内

几乎都是 `state->encoding[]` 未初始化。运行一次 `vterm_screen_reset(screen, 1)`
即可修复。参见设计文档 3.1.1。

### 7.3 子进程超时但确实有输出

- 增大 `--timeout`。
- 检查命令是否在等待输入（如 `cat` 无 stdin 结束）。
- 检查 `feed` 回调是否真的调用 `vterm_input_write`。

### 7.4 `pkg-config: not found`

未安装 pkg-config：`sudo apt install pkg-config`。

### 7.5 `make STATIC=1` 报找不到 `.a`

缺少静态库：安装对应 `-dev` 包，或确认 `pkg-config --static --libs` 能输出。

### 7.6 Windows 版 `CreatePseudoConsole` 失败

- 系统 < Win10 1809：升级或改用 winpty。
- 确认 `kernel32.dll` 导出符号存在（程序运行时动态 GetProcAddress）。

---

## 8. 代码质量约定

- 所有 malloc 必须检查返回值，失败路径完整释放。
- 平台层不得依赖 libvterm 类型（用 `void*` + 回调注入）。
- 新平台文件必须完整实现 `platform.h` 接口，不破坏其余模块。
- 注释保留关键设计决策（尤其初始化顺序、共享缓冲等踩过的坑）。
- 提交前运行：
  ```bash
  make clean && ./build.sh && ./termrenderer -- echo smoke-test
  ```
