# termrenderer 用户手册

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 适用对象 | 终端用户、CI 使用者、需要"终端截图"的任何人 |
| 前置知识 | 基本命令行操作 |

termrenderer 是一个命令行工具：它把一条命令放进**真实的伪终端**里运行，
然后把终端**最终画面**输出为一张 PNG 图片。无需图形界面、无需浏览器、
无需安装终端模拟器。

```
$ termrenderer -- curl -sS -I https://www.google.com
wrote out.png (880x480)
```

---

## 1. 快速上手

### 1.1 获取可执行文件

你有三种方式拿到 `termrenderer`：

1. **使用发布包**：解压后直接运行（静态链接，无需安装依赖）。
2. **自己构建**：见《开发者手册》第 3 节（`./build.sh`）。
3. **仓库自带产物**：仓库 `termrenderer/` 目录下已有编译好的二进制。

> 静态构建的二进制不依赖任何系统库，拷到任意同架构 Linux 即可运行。

### 1.2 最小示例

```bash
# 输出普通文本
./termrenderer -- echo "hello world"

# 带颜色/转义的程序
./termrenderer -- printf '\033[31mRed\033[0m \033[1mBold\033[0m\n'

# 网络命令
./termrenderer --timeout 15000 -- curl -sS -I https://www.google.com

# 文件系统命令（含 ls 彩色输出）
./termrenderer -- ls -la --color=always /usr
```

每次运行都会在**当前目录**生成 `out.png`（可通过 `--output` 改路径）。

---

## 2. 命令行选项

```
usage: termrenderer [options] -- command [args...]

  --cols N        终端列数（宽）        默认 80
  --rows N        终端行数（高）        默认 24
  --font PATH     指定 TrueType 字体    默认自动探测
  --timeout MS    等待输出上限（毫秒）  默认 5000
  --output PATH   输出 PNG 路径         默认 out.png
  --fontsize N    字形像素尺寸          默认 18
  --input PATH    向 pty 喂入输入文件（可重复，按序执行）
  --input-delay MS 首份输入前等待时间    默认 300
  --input-gap MS   相邻输入文件之间的间隔  默认 0
  --dump PATH     把屏幕单元格写出为文本（row:col:char:r,g,b）
```

### 2.1 参数分隔符 `--`

`--` 之后的所有内容（直到命令结束）都作为要执行的**命令字符串**，不会再被
termrenderer 解析。即使命令自身带 `--` 或选项，也保持原样传递。

```bash
# 命令是 curl -s -o /dev/null -w '%{http_code}\n' https://example.com
./termrenderer -- curl -s -o /dev/null -w '%{http_code}\n' https://example.com
```

### 2.2 控制终端尺寸

终端尺寸影响画面布局与换行位置。例如生成一张接近 1024×768 的效果图：

```bash
./termrenderer --cols 100 --rows 30 --fontsize 20 \
  --output wide.png -- neofetch
```

各参数对输出像素的影响：

| 选项 | 影响 |
|---|---|
| `--cols` | 输出宽度 ≈ cols × font_width |
| `--rows` | 输出高度 ≈ rows × (fontsize + 2) |
| `--fontsize` | 字形大小；同时影响行列像素 |

### 2.3 超时

`--timeout` 是**整个命令**的等待上限。到期后无论命令是否结束，都会把**当前画面**
渲染输出（并提示 `command did not finish`）。

```bash
# 给慢命令更多时间
./termrenderer --timeout 30000 -- docker ps
```

---

## 3. 输出说明

### 3.1 输出文件

- 默认 `out.png`，RGBA 8-bit PNG。
- 背景默认黑色（终端默认底色），文字为终端前景色。
- 支持：前景/背景色、加粗、下划线、删除线、256 色、宽字符（CJK/emoji）。

### 3.2 标准错误信息

termrenderer 把**进度/错误**写到 stderr，把 PNG 写到文件，因此不会污染 stdout。

```bash
# 重定向错误信息到日志
./termrenderer -- echo hi 2>render.log
```

---

## 4. 典型使用场景

### 4.1 CI / 文档自动截图

```yaml
# GitHub Actions 示例
jobs:
  screenshot:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: ./build.sh
      - run: ./termrenderer --cols 100 --rows 30 \
             --output docs/img/example.png -- ./scripts/demo.sh
      - uses: actions/upload-artifact@v4
        with:
          name: screenshots
          path: docs/img/*.png
```

### 4.2 教程/演示配图

把教学命令的真实输出截图嵌入 Markdown：

```bash
./termrenderer --fontsize 16 --cols 90 --rows 25 \
  --output img/install.png -- bash install-guide.sh
```

### 4.3 日志可视化（彩色程序）

对 `ls --color=always`、`git diff --color=always`、`htop -b`（批处理）等
**输出依赖 TTY 判断颜色的**程序，只有放在真实伪终端里才会输出颜色——
这正是本项目与 `cmd > file` 的本质区别：

```bash
# 直接重定向：没有颜色（因为不是 TTY）
ls --color=always /usr > plain.txt

# 用 termrenderer：有颜色、有完整终端画面
./termrenderer --output colored.png -- ls -la --color=always /usr
```

### 4.4 只保留 stderr 干净输出

```bash
./termrenderer -- cmd 2>/dev/null
```

---

## 5. 故障排查

### 5.1 输出是黑图

1. 是否给了 `--font` 但路径不存在？→ 用默认自动探测即可。
2. 命令是否真的向终端输出？`echo` 一定可以；GUI 程序请确认其有 CLI 输出。
3. 超时是否太短？`--timeout 15000` 试大值。

### 5.2 画面只显示了一半

- 增大 `--rows` / `--cols`（超出终端尺寸的行列被截断是正常行为）。
- 调大 `--fontsize` 可让同样终端尺寸占据更多像素。

### 5.3 命令有颜色，但图片没有

- 确认命令显式要求颜色（如 `ls --color=always`）。
- 确认命令依赖 TTY 而非管道（本项目已提供真实 PTY）。

### 5.4 中文字符显示为方块/缺失

- 系统字体需包含 CJK 字形。改用支持中文的字体：
  ```bash
  ./termrenderer --font /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
    -- printf '你好世界\n'
  ```
  （安装：`sudo apt install fonts-noto-cjk`）

### 5.5 找不到字体

报错 `no monospace font found; use --font PATH` 时，显式指定：
```bash
./termrenderer --font /path/to/YourMono.ttf -- echo hi
```

---

## 6. 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 成功，PNG 已写出 |
| 1 | 参数错误 / 字体缺失 / PTY 创建失败 / 渲染或写文件失败 |
| 2 | 其他运行错误 |

> 注意：命令超时会**照常**渲染并返回 0（画面已产出），但 stderr 会提示超时。

---

## 7. 与相关工具对比

| 工具 | 渲染方式 | 终端真实性 | 依赖 | 适用场景 |
|---|---|---|---|---|
| **termrenderer** | 自研 blitter + FreeType | 真实 PTY/ConPTY | 无（静态） | CI/文档截图、教学 |
| VHS | 无头 Chromium | 真实 ttyd 会话 | Node、浏览器、ffmpeg | 动画 GIF 录制 |
| `script` + 回放 | 纯文本回放 | 半真实 | 无 | 仅文本记录 |
| 手工截图 | GUI 终端 | 真实 | 需显示器 | 交互操作演示 |

---

## 8. 许可与致谢

- 内部使用组件：
  - libvterm（MIT）——终端状态机
  - FreeType（FTL）——字形栅格化
  - libpng（libpng-2.0）——PNG 编码
  - zlib / bzip2 / brotli——压缩与 FreeType 依赖

---

## 9. 附：常用命令速查

```bash
# 基本
./termrenderer -- echo hello
./termrenderer --output pic.png -- date

# 控制画面
./termrenderer --cols 100 --rows 35 --fontsize 22 -- neofetch

# 慢命令
./termrenderer --timeout 60000 -- git log --oneline --color=always -20

# 自定义字体与输出
./termrenderer --font ~/fonts/FiraCode-Regular.ttf --fontsize 16 \
  --output code.png -- curl -sS -I https://www.google.com
```
