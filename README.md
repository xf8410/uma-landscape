# uma-landscape

赛马娘（日服）强制横屏 **Hachimi 插件**（`libhachimi_landscape.so`）。
目标：为「事件日志窗口嵌入游戏右侧」铺路（横屏后右侧才有空间）。

## 载体说明（重要）

**v0.1 走错了路**（Zygisk/Magisk 模块，需要 root）——你的环境是 **UmaPatcher 安装器 + Hachimi**，
不装 Magisk。v0.2 起改为 **Hachimi v3 插件**，跟小黑板（hlpatch 的 `libhachimi_ura.so`）同一载体：

```
UmaPatcher 改包 → libmain.so = Hachimi(-Edge)
                    └─ 插件自动扫描/加载：libhachimi_ura.so（小黑板）+ libhachimi_landscape.so（本插件）
```

`native/`、`module.prop` 是 v0.1 的 Zygisk 遗留，仅存档，**不要用**。

## 本插件做什么

1. **IL2CPP 层**：game-initialized 后 hook 两个 icall（Hachimi 插件 API 的 interceptor）：
   - `UnityEngine.Screen::RequestOrientation` → 竖屏请求一律改写为 Landscape
   - `UnityEngine.Screen::set_orientation` → 同上
2. **Android 层**：JNI 拿 `UnityPlayer.currentActivity`，直接 `setRequestedOrientation(LANDSCAPE)`，
   后台线程每 5s 重申（对抗游戏场景切换时重新锁竖屏）
3. **开关**：Hachimi 菜单里注册勾选框「强制横屏 (Landscape)」（跟小黑板同一菜单），状态持久化

## 与 Hachimi / hlpatch 不冲突

- hook 的两个函数 Android 版 Hachimi 不碰（其 Screen hook 仅 Windows 分支，`#[cfg(target_os = "windows")]`）
- 不碰 hlpatch 的 HTTP:18765 和任何 hook 目标
- 独立 so，不改 Hachimi/hlpatch 任何代码

## 安装

和 `libhachimi_ura.so`（小黑板）**完全相同的方式**装进游戏。

- 开关：游戏内 Hachimi 菜单 → 勾选「强制横屏」
- 配置文件兜底：Hachimi 数据目录 `uma_landscape.cfg`（内容 `on`/`off`）

## 预期别扭点（实测要看的）

- 竖屏 UI 在横屏下的布局（两侧黑边 / 拉伸 / 局部错位）
- 刘海/挖孔安全区、弹窗坐标
- 触控映射（若错位 → v0.3 补）

## OurPlay 机制分析（为什么它"什么游戏都能横"）

OurPlay 闭源，但机制可判明：它是**容器/虚拟机类**加速器（VirtualApp 一系）——游戏跑在它自己的
虚拟 Android 实例里，容器在装包时直接改写目标 APK 的 `screenOrientation` 属性，系统层解决，
不需要 hook 游戏。这条路证明了游戏引擎对横屏 surface 适应良好——竖屏是游戏代码自己锁的，
所以在运行时把"锁竖屏"的那两个调用拦掉即可。

## 路线

- v0.2：Hachimi 插件（本版）——icall hook + JNI 反制 + 菜单开关
- v0.3：按实测修（触控映射 / UI 适配 / enforcer 频率）
- v2：横屏稳定后 → 事件日志做成 `gui_show_window` 窗口，**放游戏右边**（Hachimi v3 插件
  API 原生支持插件窗口，这就是"浮窗放游戏右边而不是单独开浮窗"的实现路径）
