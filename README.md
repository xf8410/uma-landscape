# uma-landscape

赛马娘（日服）强制横屏 Zygisk 模块。目标：为「游戏内右侧内嵌浮窗」铺路（横屏后右侧才有空间）。

## OurPlay 为什么能强制横屏（机制分析）

OurPlay 是闭源的，读不到代码，但机制可以判明：它是**容器/虚拟机类**加速器（VirtualApp 一系），
游戏跑在它自己的虚拟 Android 实例里——容器在装包/创建 Activity 时**直接改写目标 APK 的
`screenOrientation` 属性**，等于从系统层把 Activity 定成横屏，不需要 root、不需要游戏内 hook。
这条路证明了：**游戏引擎对系统给的横屏 surface 适应良好，竖屏是游戏自己的代码锁的**
（Unity 里调用 `Screen.orientation = Portrait`）。

所以真机客户端上等价的做法是引擎层 hook：游戏每次请求竖屏，我们都改写掉。

## 本模块机制（v0.1）

```
Zygisk 注入目标包 → 等 libil2cpp.so 加载
→ dlsym(il2cpp_resolve_icall) → 解析并 hook 两个 icall：
   UnityEngine.Screen::RequestOrientation(UnityEngine.ScreenOrientation)   → 强制改写为 LandscapeLeft
   UnityEngine.Screen::set_orientation(UnityEngine.ScreenOrientation)      → 同上
+ 主动 enforcer：每 3s 调 orig_request(LandscapeLeft)，防止场景切换时游戏重新锁竖屏
```

- **与 Hachimi / hlpatch 零冲突（结构性保证）**：这两个函数在 Android 版 Hachimi 里
  只在 Windows 分支被 hook（`#[cfg(target_os = "windows")]`，见 Hachimi-Edge
  `src/il2cpp/hook/UnityEngine_CoreModule/Screen.rs`），hlpatch 完全不碰屏幕方向。
  三者各自独立 so，互不改代码。
- **开关**：
  - 配置文件：`/sdcard/Android/data/<包名>/files/uma_landscape.cfg`，内容一行
    `on` / `off` / `left` / `right`，每 2s 轮询，重启保持。
  - HTTP：`127.0.0.1:18766/on | /off | /left | /right | /status`。
  - 游戏内浮钮（v1.1 计划）。

## 安装与验证

1. Magisk 装 Actions 产出的 zip → 重启。
2. **先确认包名**：默认 `jp.co.cygames.umamusume`——装之前用你手机上的实际包名核对
   （`pm list packages | grep -i uma`），不符则改 `native/landscape.cpp` 里 `expected`。
3. 开游戏 → 应直接横屏。开关试配置文件和 HTTP。

## 预期别扭点（实测要看的）

- 竖屏 UI 在横屏下的布局方式（两侧黑边 / 拉伸 / 局部错位）
- 刘海/挖孔安全区
- 个别弹窗钉死竖屏坐标
- 触控坐标是否随旋转正确映射（若错位 → v1.2 补触控映射层）

## 路线

- v0.1：引擎层 hook + 配置/HTTP 开关（本版）
- v1.1：游戏内浮钮开关（小圆点 overlay）
- v1.2：按实测反馈补 Activity 层 hook / 触控映射 / UI 适配
- v2：横屏稳定后 → 事件日志浮窗嵌入游戏右侧（对齐日服 PC 官方布局）
