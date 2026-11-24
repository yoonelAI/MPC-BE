MPC-BE BDMenu 测试版 P1.1（回归测试）
=====================================

这一版只做两件事：
1. 把 libbluray 编进 bdnav.dll，随播放器一起发布；
2. 打开 ISO / index.bdmv 时，用 libbluray 检测一次光盘，结果写入 bdnav.log。

播放方式和原版 MPC-BE 1.9.3 完全一样（还没有菜单），用来确认改动没有破坏原有功能。

测试步骤
--------
1. 普通视频（mkv/mp4 等）：照常播放、拖进度条、切字幕音轨，确认一切正常。
2. 打开 KARA 的 ISO：应照常直接播放正片；状态栏会短暂显示一行 "BDNav: libbluray 1.5.1 | open OK ..."。
3. 打开 Vita 文件夹里的 BDMV\index.bdmv：同上。
4. 播放器目录下会生成 bdnav.log，记录每次检测结果。

回退：删除或改名 bdnav.dll，播放器即恢复为原版行为。
