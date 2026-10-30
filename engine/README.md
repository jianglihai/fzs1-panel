# IMS 引擎 · imsengine

蜂助手面板（`luci-app-fzs`）的配套引擎：让 EC200T / EC200N 模组在**旁路模式**下复用
模组自带 IMS 栈收发短信，并在需要时自建 IMS 注册、维护数据承载（PDP / RNDIS / 默认路由）。

> 这个目录于 **2026-10-25** 从 `jianglihai/fzs1-openwrt-ims`（固件编译仓库）迁入本仓库。
> 那个仓库承担的是「从零编译 OpenWrt 固件」，维护成本高，已停用并删除；
> 引擎源码与打包脚本全部保留在这里，改引擎不再需要碰固件编译链。

---

## 目录结构

```
engine/
├── mkpkg.py              打包脚本：引擎 ipk + 面板 ipk 重封 + 恢复包
├── board-files/          引擎在设备上的文件（按根目录摆放，mkpkg 原样入包）
│   ├── usr/bin/imsengine             引擎主程序（预编译 MIPS 二进制，3.6MB）
│   ├── etc/init.d/ims                IMS 服务（旁路检测 / 自建注册 / diag / learn）
│   ├── etc/config/ims                uci 配置
│   ├── etc/operators/*.json          运营商 IMS 参数（46000/46001/46011）
│   ├── usr/libexec/ims-ppp           PPP 拨号（建 IMS 专用承载）
│   ├── usr/libexec/ims-data          数据承载守护（激活 PDP / 绑 RNDIS）
│   └── usr/lib/imsengine/            面板补丁器 patch-fzs + 各 snippet + full/ 副本
├── imsengine/            引擎 Go 源码（改完用 build-engine.sh 重编）
└── build-engine.sh       交叉编译 → board-files/usr/bin/imsengine
```

`board-files/usr/lib/imsengine/full/` 里放的是**面板文件副本**（`fzs-at`、`sms.js`），
由 `mkpkg.py` 打包时从面板 ipk 里取同版本文件覆盖写入 —— 用途是引擎包 `postinst`
的「自愈」：面板文件被旧备份顶掉时用它补回。**不要在 `full/` 里手改**，
改了下一版打包就会被面板 ipk 的内容盖掉。

---

## 打包

```bash
# 一条命令出两个包（先面板后引擎）
bash build-all.sh                 # 用默认版本
bash build-all.sh 0.3.7 0.1.20    # 面板版本、引擎版本

# 只重打引擎包（面板已经打好、或只改引擎时）
python3 engine/mkpkg.py
```

面板 ipk 的取材优先级：`FZS_PANEL_IPK` 环境变量 → 本仓 `src/dist/` 里的同版本文件 →
`build/packages/` 缓存 → GitHub Release 下载。

改版本号的位置：
- 面板：`src/build_ipk.py` 的默认版本 / `bash build-all.sh <面板版本>`
- 引擎：`engine/mkpkg.py` 顶部的 `ENGINE_VER`、`PANEL_VER`

---

## 改引擎 Go 源码

```bash
bash engine/build-engine.sh
```

固定参数：`GOOS=linux GOARCH=mipsle GOMIPS=softfloat CGO_ENABLED=0`
（ramips/mt76x8 = MIPS 24Kc LE，无硬件浮点；用错参数编出来的二进制在设备上直接
`Segmentation fault` 或 `Exec format error`）。

需要 Go 1.21+ 与 Linux / WSL2 / Git Bash 环境，Windows 原生 cmd 编不了。
改完记得把 `board-files/usr/bin/imsengine` 一起提交 —— 那才是打进包里的东西。

---

## 版本要点

| 引擎版本 | 内容 |
| --- | --- |
| `0.1.20` | **AT 口一律动态发现**：新增 `ims-atport`（按 sysfs `idProduct` + 接口号 `>= :1.3` 枚举，`:1.4` 优先）与 `ims-submit`（文件交互式发短信，绕开 `rc.common` 的 `procd_lock`，修掉发短信无限卡死）；`native_self_detect` / 诊断段 / `ims-data` 全部改走 `ims-atport`；修掉 `start_bypass_watch` 吊住 procd 管道 + 子 shell 抢父进程锁造成的自锁；postinst 不再同步调 `restart`，opkg 不再阻塞 |
| `0.1.13` | 随面板 0.3.5 发布；`sim` 子命令（读卡状态/重新初始化）、开机读卡自愈 `fzs-simretry`（面板包内） |
| `0.1.12` | 修「升级后新功能消失」（prerm 不再在 upgrade 时还原面板文件）、`ims diag` AT 探测改写 + watchdog 重定向 |
| `0.1.8` | 旁路态补 `REG_SINCE` / `UP_SEC` / `RENEW_MODE`；「导出诊断日志」按钮 |
