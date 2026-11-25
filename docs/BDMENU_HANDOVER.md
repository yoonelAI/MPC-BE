# MPC-BE BDMenu 项目移交文档

> 每轮开发后更新本文件（顶部"当前状态"和末尾"开发记录"）。换 agent / 换对话时先读这里。
> 最后更新：2026-11-25 19:30（Asia/Shanghai） 版本 P1.3

## 1. 目标（用户需求）
- 基于 **MPC-BE 1.9.3**（最新稳定版）改内核，实现**蓝光 ISO / BDMV 先出菜单、鼠标可操作**；参考 mpv 0.42（PR #18080）和 Yaozhil/mpv-Yaozhi。
- 默认开启、**没有配置项**，出错自动回退原版播放；交付**绿色版压缩包**（含全部依赖，将来 BD-J 需要精简 Java）+ **安装版补丁包**。
- 用户决定：开盘直接进正片的盘（弹出式菜单盘）→ 正片开始约 3 秒后自动调出菜单。
- 项目放 `E:\AI`，不放 C 盘。回复用户用中文。

## 2. 当前状态（P1.3）
| 功能 | 状态 |
|---|---|
| HDMV 蓝光菜单（ISO/BDMV/index.bdmv） | ✅ KARA 测试盘全部正常 |
| 鼠标悬停/点击、方向键/回车、Ctrl+M 弹出菜单、Home 主菜单 | ✅ |
| 菜单无闪烁、跟手（同步字幕队列） | ✅ |
| 跳转后画面声音立即切换（链路 flush + 时间重设） | ✅ |
| LPCM 16/24bit 片段间格式变化 | ✅ |
| 进度条（标题时间）显示/点击/拖动 | ✅ |
| 章节：PageUp/PageDown、导航→章节、进度条标记 | ✅ 已编译，待用户测 |
| 光盘切音轨 → 播放器同步 | ✅ 已编译，未测（KARA 只有 1 条音轨） |
| 崩溃保护（blocklist）、开始 20 秒内读盘失败自动回退 | ✅ 已编译，未测 |
| 日志精简（bdnav_debug.txt 打开详细日志，2MB 轮换） | ✅ |
| DVD 菜单 | ✅ MPC-BE 原生，用户确认可用 |
| 菜单与正片编码不同的盘（重建链路） | ❌ 需要测试盘 |
| PG 字幕与导航同步 | ❌ |
| BD-J（Java 菜单） | ❌ 自动回退原版；需要 BD-J 测试盘 + 精简 JRE |

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
*.iso                  测试盘：KARA...Disc1.iso（HDMV, 弹出式菜单, LPCM 16/24bit）；www.98t.la@HAVD_112A.iso（DVD）
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

## 6. 交付
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
1. 用户测 P1.3：章节键、进度条章节标记、日志精简后是否正常。
2. 收集"菜单与正片编码不同"的蓝光盘 → 实现格式变化时重建视频/音频解码链路。
3. PG 字幕同步（PG_TEXTST / PG_TEXTST_STREAM 事件 → 选择分离器字幕流 / 开关）。
4. BD-J：需 BD-J 测试盘；打包精简 JRE（参考 mpv-Yaozhi），libbluray BD-J 需 libbluray.jar（vcpkg 是否带 BD-J 支持待确认）。
5. 最终打包、发布 GitHub Releases。

## 10. 开发记录（摘要，详细见 docs\PLAN.md）
- 11-24 P0 验证 libbluray 直读 ISO、IG 菜单、鼠标命中；P1.1 bdnav.dll + 探测；P1.2 导航源/菜单叠加（云构建）。
- 11-25 本机编译环境；修沙沙声（LPCM 格式）、悬停延迟、跳转卡顿/重复（全链路 flush+resync）、根菜单、进度条（锚点+seek_time）、
  菜单闪烁（同步字幕队列）、进度条闪回/延迟（待定 seek 序号）；本机编 bdnav.dll；
  P1.3：章节、音轨同步、崩溃保护、早期失败回退、日志精简、绿色版+补丁包、git_push。
