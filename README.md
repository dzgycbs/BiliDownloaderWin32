# B 站视频下载器（Win32 原生版）

一个不依赖 Qt / 第三方库的 C++ 版 B 站视频下载工具，界面全部使用 Windows 原生控件（Win32 + Common Controls），
单文件 exe 可直接拷走使用。

> 参考了 B23Downloader 的接口思路，代码全部重写，只依赖 Windows 系统库（WinHTTP / comctl32 / shell32）。

## 功能

| 能力 | 说明 |
|---|---|
| 链接解析 | BV 号、av 号、`bilibili.com/video/...` 链接（支持 `?p=N`）、`ss`/`ep` 番剧号、`b23.tv` 短链、`md` 番剧详情页 |
| 分 P / 剧集 | 列表展示全部分 P 或剧集，可勾选批量下载 |
| 画质选择 | 解析后自动列出可用画质（360P ~ 1080P+），默认选最高 |
| **多任务并发** | 勾选的每一项是一个独立任务，按「并发」数（1/2/3/4/6/8，默认 3）同时下载；每项独立显示进度，底部汇总总进度 |
| **扫码登录** | 点「扫码登录」弹出二维码，手机哔哩哔哩 App 扫码确认后自动写入 Cookie，无需手抄 SESSDATA |
| **wbi 签名** | playurl 走 `x/player/wbi/playurl` 并带 `w_rid` 签名（自动取 nav 密钥、30 分钟缓存），失败自动回落老接口 |
| 下载 | mp4 直链下载；多段自动拼接；DASH 音视频分离流自动用 ffmpeg 合并 |
| 断点续传 | 中途失败可重试，从 `.part` 文件继续；若残留文件与本次资源不匹配（HTTP 416）自动丢弃重下 |
| 登录态 | 扫码或手填 `SESSDATA` 后可下 1080P / 大会员画质 |
| 配置记忆 | Cookie、保存目录、并发数存在 exe 同目录的 `config.ini` |

## 编译

方式一（推荐）：用 VS2022 打开 `BiliDownloader.sln`，选择 `Release | x64` 直接生成。

方式二：开 **x64 Native Tools Command Prompt for VS 2022**，在本目录执行：

```
build.bat
```

产物：`build\BiliDownloader.exe`（静态链接 CRT，无需 VC 运行库）。

## 使用

1. 粘贴链接或直接输入 `BV1xxxxxxxxx` / `av123` / `ss123` / `ep123`，回车或点「解析」。
2. 列表里勾选要下的分 P/剧集，选画质、并发数、保存目录。
3. 点「开始下载」。想中途放弃点「停止」，已排队的任务会一并取消。

### 登录（推荐扫码）

点第二行的「扫码登录」→ 手机哔哩哔哩 App 扫屏幕上的二维码 → 手机上点确认，程序自动填入 Cookie 并存进
`config.ini`，之后重新「解析」就能看到会员画质。

也可以手动填：浏览器登录 B 站 → F12 → Application → Cookies → `https://www.bilibili.com`，复制
`SESSDATA` 的值粘到 Cookie 输入框（只填值或填 `SESSDATA=xxx` 都可以）。

### 关于 4K / HDR / 1080P+

这类画质 B 站只提供 DASH 分离流（视频轨 + 音频轨）。程序会分别下载两路，然后调用
`ffmpeg.exe -c copy` 合并。把 `ffmpeg.exe` 放到 exe 同目录即可自动合并；没有 ffmpeg 时会
分别保存 `xxx.video.mp4` 和 `xxx.audio.m4a`。

## 代码结构

```
src/
  json.h/.cpp      迷你 JSON 解析器（递归下降，约 300 行，无第三方依赖）
  http.h/.cpp      WinHTTP 封装：GET/POST、手动重定向、Cookie、Set-Cookie、断点续传下载
  md5.h/.cpp       MD5（wbi 签名用，RFC 1321 精简实现）
  wbi.h/.cpp       wbi 签名：nav 取 img_key/sub_key → 混淆出 mixin_key → md5(query+mixin)
  bili.h/.cpp      B 站接口层：URL 解析、view / pgc season / wbi playurl（带回落）
  qr.h/.cpp        二维码编码器：字节模式 + 纠错 M + 版本 1~10，含 RS 纠错与掩码罚分
  login.h/.cpp     扫码登录窗口：qrcode/generate + qrcode/poll，二维码用 GDI 现场绘制
  queue.h/.cpp     多任务并发队列：线程池、可追加、可整体取消、状态/字节统计
  download.h/.cpp  下载调度：单段、多段拼接、DASH 合并、文件名清洗
  main.cpp         Win32 界面：控件、布局、定时刷新、消息回传
test/
  test.cpp         接口自测：解析 + playurl
  selftest.cpp     md5 / wbi / 队列并发 / 取消 自测（命令行）
  qr_test.cpp      二维码编码器输出矩阵，配合 qr_check.py 与 Python qrcode 库逐模块比对
  shot.py          窗口截图工具（调试用）
```

线程模型：
- 解析在临时线程里跑，结果用 `PostMessage` 回 UI 线程。
- 下载走 `DownloadQueue` 的线程池（并发数可调）；每个任务自带一份 `MediaInfo`，互不干扰。
- UI 不做阻塞调用，用 120ms 定时器轮询队列状态刷新进度条/状态栏/每一行，避免每块数据都跨线程发消息。

## 自测结果

| 项目 | 方法 | 结果 |
|---|---|---|
| MD5 | 与 Python `hashlib` 比对 16 组（含 55/56/63/64 字节填充边界） | 全等 |
| wbi 签名 | 生成的 `w_rid` 直接请求 `x/player/wbi/playurl` | 服务端 code=0，正常返回 durl |
| 二维码 | 与 Python `qrcode` 库逐个版本、逐个掩码比对矩阵；截图再用 OpenCV 解码 | V1~V10 矩阵完全一致；截图解码得到正确登录 URL |
| 并发 | 同一视频 1 任务 11.2s，3 任务并发 11.0s（10.5MB × 3） | 3 路并行，全部成功 |
| 取消 | 3 任务下载中取消 | 3 项全部「已取消」，不残留「排队中」 |
| 断点续传 | 720P 中断留 `.part` → 换 360P 重下 | 触发 416 后自动丢弃重下，4/4 成功 |

## 已知限制

- 不支持直播流录制（原项目的 FLV 直播录制未实现）。
- 不支持互动视频、漫画。
- 付费/大会员内容取决于账号权限，未购买时接口只返回试看片段。
- wbi 密钥按 `BiliApi` 实例缓存（每个任务各自取一次 nav），并发任务会多几个 nav 请求；
  若后续 nav 加验证，需要改成全局共享密钥。
- 接口为 B 站公开 web 接口，官方调整后仍可能失效，届时优先检查 `wbi.cpp` 的混淆表与 `bili.cpp` 的接口路径。
