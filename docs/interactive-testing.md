# termrenderer 交互测试扩展：动机与用法

> 本文档解释 `--input`（可重复 + `--input-gap`）、`--dump` 与 `tr_proc_write`
> 三个扩展的**背景与设计动机**，并给出针对交互式 TUI 程序的实测用法。

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 适用对象 | 需要程序化验证 TUI 渲染效果、或回放按键序列的开发者 |
| 前置知识 | `user-guide.md`、基本终端/ANSI 概念 |

---

## 1. 动机：为什么需要这些扩展

原版 termrenderer 只解决一个问题：**"把一条命令的真实终端画面输出为 PNG"**。
当用它测试交互式 TUI（如 bubbletea 程序）时，会遇到三个缺口：

1. **PNG 无法程序化断言。** 想要验证"分割线亮斑动画的灰阶是否正确"、
   "状态栏文字是否为 `busy`"，仅靠图片需要人眼查看，无法在脚本/CI 中自动比对。
2. **只能喂一份输入，无法编排时序。** 交互式测试往往是"输入一句提示词 →
   等待流式响应完成 → 再按 PgUp 滚动"。原实现只有一个固定延迟后的一次性输入，
   无法表达这种"先喂 A，等 T 秒，再喂 B"的序列。
3. **缺少向 pty 写入的通道。** 平台接口有 `tr_proc_read`/`tr_proc_drain`，
   但没有 `tr_proc_write`，交互输入无从下手。

三个扩展分别对应这三个缺口：`--dump` 提供可断言的单元格级输出，
`--input` 可重复 + `--input-gap` 提供脚本化按键时序，`tr_proc_write` 补上写入通道。

---

## 2. `--dump PATH`：程序化断言渲染结果

### 2.1 设计动机

libvterm 的屏幕层（screen layer）把解析结果先累积在"damage 缓冲"里，只有
`vterm_screen_flush_damage()` 被调用、或 damage 缓冲写满（约 512 条）时，
才会真正落到可读取的 cell 缓冲。对于一次性小输出（例如 `echo hi`）或末帧改动
很小的交互程序，若不主动 flush，`vterm_screen_get_cell()` 读到的是空白——
表现就是"PNG 正常但 `--dump` 全空"。

因此 `--dump` 在读出前先调用一次 `vterm_screen_flush_damage(screen)`，
保证读到的是**当前真实画面**，而不是未落盘的 damage 队列。

### 2.2 输出格式

每行一个非空白单元格：

```
row:col:char:r,g,b
```

- `row`/`col`：0 基的行列号。
- `char`：单元格首字符（UTF-8）。
- `r,g,b`：该单元格**前景色**经 256 色表换算后的 RGB（0-255）。

空白（`0` 或空格）单元格不输出，因而文件天然稀疏。

### 2.3 用法示例

```bash
./termrenderer --cols 90 --rows 26 --font Mono.ttf --timeout 4000 \
  --input prompt.txt --dump screen.txt --output screen.png \
  -- your-tui-app -config test.yaml
```

随后可用脚本断言，例如验证分割线灰阶范围、状态栏文本：

```bash
# 分割线（row=24）出现过的所有颜色
awk -F: '$1==24 {print $4}' screen.txt | sort -u
# 状态栏（row=23）是否显示 busy
grep -a '^23:' screen.txt | grep -a busy
```

### 2.4 注意事项

- 256 色灰阶 232..255 对应的 RGB 由 libvterm 的 `ramp24` 表给出，
  例如 232→(0,0,0)、236→(44,44,44)、240→(88,88,88)、255→(255,255,255)。
  做颜色断言时请对照该表，而非自行推算。

---

## 3. `--input` 可重复 + `--input-gap`：脚本化按键时序

### 3.1 设计动机

交互测试通常需要"按时间点喂按键"。原实现只支持一份输入文件、固定延迟，
无法表达 `提示词 → 等流式完成 → 按 PgUp` 这样的序列。改为：

- `--input PATH` 可**重复**出现；每个文件按命令行顺序依次喂入。
- `--input-delay MS`：喂第一份文件前等待的时间（默认 300）。
- `--input-gap MS`：相邻两份文件之间的间隔（默认 0）。

喂入线程依次打开、写入、关闭每个文件，其间睡眠 `gap` 毫秒，从而把一次运行
编排成一段"脚本"。

### 3.2 关键实践：PTY 的回车是 `\r` 不是 `\n`

在真实伪终端里按 Enter 发送的是 **CR（`0x0d`）**，而非 LF（`0x0a`）。
bubbletea 等框架把 `\r` 识别为 `key.Enter`、把 `\n` 当作普通字符。
因此输入文件中提交输入必须以 `\r` 结尾：

```bash
# 正确：提交输入
printf '你好\r' > prompt.txt

# 错误：只算输入了换行字符，不会触发提交
printf '你好\n' > prompt.txt
```

### 3.3 用法示例：验证流式动画与滚动

```bash
# 输入 1：提交一句提示词；输入 2：流式完成后连续按 PgUp 与上方向键
printf '你好\r' > s1.txt
printf '\x1b[5~\x1b[5~\x1b[5~\x1b[A\x1b[A\x1b[A' > s2.txt

./termrenderer --cols 90 --rows 26 --font Mono.ttf \
  --timeout 14500 \
  --input s1.txt --input s2.txt \
  --input-delay 400 --input-gap 12000 \
  --dump screen.txt --output screen.png \
  -- your-tui-app -config test.yaml
```

要点：

- `--input-gap 12000`：给流式响应留出完成时间，再发滚动按键。
- PgUp/PgDn/方向键的 ANSI 序列分别是 `ESC[5~`、`ESC[6~`、`ESC[A`、`ESC[B`。
- `--timeout` 需要覆盖 `input-delay + 总 gap + 命令实际耗时`，
  否则在按键序列喂完前画面就被截断。

---

## 4. `tr_proc_write`：向 pty 写入的通道

### 4.1 设计动机

平台层原来只有读取侧（`tr_proc_read`）与整段排空（`tr_proc_drain`），
而"喂入按键"需要把字节写进 pty 的 master 端，进而到达子进程的 stdin。
`tr_proc_write` 与 `tr_proc_read` 对称，是 `--input` 功能的地基。

```c
/* src/platform.h */
int tr_proc_write(TrProc *proc, const char *buf, int len);
```

POSIX 实现即对 master fd 的 `write()`；Windows（ConPTY）对应实现在
`platform_windows.c` 中补充。

---

## 5. 组合示例：端到端校验一个 TUI

以 coreloop（bubbletea 应用）为例，完整的"空闲 → busy → 滚动"验证流程：

```bash
# 空闲画面
./termrenderer --timeout 1500 --dump idle.txt --output idle.png \
  -- coreloop -config test.yaml

# busy 画面（慢速 mock 流式回复，中途截取，可看到分割线亮斑）
./termrenderer --timeout 8000 --input prompt.txt --input-delay 500 \
  --dump busy.txt --output busy.png -- coreloop -config test.yaml

# 滚动（提示词提交后等流式完成，再按 PgUp）
printf '\x1b[5~\x1b[5~\x1b[5~\x1b[A\x1b[A\x1b[A' > keys.txt
./termrenderer --timeout 14500 --input prompt.txt --input keys.txt \
  --input-delay 400 --input-gap 12000 \
  --dump scroll.txt --output scroll.png -- coreloop -config test.yaml
```

对每一张 dump 做灰阶/文本断言，即可在 CI 中自动验证渲染与交互行为，
无需人眼查看图片。
