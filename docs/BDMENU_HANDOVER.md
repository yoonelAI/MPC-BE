# MPC-BE BDMenu 项目移交文档

> 每轮开发后更新本文件（顶部"当前状态"和末尾"开发记录"）。换 agent / 换对话时先读这里。
> 最后更新：2026-11-26 00:45（Asia/Shanghai） 版本 **P1.4（已发布：已打包、已推送 c897005）**　下一步：P2 BD-J
> 11-26 01:30 新对话接手：远端最新提交为 f57fe87（仅交接文档）；P2 BD-J 方案草案见 `docs\P2_BDJ_PLAN.md`（待用户批准）。
> 11-26 01:50 用户批准 P2，从 P2-0 验证开始。发现：deps\bdlibs 的静态 bluray.lib 1.5.1 **已含 BD-J 本地代码**（JNI、Java 9+ --patch-module、JAVA_HOME/LIBBLURAY_CP 查找），只缺 jar 和 JRE。
> 新增：`repo\.github\workflows\bdj.yml`（ubuntu 编 libbluray-j2se/awt jar，JDK17/21 两份，发布到 release `bdj-1.5.1`）、`repo\bdmenu\tools\bdjprobe.c`（BD-J 验证工具）、`tools\bdj_deps.cmd`（下载 jar + Temurin JRE 21/17 到 deps\bdj）、`tools\bdj_probe.cmd`（编译并跑 4 组配置，输出 builds\bdjprobe）；代理新增命令 `bdjdeps`、`probe`（改了 build_agent.cmd，需用户重开代理）。
> 11-26 17:50 **P2-0 通过**（推送 873d5fe；deps\bdj 有 jars17/jars21/jre17/jre21）：**Java 17 下 Odyssey BD-J 菜单完整可用**（片头、主菜单、左右键高亮、PLAY 进正片、弹出菜单、回主菜单）；
>   Java 21 失败（VFS 缓存不按需拷 JAR\00001\*.xml → Xlet 启动失败）→ 交付 Java 17。BD-J 菜单不吃鼠标（mouse_select=0）。详见 docs\P2_BDJ_PLAN.md 顶部"P2-0 结果"、截图 docs\P2_0_java17_shots.png。下一步 P2-1（bdnav.dll API v6）。
> 11-26 18:20 用户新要求（**长期有效**）：
>   1. **阶段性版本不再做补丁包**，改为 **安装版（Inno Setup exe，和原版一样写注册表/卸载项/文件关联）+ 绿色版**。安装版基于 `repo\distrib\mpc-be_setup.iss` 修改：
>      加 bdnav.dll、BDMenu-README、（P2 起）bdj\jre17 + jar；版本名带 BDMenu；本机未装 Inno Setup 6（需 ≥6.5.2，打包前让代理装便携版）；
>      distrib\MPC_components\DirectX\x64 为空，打包时从 builds\local 拷 d3dcompiler_47.dll / d3dx9_43.dll。
>      **用户已定：安装版覆盖原版**（沿用原版 AppId {FE09AF6D-...}、目录 C:\Program Files\MPC-BE x64），但**只在绿色版确认完全正常后**才用来替换用户日常的原版。
>   2. **默认设置**：查看→信息栏 默认关；选项→播放器→界面→显示章节标记 默认开。已改 AppSettings.cpp（ResetSettings fChapterMarker=true；LoadSettings 末尾一次性迁移：
>      设置里无 `BDMenuDefaults` 时 nCS 去掉 CS_INFOBAR、ChapterMarker=1、写 BDMenuDefaults=1 —— 原版设置沿用过来也会生效一次）。18:16 main 编译成功，已部署 builds\local（未推送）。
>      注意：builds\local 无 ini → 用注册表 HKCU\Software\MPC-BE，和已安装的原版共用设置。
> 11-26 18:11 新测试盘（bdjprobe，Java 17，agent\probe_discs.txt 列表模式，日志 builds\bdjprobe\log_<名>.txt，截图 artifacts）：
>   - **可怜的东西 Poor Things**（`E:\AI\MPC-BE-BDMenu\2023.可怜的东西.Poor Things.iso`，50GB，UHD，**BD-J**，56 BD-J + 34 HDMV 标题，已解密）：✅ 片头 → BD-J 语言选择菜单（13 种语言，上下键移动）→ 回车 → 主菜单 PLAY/SCENES/SET UP/EXTRAS，键盘可用。
>     注意：语言菜单是**无限静止画面**（连发 STILL_TIME param=0，读取停止）→ P2-1/2 要处理静止：停读、继续泵 VM、保持最后一帧。
>   - **罗宾汉之死 The Death of Robin Hood**（UHD，**HDMV**）：bdjprobe ✅；18:22 **用户用 builds\local 实测正常、鼠标可点** → 用户已删除该 ISO（非 BD-J 不保留）。
> 11-26 18:25 BD-J 鼠标：查证为普遍情况——PowerDVD 对 BD-J 盘同样不能直接用鼠标（官方说明：BD-J 内容不支持鼠标，需用遥控/方向键面板）；libbluray 会把鼠标作为 AWT 事件转给 BD-J，但光盘程序多数不处理；mpv 用同一 libbluray，同样受限。
>   方案保持：BD-J 下鼠标左键=回车，方向键/回车/弹出菜单为主；P2-2 可加"屏幕方向键面板"（类似 PowerDVD）作为可选增强，待用户定。
> 11-26 18:45 **P2-1 完成**（bdnav.dll API v6，dll + exe 已编译部署 builds\local，builds\local\bdj 已放 JRE17 + jar；已推送）：
>   - dll 新接口：`bdnav_open_ex(path, BDNAV_CONFIG*)`（JAVA_HOME/LIBBLURAY_CP 在 dll 的 CRT 和进程环境里设置；预加载 JRE 自带 VC 运行库（系统没有时）；
>     BD-J 持久存储/缓存 = 数据目录\bdj_storage\persist|cache；启动时清 %TEMP%\libbluray-bdj-cache 下 1 小时前的旧缓存）、
>     ARGB 叠加回调（BD-J 菜单画到同一 IG 平面结构）、`BDNAV_PLANE` 加脏矩形 dx0..dy1（unlock 时清零）+ argb_source、
>     `bdnav_read_ex`（flags BDNAV_READ_POS_JUMP = 读取位置不连续）、`bdnav_get_title_kind`、`bdnav_bdj_started`；`bdnav_set_debug(2)` 加 BD-J 日志。
>   - 播放器：BDNavLib 加载新导出（API 版本 6，exe 和 dll 必须一起换）；CBDNavigator::Open 用 open_ex（exe目录\bdj\jre + bdj\libbluray-j2se-1.5.1.jar）；
>     OnNotify 按脏矩形只复制/转换变化区域（m_bPlaneSynced 不同步时整帧）；ReadTS 用 read_ex。**BD-J 盘仍回退原版**（P2-2 才放开），日志会写 Java/BD-J 是否可用。
>   - 验证：新工具 `repo\bdmenu\tools\bdnavtest.c`（通过 bdnav.dll 跑盘，每帧比对"脏矩形增量副本"与整帧）；代理 `dll` 同时编 bdnavtest.exe 到 builds\bdjprobe；
>     `probe` 在 agent\navtest.txt 存在时用 probe_discs.txt 列表跑 bdnavtest（日志 builds\bdjprobe\navlog_<名>.txt）。
>     结果：Odyssey 131 帧 / Poor Things 62 帧 / KARA(HDMV) 3 帧，**增量副本与整帧 0 差异**；Odyssey 增量只复制 17.9 Mpx（整帧需 271.6）。截图 artifacts p2_1_odyssey_dll_v6.png。
>   - 待用户：用 builds\local 复测 KARA / 倩女幽魂（HDMV 回归），确认 P1.4 功能不变。下一步 P2-2：放开 BD-J、静止/IDLE 泵、黑场、按键。
> 11-26 18:54 用户复测 P2-1：**KARA、倩女幽魂正常**（HDMV 无回归）。
>   - 倩女幽魂"标题菜单"= 正片中的**光盘弹出菜单**（透明悬浮、背景电影继续播，日志 user: pop-up menu -> 1 / MENU 1），"根菜单"= Top Menu，**属正确行为**（以前没注意到）。
>   - **输入法 bug**：播放器在前台时输入法显示英文、点别的窗口变回中文。原因：P1.3（11-25 20:00）为修快捷键在主窗口/视频窗口 ImmAssociateContextEx(NULL) 禁用了 IME。
>     18:57 修复（已编译部署 builds\local，未推送）：不再禁用 IME；PreTranslateMessage 遇 VK_PROCESSKEY（Edit/RichEdit/ComboBox 除外）还原真实按键后
>     ImmNotifyIME(NI_COMPOSITIONSTR, CPS_CANCEL) + NI_CLOSECANDIDATE 取消候选。待用户复测：中文输入法下前台仍显示中文，且字母/方向键/空格/PageUp 等快捷键仍有效、不弹候选框。
> 11-26 19:06 **P2-2 第一版**（已编译部署 builds\local，已推送）：BD-J 盘不再回退，进入导航模式（需 exe目录\bdj\jre + jar，缺失或 bdj_handled=0 仍回退）。
>   - 首次取数据等待：BD-J 盘 25 秒（JVM + Xlet 启动，Odyssey 首个播放列表约 5.9 秒才出数据），HDMV 仍 3 秒；日志 "stream: first data after N ms"。
>   - IDLE / STILL_TIME 事件不再算"有事件"→ 读取线程 Sleep(10)，不再空转。
>   - 菜单变化后 80ms 若还没有一帧画出它（静止画面/空闲，没有新视频帧）→ m_pCAP->Paint(false)（TIMER_BDNAV_REPAINT，IsRenderPending）。
>   - BD-J 标题（TITLE 事件 + bdnav_get_title_kind：BDNAV_TITLE_BDJ/INTERACTIVE）：鼠标悬停转发；点击 = 光盘报告命中时 MOUSE_ACTIVATE，否则 ENTER；
>     Ctrl+M 直接发 POPUP 键（BD-J 不发 POPUP 事件）；自动调菜单也用 POPUP；读取位置无事件跳变 → 新时间锚点。
>   - 菜单显示时新增按键：0-9（含小键盘）、F1-F4 = 红绿黄蓝。
>   - 已知限制：BD-J 只画图形不放任何播放列表的菜单（没有视频）→ 25 秒无数据回退原版（需要时再做黑场视频）；BD-J 弹出菜单里的"缩小视频窗"不支持（视频仍全屏，在图形下面）。
>   - 本轮测试开了详细日志：builds\local\bdnav_debug.txt（**测完删除**），旧日志移到 builds\logs_archive\1126_1907_bdnav.log。
> 11-26 19:19 用户测 P2-2 第一版：**Odyssey** 启动约 5 秒；鼠标点击=确定、方向键可操作；字幕有；PageUp/Down、Ctrl+M（弹出菜单 + 画中画小窗，下面按钮可操作）正常；
>   但 PLAY 后 FBI 警告一闪就进正片。**Poor Things** 片头上的语言菜单（从上到下）→ 闪一下（1611→1355 音频 DTS-HD→LPCM 重建图）→ 换背景的语言菜单（带进度条）可选；
>   选语言后 PLAY **卡住**，按暂停**播放器卡死**（被 blocklist 加入，已手动清除）。KARA 正常。
>   19:25 修复（已编译部署，已推送）：
>   - **卡死根因 = 死锁**：CAsyncRequest::Complete 在 Read() 等数据期间持有流的 m_csLock；读取线程遇用户跳转 flush 时 TruncateUnread 也要 m_csLock →
>     BD-J 静止菜单（无数据、分离器在 Read 里等）+ 回车跳转 = 互等。TruncateUnread 改为只用 m_csPacketsLock。
>   - **FBI 一闪**：用户跳转后 3 秒内的格式变化会"立即重建、丢弃旧数据"，但 Odyssey 在 0.2 秒内就读完了 21 秒警告片（00004+00100）→ 被丢。
>     改为按字节判断：跳转后读了 <1MB 就遇到格式变化才立即重建，否则播完再重建（m_flushTotal）。
>   - END_OF_TITLE 连发几百次：不再当"有事件"空转，日志只记第一条。
>   - BD-J 菜单（短播放列表 <10 分钟 + 菜单可见）及 interactive 标题不显示进度条。
>   - 启动 5 秒分析：JVM 0.3 秒、Xlet 1 秒内就绪，之后光盘程序自己等约 4 秒（读 loading 资源）才放第一个播放列表 → 期间没有视频图，画面空白。
>     → 进**已排期待办**：先建"黑场视频图"让光盘的 loading 画面立刻显示（感知启动 ~1 秒）。

## ⏳ 长期待办（不排期）
- **Java 21 下 BD-J 失败**（用户 11-26 18:22 要求进长期待办）：Odyssey/Java 21 时 libbluray VFS 缓存未按需从 ISO 拷出 `BDMV\JAR\00001\*.xml` → FileNotFoundException → MenuXlet 启动失败；Java 17 正常。
  线索：Java 18+ 文件系统钩子（java.io.BDFileSystem / FileInputStream 补丁）行为变化；复现：`agent\request.txt` 写 probe（删 agent\probe_discs.txt 走 4 组配置）。当前交付 Java 17。

## 📌 已排期待办
- **BD-J 启动加速**（用户 11-26 19:19：Odyssey 启动 5 秒"想办法加速或放入待办"）：JVM 只占 0.3 秒，其余是光盘程序自己的初始化；
  方案：Xlet 一启动就建一个黑场视频图（内置 1080p 黑帧 TS），BD-J loading 画面立刻可见，第一个播放列表来时再重建。可能再试：JVM 预热（播放器启动后空闲时创建）。
- **屏幕方向键面板**（BD-J 菜单用鼠标点方向键/确定，类似 PowerDVD）：用户 11-26 18:29 同意后续排期加上，**动手前先出设计图给用户看**。计划放在 P2-2 之后。

## 1. 目标（用户需求）
- 基于 **MPC-BE 1.9.3**（最新稳定版）改内核，实现**蓝光 ISO / BDMV 先出菜单、鼠标可操作**；参考 mpv 0.42（PR #18080）和 Yaozhil/mpv-Yaozhi。
- 默认开启、**没有配置项**，出错自动回退原版播放；交付**绿色版压缩包**（含全部依赖，将来 BD-J 需要精简 Java）+ **安装版补丁包**。
- 用户决定：开盘直接进正片的盘（弹出式菜单盘）→ 正片开始约 3 秒后自动调出菜单。
- 项目放 `E:\AI`，不放 C 盘。回复用户用中文。


## ⏰ 用户要求提醒的待办（以后做）
- **格式切换改为"不重建、动态切换解码器"**（用户 11-25 23:26 要求以后提醒）：当前 P1.4 遇视频/音频格式变化（H.264↔HEVC、新增音轨/PG）会整条重建播放链路，切换处停顿 1–2 秒、末帧停留。
  目标：保留图，分离器输出 pin 动态换媒体类型（QueryAccept/ReceiveConnection 或 IPinConnection 动态重连），解码器 MPC Video Decoder 支持动态格式；失败再回退为重建。已加入用户 To-dos 和记忆。

## 2. 当前状态（P1.4）
| 功能 | 状态 |
|---|---|
| HDMV 蓝光菜单（ISO/BDMV/index.bdmv） | ✅ KARA 测试盘全部正常 |
| 鼠标悬停/点击、方向键/回车、Ctrl+M 弹出菜单、Home 主菜单 | ✅ |
| 菜单无闪烁、跟手（同步字幕队列） | ✅ |
| 跳转后画面声音立即切换（链路 flush + 时间重设） | ✅ |
| LPCM 16/24bit 片段间格式变化 | ✅ |
| 进度条（标题时间）显示/点击/拖动 | ✅ |
| 章节：PageUp/PageDown、导航→章节、进度条标记 | ✅ 用户确认；刻度需开 选项→播放器→界面→显示章节标记（MPC-BE 原生，默认关） |
| 中文输入法下快捷键（VK_PROCESSKEY） | ✅ 主窗口/视频窗口禁用 IME + PreTranslateMessage 还原真实按键（待用户复测） |
| 语言包版本检查 | ✅ 只比较 1.9.3，不比较修订号（修订号=git 提交数，推送后会变） |
| 光盘切音轨 → 播放器同步 | ✅ 已编译；进正片时按光盘选择的音轨 PID 同步（未专门测） |
| 崩溃保护（blocklist）、开始 20 秒内读盘失败自动回退 | ✅ 已编译，未测 |
| 日志精简（bdnav_debug.txt 打开详细日志，2MB 轮换） | ✅ |
| DVD 菜单 | ✅ MPC-BE 原生，用户确认可用 |
| 菜单与正片编码不同的盘（重建链路） | ✅ P1.4 倩女幽魂3 UHD（H.264 片头/菜单 ↔ HEVC 4K 正片）用户测通：菜单背景有声、Play Film 立即开始、片头完整、正片菜单/进度条/章节正常；切换处末帧停 1–2 秒 |
| PG 字幕与导航同步 | ✅ P1.4：PG_TEXTST/PG_TEXTST_STREAM → 按 PID 选分离器字幕流；倩女幽魂正片字幕用户确认有 |
| BD-J（Java 菜单） | ❌ 自动回退原版（Odyssey 盘用户确认：直接播正片、章节有效、不卡）；已有 BD-J 测试盘 The Odyssey 2026 UHD，P2 需精简 JRE + libbluray BD-J |

## 3. 目录结构（E:\AI\MPC-BE-BDMenu）
```
HANDOVER.md            本文件
docs\PLAN.md           方案与逐条开发记录（详细） ; docs\P1_DESIGN.md 早期设计
repo\                  ★ 主源码（git clone yoonelAI/MPC-BE 分支 bdmenu-qw，本机编译用这个）
agent\                 本机编译代理的通信目录（request.txt / status.txt / build.log / errors.log / dll.log / push.log）
tools\                 setup_local.cmd 环境准备 | build_agent.cmd 编译代理 | build_bdnav.cmd 编 bdnav.dll
                       git_push.cmd 提交推送 | check_env.cmd 环境检查
deps\bdlibs\           libbluray 静态库 + 头文件（云端 bdlibs.yml 打包，Run 45592615593）
builds\local\          ★ 本机编译后的测试目录（mpc-be64.exe + bdnav.dll + 其余文件来自 1.9.3 便携包）
builds\logs_archive\   历次测试日志
release\               交付压缩包（portable / patch）
upstream\, src\        早期只读源码快照/镜像（已被 repo\ 取代，勿再改）
*.iso                  测试盘：KARA...Disc1.iso（HDMV, 弹出式菜单, LPCM 16/24bit）；倩女幽魂 III UHD（HDMV，格式切换）；
                       The Odyssey 2026 UHD（BD-J，P2 用）；www.98t.la@HAVD_112A.iso（DVD）
tmp\                   抽出的片段（cgs3_00003/00004.m2ts，诊断用，可删）
```
其他：Vita.and.Virginia ISO 在 `C:\Users\...\Downloads`（HDMV 14 标题，00005.m2ts 部分数据全零，菜单 IG 缺失，暂不可用）。

## 4. 本机编译（重要）
- 环境：VS2022 **Build Tools** 17.14（MSVC 14.44、ATL、MFC）、SDK 10.0.26100/28000、Git、WinRAR（无 7-Zip）、MSYS GCC 在 `E:\AI\tools\MSYS`。代理端口 127.0.0.1:7897。
- agent（AI）**不能在用户电脑上启动进程**（沙箱禁止 subprocess），所以用户常开 `tools\build_agent.cmd`；
  AI 写 `agent\request.txt`（白名单命令，原子写：先写 request.tmp 再 os.replace），代理执行后写 `agent\status.txt`。
  | 命令 | 作用 |
  |---|---|
  | full | 完整编译（含 ffmpeg，首次约 5–30 分钟） |
  | main | 增量编译主程序（约 1.5 分钟），成功后自动部署到 builds\local |
  | dll  | 编 bdnav.dll（几秒），部署到 agent\ 和 builds\local |
  | deploy | 只部署 | kara | 用 builds\local 打开 KARA | push | 提交推送 GitHub | stop | 退出 |
- 修改 build_agent.cmd 后要让用户**关掉代理窗口重开**（cmd 运行中的批处理被改会出错）。
- 坑：**修订号 REV_NUM = git describe 的提交数**，推送/提交后会变；agent `main` 只编 exe 不编 Lang → 语言包版本不一致（已放宽检查；发布前用 `full` 同时重编 Lang）。
- 坑：系统代码页 936 → 必须 `set CL=/utf-8`（代理已设）；改编译选项后删 `repo\_obj` 下 .obj/.pch；
  `repo\build.bat` 的 vswhere 加了 `-products *`（否则找不到 Build Tools）。
- 云端：GitHub Actions `.github/workflows/main.yml`（完整绿色包，约 15 分钟）、`bdlibs.yml`（libbluray 库包）、`bdprobe.yml`（诊断工具）。
  浏览器下载 ≥15MB 的 artifact 常超时 → 让用户手动下载。

## 5. 架构（代码位置：repo\src\apps\mplayerc\BDNav\ 等）
- **bdnav.dll**（`bdmenu\bdnav\bdnav.c`，静态 libbluray 1.5.1）：C API，见 `BDNav\bdnav_api.h`（API v5）。overlay 回调把 IG 平面画成 ARGB。
- **CBDNavLib**（BDNavLib.*）：LoadLibrary exe 目录的 bdnav.dll，版本校验；日志 `BDNavLog`；数据目录 `BDNavGetDataDir()`（exe 目录可写则用之，否则 %LOCALAPPDATA%\MPC-BE-BDMenu）。
- **CBDNavigator**（BDNavigator.*，单例）：libbluray 会话；读 192 字节 BD-TS；事件处理；自动调菜单；IG 平面拷贝+预转换；鼠标/按键（CUILock 让 UI 优先）；
  时间锚点/进度条（Marker→Anchor）；SeekTitle（bd_seek_time）；章节；音轨 PID；崩溃锁 `bdnav_session.lock` / `bdnav_blocklist.txt`。
- **CBDNavReader / CBDNavStream**（BDNavReader.*）：DirectShow 源滤镜（仿 CUDPReader），Size()=0 → 分离器流式模式；3MB 缓冲；
  用户跳转时截断未读数据并增加 flush 代数；时间标记；实现 `IBDNavSource`（`include\IBDNavSource.h`）。
- 打开方式：`%TEMP%\MPC-BE-BDMenu\<盘名>.bdnav` 标记文件（内容 BDNAV-MPC1），FGManager 按 chkbytes 选中 CBDNavReader；
  GetCurFile 报虚拟 `...\BDMV\STREAM\<clip>.m2ts`，旁边写入真实 .clpi → MPEG 分离器读到流信息（m_bIsBD）。
- **MPEG 分离器改动**（`src\filters\parser\MpegSplitter\MpegSplitter.cpp/.h`）：识别 CBDNavReader（CLSID 6B1C2A51-...C0DE）；
  DemuxLoop 检查 flush 代数 → `BDNavResync()`（清队列、Begin/EndFlush、NewSegment、`m_rtResyncTo`=流时间+200ms）；时间标记打标。
- **BaseSplitter 改动**：`m_rtResyncTo`、`m_nBDNavMarkArm/m_rtBDNavMarkTime`、`CPacket::bBDNavMark`（DSUtil\Packet.h）；
  `BaseSplitterParserOutputPin::ParseHDMVLPCM` 检测 LPCM 格式变化发新媒体类型、裁整样本块。
- **菜单叠加**：`BDMenuSubPic.*`（ISubPicProvider）；导航模式下 FGFilter.cpp 创建渲染器时 SubpicSets.nCount=0 → CSubPicQueueNoThread（同步）；
  全局 `g_bBDNavSubpicNoQueue / g_bBDNavNoQueueApplied`。
- **MainFrm.cpp**：OpenCurPlaylistItem 先 `OpenBDNav`；OnPostOpen 建图失败回退；OnBDNavNotify（菜单、章节、音轨、回退）；
  鼠标/按键（PreTranslateMessage、OnLButtonDown、OnMouseMove）；TIMER_STREAMPOSPOLLER 用 GetTitlePos；SeekTo→SeekTitle；OnNavigateSkip 章节；CloseMediaPrivate 关闭会话。

### 格式变化重建（P1.4-dev）
- 每个 clip 的签名 = 视频 "pid=codec/格式" 列表 + 音频 "pid=codec" 集合（bdnav_get_clip_info desc，bdnav.c 加了视频 /f 格式）。
- PLAYITEM 事件时若视频签名不同或出现当前图没有的音频 → `m_bSegmentEnd`：本次 bd_read 返回的新 clip 数据存入 `m_carry`，ReadTS 返回 kReadSegmentEnd，
  补一个时间标记，PostMessage(WM_BDNAV_NOTIFY, kNotifyRebuild)。读取线程停读 → 缓冲读完后 Read 返回 EOF → 分离器 EOS → EC_COMPLETE。
- MainFrm：EC_COMPLETE 且 IsRebuildPending → TIMER_BDNAV_REBUILD(1ms)；或 4 秒超时。BDNavRebuild：m_bBDNavKeepSession=true → CloseMedia(TRUE)（不关导航）→ OpenFile(标记文件)。
  CBDNavReader::Load → nav.BeginSegment()（清 segment 标志、锚点）→ 先送出 m_carry → PrepareClipInfoPath 用新 clip 的 clpi 并记录新签名。
  OnPostOpen → BDNavAfterOpen（菜单叠加、章节、光盘音轨/字幕同步）。
- dev2 修正：libbluray 只在 PSR 播放项号**变化**时发 PLAYITEM（00004 item0 → 00001 item0 无事件）→ PLAYLIST 事件也做格式检查（CheckFormatChange）；
  clip 有 PG 而当前图没有 PG → 也重建；旧图的 CBDNavReader 析构晚于新图 Load → 用 ClearReader(this)（否则流时间=0，进度条不动）；
  读完旧格式数据 → kNotifyDrained → 400ms 后重建（不再等 4 秒超时，分离器流式 EOF 常不发 EC_COMPLETE）；
  顶层菜单标题（title 0，常为数小时循环播放列表）不显示进度条。
- dev3 修正：重建时机改为"旧图播完"：分离器输出 pin 记录最后送出样本的结束时间（m_rtBDNavLastOut）→ IBDNavSource::SetDeliveredTime →
  CBDNavReader::IsPlayedOut（数据已读完且流时间 ≥ 最后样本-30ms）；MainFrm TIMER_BDNAV_PLAYOUT 每 40ms 检查，最多 8 秒；EC_COMPLETE 仍立即重建。
  CheckFormatChange 重建时同步更新 m_graphPG（否则 PLAYLIST 后的 PLAYITEM 再次触发 → 新图 Load 读不到数据 → 回退原版 → 正片无菜单）。
- dev4 修正：dev3 的“已送出时间”只在 DemuxLoop 里上报，而读完数据后 DemuxLoop 就停了 → 判断永远失效、等满 8 秒（Play Film 后停 8 秒）。
  改为输出 pin 每送出一个包就上报（CBaseSplitterFilter::OnBDNavDelivered 虚函数 → MpegSplitter → IBDNavSource::SetDeliveredTime）；
  用户跳转（菜单按钮/顶层菜单，3 秒内有 flush）引起的格式变化 → 立即重建（旧数据作废，不必播完）；日志 "stream: played out (...)"。
- dev5 修正：libbluray 读盘远快于播放（KARA 23 秒片头 9 秒就读完），固定 8 秒保底会截掉片头 → 保底改为"剩余未播时长 + 1.5 秒"（CBDNavReader::RemainingMs，暂停时自动延长，硬上限 2 分钟）。

## 6. 交付
- 当前发布：**P1.4**（`release\MPC-BE.1.9.3-BDMenu-P1.4.x64-portable.zip` 25.3MB / `...-P1.4.x64-patch.zip` 25.2MB，exe 与语言包版本 1.9.3.31，源码提交 c897005）。
  发布流程：更新 `bdmenu\packaging\BDMenu-README.txt` → `push` → `full`（exe+Lang 同修订号）→ AI 用 Python 从 builds\local 打包（排除 bdnav_*/日志/ini，portable 另写干净 mpc-be64.ini，patch 去掉 MPCBEShellExt64.dll 并加 release_scripts）。
- `release\MPC-BE.1.9.3-BDMenu-<版本>.x64-portable.zip`：绿色版（含 mpc-be64.ini → 设置保存在程序目录）。
- `release\MPC-BE.1.9.3-BDMenu-<版本>.x64-patch.zip`：安装版补丁（files\ + install_patch.cmd/uninstall_patch.cmd，自动备份到 安装目录\BDMenu_backup）。
  安装版路径 `C:\Program Files\MPC-BE x64`。脚本模板在 `repo\bdmenu\release_scripts\`。
- 回退：删 bdnav.dll 即原版行为。

## 7. GitHub
- 账号 yoonelAI，仓库 https://github.com/yoonelAI/MPC-BE ，分支 **bdmenu-qw**（从 tag 1.9.3）。他人旧分支 bdmenu-m1/m2-menu 不用。
- 推送：`tools\git_push.cmd`（或代理 push 命令），以远端最新为父提交本机工作区（reset --soft），提交信息取 `agent\commit_msg.txt`。首次弹 GitHub 登录。
- AI 无法直接用 git；以前用浏览器网页上传（已登录 yoonelAI）。

## 8. 经验/坑
- AI 沙箱写出的 exe/dll 带低完整性标签 → 运行时拒绝访问；**可执行文件一律由用户进程（代理/资源管理器）生成或解压**。
- run_python_on_device 禁 subprocess；Program Files 只能检测存在不能读。写 .cmd 用 GBK 编码。
- libbluray：用户跳转（菜单按钮/Home/seek）只在 UserAction 后首个 TITLE/PLAYLIST/SEEK 事件 flush；PLAYITEM 不 flush。
- 进度条：分离器流时间≠标题时间，靠锚点换算；CBaseSplitterFilter::GetCurrentPosition 是 E_NOTIMPL，流时间取自 CBDNavReader::StreamTime。
- BD-J 识别：BDMV\BDJO\*.bdjo 和 BDMV\JAR\*.jar **有文件**（目录存在但为空 = HDMV，例如 KARA）。

## 9. 下一步（按优先级）
0. 测试盘 **倩女幽魂 III 1991 UHD（TTG ISO, 99.3GB, E:\AI\MPC-BE-BDMenu\倩女幽魂 III ... -TTG.iso）** 已核对（11-25 21:50）：
   完整（UDF 分区边界与文件大小一致，主片 00007 抽样 300 点无空洞）、已解密（无 AACS 目录）、INDX0300（UHD）、FirstPlay/TopMenu/14 个标题全部 HDMV（BDJO/JAR 空目录）。
   编码：菜单循环 00000.mpls = 00000/00001.m2ts **H.264 1080p + LPCM**，IG 菜单在 00011.m2ts；片头 00002.mpls（H.264）、00004.mpls（HEVC 2160p + LPCM）；
   正片 00001/00005.mpls = 00007.m2ts **HEVC 2160p + 1080p DV 增强层 + LPCM 1.0 + AC3 + PGS**（6547s）。→ 正好测"菜单与正片编码不同/4K/DV/新增 AC3 与 PGS 轨"。
   早期候选（用户提供目录清单 E:\AI\MPC-BE-BDMenu\新建文本文档.txt）：
   - 蜘蛛侠：崭新之日 2026 UHD（BDJO 5 个 .bdjo、JAR\00000.jar、CERTIFICATE app/bu.discroot.crt）→ **BD-J**，UHD HEVC，TrueHD/Atmos
   - 倩女幽魂3 1991 UHD（无 BDJO/JAR，MovieObject.bdmv 1.81KB，大量 18–30KB 小 m2ts 菜单片段）→ **HDMV**，UHD HEVC + LPCM 1.0
1. 用户测输入法修复。
2. 收集"菜单与正片编码不同"的蓝光盘 → 实现格式变化时重建视频/音频解码链路。
3. PG 字幕同步（PG_TEXTST / PG_TEXTST_STREAM 事件 → 选择分离器字幕流 / 开关）。
4. BD-J：需 BD-J 测试盘；打包精简 JRE（参考 mpv-Yaozhi），libbluray BD-J 需 libbluray.jar（vcpkg 是否带 BD-J 支持待确认）。
5. 最终打包、发布 GitHub Releases。

## 10. 开发记录（摘要，详细见 docs\PLAN.md）
- 11-24 P0 验证 libbluray 直读 ISO、IG 菜单、鼠标命中；P1.1 bdnav.dll + 探测；P1.2 导航源/菜单叠加（云构建）。
- 11-25 本机编译环境；修沙沙声（LPCM 格式）、悬停延迟、跳转卡顿/重复（全链路 flush+resync）、根菜单、进度条（锚点+seek_time）、
  菜单闪烁（同步字幕队列）、进度条闪回/延迟（待定 seek 序号）；本机编 bdnav.dll；
  P1.3：章节、音轨同步、崩溃保护、早期失败回退、日志精简、绿色版+补丁包、git_push。
- 11-25 19:52 用户本机 git 登录并推送成功（baf65e7，27 文件）；20:00 中文输入法快捷键修复。
- 11-26 00:08 用户测 dev5：KARA 正常；Odyssey（BD-J）日志 "navigation not used (no HDMV navigation)" → 原版直播正片、章节正常、不卡；
  倩女幽魂 warning 现在完整播放（played out 27.986 s），首次看到 Arrow 标志后的英文段，**该段末尾有拖影、字被上下拉长**后进菜单。
  已把片头 clip 抽出到 `tmp\cgs3_00003.m2ts`（7.2MB），请用户用原版方式直接播放比对，判断是片源本身效果还是解码/重建问题（云端 ffmpeg 无 H.264 解码器，无法自检）。
- 11-26 00:19 查明：warning 片头 00002.mpls = 2 个播放项 **00003（18.56s，warning+Arrow）+ 00004（10.01s，英文段）**，同为 H.264 1080p 无音频（不重建），
  两个 clip in_time 都是 600s → 衔接处 PTS 回跳 18.56s、连接条件 1（非无缝）。用户确认 00004.m2ts 单独用 Pot 和本项目 MPC（原版方式）播放**都正常** → 问题在导航模式。
  抽出文件：`tmp\cgs3_00003.m2ts`、`tmp\cgs3_00004.m2ts`。
- dev6（诊断）：数据目录有 `bdnav_dump.txt` 时，每个播放链路段送给分离器的 TS 数据写到 `dump\seg_NN.m2ts`（总上限 4GB；含 carry 数据）；
  PLAYITEM 事件现在总写日志。已在 builds\local 放了 bdnav_dump.txt。下一步：AI 比对 seg_01 与 00003+00004 原文件（截断/多余数据/顺序），
  用户用 Pot 播 seg_01 看是否有拖影 → 区分"送出的数据有问题（libbluray/读取）"还是"分离器/解码处理 PTS 回跳有问题"。**排查完删除 bdnav_dump.txt。**
- 11-26 00:45 **P1.4 发布**：README 更新为 P1.4 → push c897005（ae22d68..c897005）→ full 编译（exe/Lang 1.9.3.31）→ 打包 portable（102 项）/patch（103 项），zip 校验通过。
  下一步 P2 BD-J（The Odyssey 2026 UHD：First Play/Top Menu=BD-J，90 标题=56 BD-J+34 HDMV，JAR 签名、证书齐全）。
- 11-26 00:28 结果：用户用 dev6 测 2 次**都没有拖影**，Pot 播 seg_01 也正常。AI 逐包比对：seg_01 = 00003 全部 + 00004 去掉开头 4 个 PSI 包（PAT/PMT/SIT/PCR，libbluray 正常行为），无截断/多余/乱序 → **数据正确**。
  dev6 与 dev5 的播放逻辑相同（只加了转储和日志）。差别在重建时机：dev5 那次是 "played out" 正好在最后一帧时重建；这次 played out 没触发（容差 30ms < 一帧 41.7ms），
  是在"剩余+1.5 秒"保底时重建，最后一帧先静止约 1.5 秒。推断拖影是**最后一帧还在显示时就拆渲染器**造成的偶发画面（字被拉伸），不是数据或解码问题。
  处理：保持现状（最后一帧静止后再重建，用户认可停顿可接受）；不要把容差调到 ≥1 帧。已删除 bdnav_dump.txt 和 dump 目录（转储功能代码保留，需要时再建 bdnav_dump.txt）。
  若拖影再出现：在 BDNavRebuild 里拆图前先暂停渲染/黑屏，或保证最后一帧显示后至少 300ms 再重建。
- 11-25 23:38 用户测 dev4：倩女 Play Film 立即开始、片头播完进正片、正片菜单可用、warning 停顿缩短；KARA 环球标志片头没播完就进正片（8 秒保底截断）→ 23:43 dev5。
  新测试盘 **The Odyssey 2026 UHD（TGA ISO，88.2GB）**：AI 解析 UDF 完整，无 AACS 目录、TS 同步字节/复制权限位正常（已解密）；INDX0300；**BD-J 盘**：First Play/Top Menu=BD-J，90 标题（56 BD-J + 34 HDMV）；JAR 00000.jar（515 项、签名）、00001\ 资源目录、CERTIFICATE 证书齐全；正片 00294.m2ts 86.2GB。→ 作为 P2 BD-J 测试盘。
- 11-25 23:26 用户测 dev3：正片右键菜单/字幕/进度条/章节正常、KARA 正常；Play Film 后暂停数秒（日志：8 秒超时）、4K 片头末尾仍被截。→ 23:30 dev4。
- 11-25 22:31 用户测 dev2：warning→菜单背景卡（日志：重建等满 4 秒）；菜单进度条已隐藏；4K 片头末尾被截；进正片时二次重建失败 → 回退原版（右键无菜单，字幕/章节正常）。→ 22:36 dev3。
- 11-25 22:18 用户测 dev：菜单背景有声；片头→菜单闪一下（重建等了 4 秒超时）；菜单进度条不动；正片无字幕（00004→00001 无 PLAYITEM 事件 → 没重建）；根/标题菜单相同（无弹出菜单的盘正常）。→ 22:24 dev2。
- 11-25 22:00 用户测倩女幽魂3：片头后菜单背景无声（首个 clip 00003 无音轨 → 图里没有音频）；Play Film 卡死（H.264 解码器收到 HEVC 4K）。→ 22:08 P1.4-dev 格式变化重建链路（编译成功）。
- 11-25 20:25 P1.3.1：推送后修订号 25→29，旧中文语言包被拒（"Your language pack will not work..."）→ mplayerc.cpp SetLanguage 只比较前三段版本；full 重编 Lang。
  用户确认：章节刻度（开启显示章节标记后）正常，片头无章节属正常。重新打包 release\*-P1.3.1-*.zip。
  测试盘：用户说倩女幽魂/蜘蛛侠下载可能不完整，会换别的盘。
