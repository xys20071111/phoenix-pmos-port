# aports/ — pmOS 软件包拆分镜像（device-lenovo-phoenix / linux-lenovo-phoenix）

2026-10-24 起，本设备的 Lenovo PHAB Plus (PB1-770N, `lenovo-phoenix`) 不再
作为本地改动寄居在 pmbootstrap 通用包
`device-qcom-msm8916` / `linux-postmarketos-qcom-msm8916`（均已恢复为上游
pmaports 原始内容，快照基点 = 本地提交之前的 fdf5ba4 / origin/main），而是
拆分为两个独立的本地包：

| pmaports 路径 | 本仓库镜像 | 说明 |
|---|---|---|
| `device/testing/device-lenovo-phoenix/` | `aports/device-lenovo-phoenix/`（7 个文件） | 设备包：deviceinfo / modules-initfs / 30-gpu-firmware.files / 音频 udev+脚本 / modem 栈 runlevel 接线 |
| `device/testing/linux-lenovo-phoenix/` | `aports/linux-lenovo-phoenix/`（31 个文件） | 内核包：同 msm8916-mainline/linux v6.12.1 源 + 全部 phoenix 补丁/驱动/DTS/config |

## 事实来源（source of truth）

- **权威副本（部署态）** = pmbootstrap 里的
  `~/.local/var/pmbootstrap/cache_git/pmaports/device/testing/{device-lenovo-phoenix,linux-lenovo-phoenix}/`。
  `aports/` 下是从 pmbootstrap 工作目录复制来的**完整功能快照镜像**，
  直接整目录拷入 pmaports 即可复现（含 sha512sums）。
- 本仓库散放的源文件（`msm8939-lenovo-phoenix.dts`、`panel/`、`power/`、
  `camera/`、`camss/`、`modem/`）维持原角色："repo holds the source files,
  pmaports holds the wiring"（BUILD.md §0）。
- **已发现的仓库↔pmaports 偏差（拆分时盘点，见下）**：

| 文件 | 仓库版 | pmaports/部署版 | 处置 |
|---|---|---|---|
| `msm8939-lenovo-phoenix.dts` | r84 旧版（19.2 MHz MCLK） | r86 切换（24 MHz MCLK、link-freq 400 MHz，上机验证） | ✅ 仓库已同步到 r86 版 |
| `power/venus-msm8939-res.patch` | v9 final shape（69b09c7，拆分 vcodec clocks 双数组） | 同 v9（r83 上机版；r60..r82 部署为旧 shape = repo 630e3a7） | ✅ 已收编：2026-10-25 接线（pkgrel 83）同日上机通过——1080p@30/@60 硬解正常、无回归；时钟观测点无差属预期（8939 单 RCG，三颗分支钟共享父钟且均 `CLK_SET_RATE_PARENT`，两 shape 频控等价），见 BUILD.md §13 |
| `power/venus-v1-vcodec-clks.patch` | 同上（配套 pm_helpers 流） | 同上 | ✅ 同上（已上机通过收编） |

## 与通用 qcom-msm8916 包的对应关系（拆分对照）

### 内核包 `linux-lenovo-phoenix`（自 `linux-postmarketos-qcom-msm8916` 而来）

- `_flavor="lenovo-phoenix"`；`arch="aarch64"`（去掉 armv7 及其 config）。
- **不再**携带 `replaces="linux-postmarketos-qcom-msm8909 ... msm8939"`。
- `CONFIG_LOCALVERSION` 仍为 `-msm8916`（config 文件字节级与通用包 aarch64
  config 相同，仅 5 处本地 CONFIG 差异：SMB347/OV5693/OV13850/NT35532/QMMIO）。
  因此与旧内核包不能同装（/lib/modules 同名目录冲突）——换设备包后旧内核包
  直接丢弃，无需共存。
- **tarball distfile 出于复用 cache_distfiles 缓存（约 230 MB）与校验一致的
  考虑，沿用上游名** `linux-postmarketos-qcom-msm8916-v6.12.1-msm8916.tar.gz`
  （内容与校验和均与上游完全相同，APKBUILD 内有注释说明）。
- pkgrel 连续性：`pkgrel=82` == 通用包的 82（设备构建编号 #83），历史
  r60..r86 注释块原样保留。

### 设备包 `device-lenovo-phoenix`（自 `device-qcom-msm8916` 本地改动而来）

- `deviceinfo_codename="lenovo-phoenix"`；dtb 匹配收窄为
  `qcom/msm8939-lenovo-phoenix`；补 screen 尺寸 1080×1920。
- `depends=` 相比通用包本地末态的差异：**去掉 `firmware-qcom-adreno-a300`**
  （本板是 Adreno 405，只用 a420）；其余原样保留
  （a420、firmware-lenovo-phoenix-venus/modem、q6voiced 栈、alsa-utils、
  soc-qcom-msm8916-ucm、db410c wcnss-nv symlink hack、msm-firmware-loader 等）。
- 内核子包：`kernel-extlinux`（depends `linux-lenovo-phoenix`）与
  `kernel-efi`（U-Boot，本板不用，仅为与通用包结构保持同型）。
- `modules-initfs` / `msm8916-audio-init.sh` / `90-msm8916-audio.rules` /
  `30-gpu-firmware.files` / `kernel-cmdline.conf` 与拆分前逐字节一致。
- pkgver/pkgrel 延续旧编号（8 / r9），便于对照 BUILD.md 历史。

## 上机/构建注意

- pmbootstrap 已切到该设备：`pmbootstrap config device` → `lenovo-phoenix`
  （kernel 选择维持 `extlinux`，UI fbkeyboard 不变）。
- 存量 rootfs（旧包时代刷的）迁移：rootfs 里仍装着
  `device-qcom-msm8916` + `linux-postmarketos-qcom-msm8916`，最稳路径是
  重跑 `pmbootstrap install` 后**成对**刷 boot/root（image 纪律见 BUILD.md）。
  若要走 sideload 增量：设备上先
  `apk del device-qcom-msm8916 linux-postmarketos-qcom-msm8916` 再
  `apk add device-lenovo-phoenix`（内核 LOCALVERSION 未变，两内核包互斥，
  不能共存；device 包装时 kernel-extlinux 会把新内核拉进来）。
- pmaports 本地历史提交中 af110f2/9ee4fd3/244fc92 对两个通用包的改动已被
  本次恢复"撤销"（工作区与上游一致）；后续 rebase 上游时注意这三个提交仍
  会在历史里重放——建议未来找机会用新的一次性提交把拆分固化，或整理本地
  分支（用户决策，未自动提交）。
