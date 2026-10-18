# qmmio 调试模块移除说明

**给执行者**：按本文档操作即可把相机调障用的 qmmio 调试模块从设备的内核里彻底移除。
该模块是相机排障期间手工编写的 debugfs 直读直写工具，**无任何错误处理**：
对任意物理地址裸执行 `readl/writel`，一旦命中未映射或安全（secure-only）区域
会触发外部中止（imprecise abort）→ 内核 panic → 整机重启。
近期设备多次异常重启与"首次读取新地址族"高度相关，故此移除。

## 现状事实（先读）

- 模块源码：`drivers/misc/qmmio.c`（由 AKBUILD 注入，见下）。
- 配置符号：`CONFIG_QMMIO=y` 为 **bool 内建**（`config-postmarketos-qcom-msm8916.aarch64:7505`），
  编进 vmlinuz 本体。**无法运行时 rmmod，唯一移除途径 = 重建内核**。
- Kconfig 定义：`qmmio.kconfig`，`default y`、依赖 `DEBUG_KERNEL && DEBUG_FS && ARCH_QCOM`。
- 设备当前内核：`6.12.1-msm8916`，包 `linux-postmarketos-qcom-msm8916`，装机版 pkgrel=58。
- 设备 `/tmp` 是磁盘目录（非 tmpfs），上面的排障脚本（`safari*.sh` 等）全部依赖
  `/sys/kernel/debug/qmmio`，模块移除后它们会报错退出——无害，可顺手清理。

## 位置速查

pmaports 目录（本机路径，其他机器以各自 checkout 为准）：

```
/home/xys/.local/var/pmbootstrap/cache_git/pmaports/device/testing/linux-postmarketos-qcom-msm8916/
    APKBUILD                                   ← 接线处（source 列表、prepare() 注入、sha512sums）
    qmmio.c                                    ← 模块源码
    qmmio.kconfig                              ← 符号定义（default y）
    config-postmarketos-qcom-msm8916.aarch64   ← CONFIG_QMMIO=y 在 ~7505 行
    config-postmarketos-qcom-msm8916.armv7     ← 无此行（见"易错点"）
```

APKBUILD 内凡编成 grep 定位（行号可能漂移，勿硬按行号删）：

```
$ grep -n qmmio APKBUILD
  source= 列表:      qmmio.c / qmmio.kconfig 两行
  prepare():        cp "$srcdir/qmmio.c" drivers/misc/ ;  \
                    echo 'obj-$(CONFIG_QMMIO) += qmmio.o' ... ; \
                    cat "$srcdir/qmmio.kconfig" >> drivers/misc/Kconfig   ←共 4 行左右
  sha512sums:       qmmio.c / qmmio.kconfig 的两条 hash
```

## 移除步骤（构建机）

```sh
D=~/.local/var/pmbootstrap/cache_git/pmaports/device/testing/linux-postmarketos-qcom-msm8916
cd $D
grep -n qmmio APKBUILD          # 确认三处位置
# 1) 删 APKBUILD 三处：source 里的两行、prepare() 注入段（cp / obj- echo / cat kconfig）、
#    sha512sums 里对应两条
# 2) aarch64 配置：CONFIG_QMMIO=y 改为关闭态
sed -i 's/^CONFIG_QMMIO=y$/# CONFIG_QMMIO is not set/' config-postmarketos-qcom-msm8916.aarch64
# 3) armv7 配置没有该行：因 Kconfig default y，若日后用 armv7 构建会重新内联。
#    建议补一行关闭态（与 aarch64 同写法），放 QOM 相关配置附近即可。
```

可选（更净）：同时 `git rm qmmio.c qmmio.kconfig`（保留|string 亦无妨，
只要 prepare() 不再注入，Kconfig 精华内容不会被读）。

然后：

```sh
# pkgrel 在现有值上 +1（装机值 pkgrel=58 → 59）
$EDITOR APKBUILD   # pkgrel= 行
pmbootstrap checksum linux-postmarketos-qcom-msm8916
pmbootstrap build linux-postmarketos-qcom-msm8916
```

## 安装到设备

沿用既有内核更新流程（此前每次内核迭代走过）：

- 方式 A（在身）：
  ```sh
  # 构建产物约在 ~/local/var/pmbootstrap/packages/…

  # 传到设备 /tmp 后
  echo 123456 | sudo -S apk add --allow-untrusted --force-overwrite /tmp/linux-postmarketos-*.apk
  sync && reboot
  ```
- 方式 B：`pmbootstrap install`（整盘重刷，设备数据会清）——不推荐日常用。

提交由人负责 GPG 签名（git 提交规则不变）。

## 验证（装机重启后）

```sh
ls /sys/kernel/debug/qmmio         # 应不存在
dmesg | grep -i qmmio              # 应无输出
grep -ao qmmio /boot/vmlinuz* | wc -l   # 应为 0（内核镜像里已无该符号）
```

设备应照常启动，其余功能（相机 pBAA 30fps 出帧流）不受影响——
主线 camss 驱动并不依赖 qmmio，它只是调试工具。

## 顺手清理（可做可不做）

- 设备 `/tmp` 下所有依赖 qmmio 的排障脚本与快照：
  `rm /tmp/safari*.sh /tmp/probe*.sh /tmp/q1*.txt /tmp/L*.log /tmp/tg*.sh`
  （保留数据文件如 base.raw / tx*.raw / cap*.raw 供后续分析）
- bash 历史中含 `echo 1 > $Q/poke` 的命令提醒执行者**不要**在移除后复现——
  category 无害（模块已不在），但指避免误触其他调试路径。

## 若日后仍需 MMIO 调试（一句建议）

不要再恢复 qmmio 原样。需要时写健壮版：`ioremap` 后配合 `fault 处理/仅白名单地址段/mutex 保护/读完 iounmap`，
或改用用户态 `/dev/mem` mmap + SIGBUS 兜底，避开不可访问区域。
