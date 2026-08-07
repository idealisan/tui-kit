# termrenderer 方案设计文档

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 编写日期 | 2026-08-07 |
| 项目状态 | 已实现，Linux 验证通过 |

---

## 1. 背景与目标

### 1.1 问题描述

终端模拟器（terminal emulator）通常与 GUI 绑定。很多场景（CI 流水线、文档截图、
远程调试、教学演示）需要在**无显示环境**下把一个命令在真实终端中执行，并把终端
最终画面导出为位图（PNG）。

常见做法（如 VHS、ttyrec 回放）依赖 Chromium 无头浏览器来"渲染"终端，重量级且
难以定制。本项目的目标是：**不依赖任何浏览器或 GUI，从零实现一个可嵌入、可裁剪、
跨平台的终端渲染管线**。

### 1.2 目标

1. 在真实伪终端（PTY / ConPTY）中运行任意命令。
2. 用 libvterm 解析命令输出中的 VT escape sequence，得到精确的"屏幕单元格"状态。
3. 自研 bitmap renderer，用 FreeType 将每个单元格字形栅格化到 RGBA 像素缓冲。
4. 用 libpng 编码输出 PNG 图片。
5. 无显示、无 GUI、无浏览器，适合 CI/headless。
6. Linux、macOS 全支持，Windows（Win10 1809+，ConPTY）尽力支持。
7. 尽量静态链接，产出单文件、无运行时依赖的可执行程序。

### 1.3 非目标

- 不做交互式终端 / 真实输入回显（当前只捕获输出）。
- 不做 WebSocket / HTTP 暴露。
- 不做动画 GIF / WebM（VHS 的领域）；本项目的渲染管线可扩展实现。
- 不做 ligature（连字）合成。

---

## 2. 总体架构

### 2.1 分层图

```
                 ┌─────────────┐
                 │  TUI app    │       用户命令，如 curl www.google.com
                 │  (shell等)  │
                 └──────┬──────┘
                        │
                       PTY        platform layer: forkpty/openpty (POSIX)
                        │         或 ConPTY (Windows)
                 ┌──────▼──────┐
                 │  libvterm   │   纯 C 终端状态机，解析 VT/xterm 序列
                 └──────┬──────┘
                        │
                 screen buffer     单元格级状态：字符/前景/背景/属性
                        │
             ┌──────────▼──────────┐
             │ bitmap renderer     │ 自研 blitter + FreeType 字形栅格化
             │ (canvas + glyph)    │
             └──────────┬──────────┘
                        │
                       RGBA        0xAARRGGBB 内存布局
                        │
             ┌──────────▼──────────┐
             │      libpng         │ 编码 PNG
             └──────────┬──────────┘
                        │
                       PNG
```

### 2.2 模块划分

| 模块 | 文件 | 职责 |
|---|---|---|
| 入口 / 编排 | `src/main.c` | CLI 解析、初始化顺序、流程串联、资源回收 |
| 平台抽象接口 | `src/platform.h` | 跨平台类型与函数签名 |
| POSIX 平台层 | `src/platform_posix.c` | openpty/fork/exec、select 事件循环、字体发现 |
| Windows 平台层 | `src/platform_windows.c` | ConPTY 创建、命名管道、进程管理 |
| 渲染器 | `src/render.c` | Canvas + FreeType 栅格化 + 单元格绘制 |
| PNG 编码 | `src/png.c` | RGBA 缓冲 → PNG 文件 |
| 公共定义 | `src/termrenderer.h` | 尺寸、常量、跨模块函数声明 |

### 2.3 数据流

```
argv → main() 解析选项
   → tr_font_path() 发现字体（可 --font 覆盖）
   → vterm_new(rows, cols)
   → vterm_set_utf8(vt, 1)                  [必须先于 screen 创建]
   → vterm_obtain_screen(vt)                [必须在喂数据前]
   → vterm_screen_reset(screen, 1)          [必须：初始化编码表]
   → tr_proc_spawn(cmd, rows, cols)
   → tr_proc_drain(..., feed=feed_vterm)    [read→vterm_input_write 循环]
   → tr_proc_close()
   → render_screen(vt, size, font, font_px) → RGBA
   → png_write(path, rgba, w, h)
```

---

## 3. 关键技术设计

### 3.1 libvterm 集成

libvterm 是 neovim 孵化的纯 C 终端模拟核心（MIT），提供两层 API：

- **State 层**（`vterm_obtain_state`）：底层状态机，追踪光标、滚动、调色板等。
- **Screen 层**（`vterm_obtain_screen`）：基于 state 的高层封装，维护可查询的
  单元格缓冲 `VTermScreenCell[]`。

#### 3.1.1 必须的初始化顺序（三个关键 Bug 的教训）

经过真实调试，libvterm 的初始化对顺序极其敏感：

```c
VTerm *vt = vterm_new(rows, cols);
vterm_set_utf8(vt, 1);              /* ① UTF-8 必须先于 state 创建      */
VTermScreen *s = vterm_obtain_screen(vt);  /* ② screen 必须先于喂数据  */
vterm_screen_reset(s, 1);           /* ③ reset 必须执行一次             */
```

原因分析：

1. **`vterm_set_utf8` 必须先调用**：libvterm 内部 `vterm_build()` 使用 `malloc`
   而非 `calloc` 分配 `VTerm`，`vt->mode.utf8` 是**未初始化**的随机位。state 创建
   时用它选择初始编码（UTF-8 vs ASCII），随机值会导致编码表选错。
2. **`vterm_obtain_screen` 必须先于输入**：screen 层通过 state 回调同步缓冲。
   若先 `vterm_input_write` 再 obtain screen，缓冲区为空（后续刷新不追溯历史）。
3. **`vterm_screen_reset` 必须执行**：`state->encoding[0..3]` 表在
   `vterm_state_reset()`（由 `vterm_screen_reset` 触发）中初始化。不 reset 则
   `state->encoding[i].enc == NULL`，首个文本字节解码即段错误。

> 这正是官方 `bin/unterm.c` 的初始化模式：`vterm_new → set_utf8 →
> obtain_screen → screen_reset`。

#### 3.1.2 颜色转换

`VTermColor` 是 tagged union：
- `VTERM_COLOR_IS_RGB` → `.rgb.red/green/blue`
- `VTERM_COLOR_IS_INDEXED` → `.indexed.idx`（调色板索引）
- `VTERM_COLOR_IS_DEFAULT_FG/BG` → 默认色

取色必须经 `vterm_screen_convert_color_to_rgb()` 归一化后再读 `.rgb` 字段
（render.c:34-40）。

#### 3.1.3 单元格结构

```c
typedef struct {
  uint32_t chars[VTERM_MAX_CHARS_PER_CELL]; /* 组合字符最多 6 个 */
  char width;        /* 宽字符 (CJK/emoji) 占 2 列 */
  VTermScreenCellAttrs attrs;  /* bold/underline/italic/blink/reverse/strike */
  VTermColor fg, bg;
} VTermScreenCell;
```

### 3.2 平台抽象层设计

#### 3.2.1 接口

`platform.h` 定义最小而完备的接口：

```c
typedef struct { int fd; void *proc; void *platform; } TrProc;

int  tr_proc_spawn(TrProc *out, const char *cmd, int rows, int cols);
int  tr_proc_read(TrProc *proc, char *buf, int len);
int  tr_proc_drain(TrProc *proc, void *vt_ctx, int timeout_ms,
                   void (*feed)(void*, const char*, int));
void tr_proc_close(TrProc *proc);
int  tr_font_path(char *buf, size_t buflen);
```

`tr_proc_drain` 采用**回调注入**（`feed`），让平台层完全不知道 libvterm 的存在，
实现严格分层、便于单测。

#### 3.2.2 POSIX 实现（Linux / macOS）

- `openpty()` 打开主从伪终端，`ioctl(TIOCSWINSZ)` 设置行列数。
- `fork()` + `setsid()` + `ioctl(TIOCSCTTY)` 让子进程获得控制终端。
- `dup2()` 将 slave 复制到 0/1/2，`execl("/bin/sh", "sh", "-c", cmd)` 执行。
- 主循环 `select()` 监听 master fd；`waitpid(WNOHANG)` 检测子进程退出；
  子进程退出后仍有 100ms 宽限期排空残余输出。
- 读侧对 `EIO`（slave 关闭）视作 EOF。

#### 3.2.3 Windows 实现（ConPTY，Win10 1809+）

- `CreatePseudoConsole(size, hInput, hOutput, 0, &pc)` 创建伪控制台。
- 两条命名管道：`pipeIn`（应用写 stdin → hPipeIn）与 `pipeOut`
  （应用读 stdout ← hPipeOut）。
- `STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` 把子进程挂到伪控制台。
- `cmd.exe /c <cmd>` 作为 shell；`WaitForSingleObject` 检测退出。

> 注：Windows 交叉编译需要额外准备 FreeType/libpng/zlib 的 MinGW 版本，
> 详见开发者手册第 5.4 节。

### 3.3 渲染器设计

#### 3.3.1 Canvas

RGBA 缓冲以 `0xAARRGGBB` 打包（原生字节序），配合自研 alpha blending：

```c
typedef struct {
    uint32_t *pixels;   /* 0xAARRGGBB */
    int width, height;
    FT_Face face;
    int font_height, font_width, ascender, baseline;
} Canvas;
```

几何关系（font_px = 18 时）：
- `font_width = max_advance / 64`（DejaVu Sans Mono ≈ 11px）
- `font_height = font_px + CELL_PADDING(2)`
- `baseline = font_height - 1 - 1`，保证字形下探与单元线对齐。

#### 3.3.2 字形栅格化

`FT_Load_Char(ch, FT_LOAD_RENDER)` 渲染字形位图，两种像素模式：
- `FT_PIXEL_MODE_MONO`：按位提取覆盖率。
- `FT_PIXEL_MODE_GRAY`：直接取灰度作为 alpha。

覆盖率为 alpha，`blend()` 与背景合成，支持抗锯齿。

#### 3.3.3 单元格绘制

- 背景：颜色 ≠ 默认色才整格填充（避免无用写）。
- 前景：字形水平居中；粗体通过 **x+1 二次描边** 模拟。
- 下划线：`baseline+1` 画水平线；删除线：`baseline-4`。
- 宽字符（`cell.width == 2`）：字形宽度按两列计算，FreeType 按宽字形自然输出。

#### 3.3.4 复杂度分析

设屏幕 `R×C`、每个字形平均 `G²` 像素：`O(R·C·G²)`。
- 80×24、18px：约 80×24×110 ≈ 21 万像素操作，微秒级完成。
- 缓冲内存：`R·C·font_height·font_width·4` 字节。

### 3.4 PNG 编码设计

- libpng `png_create_write_struct + png_set_IHDR`（8bit RGBA，无交错）。
- **逐行独立分配行缓冲**（关键修复）：早期版本所有行共享同一 `row` 缓冲，
  导致 libpng 读到的是"最后一行"内容，整图近乎全黑。
- 字节序转换：`0xAARRGGBB`（原生序）→ 网络序 RGBA 字节流。

### 3.5 静态链接策略

| 依赖 | 许可 | 链接方式 |
|---|---|---|
| libvterm | MIT | 预编译 `.a` 内置 `lib/` |
| FreeType | FTL | `pkg-config --static` |
| libpng | libpng-2.0 | `pkg-config --static` |
| zlib / bz2 / brotli | zlib/BSD | 静态（FreeType 传递依赖） |

`make STATIC=1` 在链接期加 `-static`，产出 `not a dynamic executable`
的单文件二进制（aarch64 实测 2.2MB）。

---

## 4. 目录结构

```
termrenderer/
├── Makefile              平台自适应 + STATIC=1 静态构建
├── build.sh              一键构建（自动判平台、回退动态）
├── README.md             项目总览
├── .gitignore
├── docs/                 本文档、开发者手册、用户手册
├── include/              内置头文件 (vterm.h, vterm_keycodes.h)
├── lib/                  预编译静态库 (libvterm.a)
└── src/
    ├── main.c
    ├── termrenderer.h
    ├── platform.h
    ├── platform_posix.c
    ├── platform_windows.c
    ├── render.c
    └── png.c
```

---

## 5. 安全与健壮性

### 5.1 内存安全

- 所有 malloc 均有 NULL 检查与失败路径清理。
- 像素坐标均做边界钳制（`canvas_set_pixel`、`draw_glyph` 内部越界丢弃）。
- 子进程失败路径关闭所有 fd，避免泄漏。

### 5.2 超时与僵尸进程

- `tr_proc_drain` 以绝对 deadline 计算剩余等待，避免 `select` 超时漂移。
- 子进程退出后 `waitpid` 回收，无僵尸。
- 超时返回 -1，主程序照常渲染"当前画面"并提示，不崩溃。

### 5.3 命令注入

命令经由 `/bin/sh -c`（POSIX）/ `cmd.exe /c`（Windows）执行。与常规 shell 行为
一致：参数会经历 shell 展开。需要精确控制时由调用方自行引用。

### 5.4 错误处理矩阵

| 场景 | 行为 |
|---|---|
| 字体未找到 | 报错并提示 `--font` |
| PTY 创建失败 | 报错退出 |
| 子进程超时 | 提示 + 渲染当前画面 |
| FreeType 初始化失败 | 返回 NULL，主程序报错退出 |
| PNG 写入失败 | 报错并清理资源 |

---

## 6. 已知限制与后续规划

| 项 | 现状 | 后续 |
|---|---|---|
| ligature 连字 | 不支持 | 接 harfbuzz shaping |
| 动画输出 | 仅静态 PNG | 多帧 → GIF/WebM（扩展 renderer 输出帧序列） |
| Windows 构建 | 源码就绪，工具链未在本机验证 | 补齐 MinGW 依赖文档与 CI |
| 调色板/256色 | libvterm 已解析 | 已通过 convert_color_to_rgb 支持 |
| 光标闪烁/块光标 | 未绘制 | 可在 renderer 加 cursor cell 标记 |
| 性能 | O(R·C·G²) 微秒级 | 可加字形缓存（glyph cache） |

---

## 7. 验收标准

- [x] 无显示环境（本 LXC 容器）中，命令在真实 PTY 执行。
- [x] libvterm 正确解析，屏幕缓冲内容与 `vterm_screen_get_cell` 一致。
- [x] 渲染 PNG 内文字内容可被程序化检测（非全黑）。
- [x] `make STATIC=1` 产出静态链接单文件。
- [x] 初始化顺序错误（缺 set_utf8 / 缺 screen_reset）已在文档与注释中固化。
