# VRAM Manager for X-Plane 12（显存管理器）

X-Plane 12 原生插件（C++ / XPLM 4.3），用**降低渲染距离**的方式管理显存，界面是自绘的现代化面板。

> **设计原则**：只改渲染距离，**绝不写纹理等级** —— 贴图不会因为本插件变糊。
> 纹理等级只做只读显示，用来区分“是 X-Plane 自己降的”还是别的插件降的。

---

## 1. 安装与目录

```
X-Plane 12/Resources/plugins/VRAMManager/
├── 64/win.xpl        插件本体（Windows 64 位，无额外运行时 DLL 依赖）
├── src/              源码
├── SDK/              X-Plane SDK 4.3 头文件与导入库（离线重新编译用）
├── build.ps1         一键编译脚本
└── README.md         本文件
```

启动后 **Plugins → VRAM Manager** 出现菜单；`Log.txt` 里搜 `[VRAMManager]` 可看到：

```
[VRAMManager] VRAM: ===== VRAM Manager v1.9.18 loaded (X-Plane 12) =====
[VRAMManager] VRAM: modern panel created at 467,681 size 520x352 (scale 1.00)
[VRAMManager] VRAM: panel fonts ready (scale 1.00, body tex=156 glerr=0)
[VRAMManager] VRAM: render-distance dataref resolved late: '.../reno/LOD_bias_rat' (type=2) base=2.2500
[VRAMManager] VRAM: first sample src=1 used=3.02 GB ... vram=7.71 GB
```

---

## 2. 菜单（Plugins 下拉栏，全英文）

| 菜单项 | 作用 |
|---|---|
| Show / hide panel | 显示 / 隐藏面板（勾选=可见） |
| Auto manage render distance | 自动管理开关 |
| Purge VRAM now | 立即清理一次（只压渲染距离，约 5 秒后恢复） |
| Restore X-Plane settings | 恢复原始渲染距离并关闭自动管理 |
| Reload scenery (hard purge) | 重载地景（最彻底，也最卡） |
| UI size | 面板缩放：自动 → 80% → 90% → 100% → 115% → 130% → 150% |
| About / diagnostics | 把显卡、显存来源、dataref 信息写进 Log.txt |

命令（可在 设置 → 键盘 绑定）：`vrammanager/panel`、`vrammanager/auto`、`vrammanager/purge`。

---

## 3. 面板

自绘面板（OpenGL 立即模式 + GDI 字形图集）：

```
┌──────────────────────────────────────────────┐
│ ● VRAM Manager                          v1.9.18  ×│
│  5.16 GB                              67%    │
│  ████████████████░░░░░░░░░░░░░░░░░           │
│  of 7.71 GB installed                         │
│  ┌ LOD 2.250 ┐┌ TEX 4 ┐┌ FPS 58 ┐            │
│  [●] Auto manage        [●] Auto rescue purge │
│  Target VRAM     ──────●───────  82%          │
│  Min distance    ──●───────────  55%          │
│  [ Purge ] [ Reset ] [ Save ] [ Hide ]        │
│  Holding at 100%                              │
└──────────────────────────────────────────────┘
```

* 文字用 `Microsoft YaHei UI` 的 GDI 字形图集（12/14/17/27 px，抗锯齿），支持中文；
* 进度条按占用变色：<80% 绿、80–92% 琥珀、≥92% 红；
* 顶部标题栏可拖动窗口，右侧 `×` 关闭；
* 底部一行显示状态（保持 / 降低 / 恢复 / 等待 dataref / 清理中）。

---

## 4. 自动管理逻辑

每 0.5 秒采样显存、每 0.9 秒决策一次，指数平滑 + 滞回。**两级杠杆，优先级从"最看不出来"排到"最能省"**：

1. **纹理驻留距离**（首选）——显存高于目标时按缺口比例收紧（默认 6 km，最低 2.5 km）。
   远处贴图不再常驻，这是每单位视觉变化省显存最多的一项；
2. **渲染距离**——驻留距离已到地板仍超标时才动，且默认最低只到 75%；
3. **云层 / 渲染目标修剪**——Cloud trim 设为 Auto 时按压力自动升降档。

反向恢复顺序相反：先把渲染距离还原，再把驻留距离放回去。

阈值：`Target VRAM`（默认 62%）以上收紧，`Restore`（默认 54%）以下恢复；地景加载中
（`async_scenery_load_in_progress`）不动作；压到最低仍 >92% 且勾选 Auto rescue purge →
每 2 分钟最多一次应急清理。

停用插件/退出游戏时会把原始渲染距离、驻留距离、纹理预算、云层开关**全部写回**。

---

## 5. 清理显存

| 方式 | 触发 | 原理 | 代价 |
|---|---|---|---|
| 浅清理 | Purge 按钮 / 菜单 / 自动应急 | 把纹理驻留距离临时收到 600 m 约 2 秒，迫使分页器卸载远处贴图，再恢复 | 远处贴图重新流式加载，近处不变 |
| 微扫 | 配置 `microsweep=1` | 每 55 秒把驻留距离收到 2500 m / 1.5 秒 | 只在巡航且帧率 >35 时触发 |
| 重载地景 | Reload scenery | `sim/operation/reload_scenery` | 卡顿最久，适合落地后使用 |

本插件对 `reno/tex_res` **只读不写**，所以不存在"清理后贴图变糊"。

---

## 6. 关于“贴图变糊”

X-Plane 12 自带显存保护：显存吃紧会自己下调纹理等级，表现就是座舱/地景变糊。
**这不是本插件干的。** 面板的 `TEX` 字段显示当前等级；如果被 X-Plane 降低了，
底部会提示 `Note: X-Plane lowered its texture level ...`。

本插件方向相反：提前压渲染距离 → 显存压力下降 → X-Plane 更不容易降纹理等级。

---

## 7. 不与其它插件冲突

1. 只写 `reno/LOD_bias_rat` 一个值；其余 `sim/private/*` 全部只读。
2. 写后 0.5 秒回读校验：被改成第三个值 → 让出控制 8 秒并以当前值为新基准；
   写入被 X-Plane 每帧覆盖 → 连续 2 次后降级为只读监控。
3. 不挂钩、不改内存、不改 `X-Plane.prf`，配置单独存放。
4. 启动扫描插件列表与 `FlyWithLua/Scripts`，命中 LockYourTexture / VisualXP 等关键字
   只写 Log.txt，不打扰界面。

---

## 8. 已知限制

* 渲染距离依赖私有 dataref `sim/private/controls/reno/LOD_bias_rat`（XP 12.4.3 实测可写，
  基准 2.25）。失效时自动降级为只读监控并把候选名单写进 Log.txt。
* 只降渲染距离的代价是**远处景物变简单**，近处和座舱不受影响。

---

## 9. 绘制与输入实现要点（踩过的坑）

X-Plane 12 的 GL 桥 / 输入系统和插件常见写法有若干不一致，代码里都写了注释：

**绘制**

* 绘制必须放在 `XPLMRegisterDrawCallback(..., xplm_Phase_Window, ...)` 里；
  窗口自己的 `drawWindowFunc` 在这台机器上拿不到可用的 GL 上下文；
* **GL 桥不会给插件窗口设置 2-D 投影矩阵**，必须自己建：

  ```cpp
  glViewport(窗口矩形);
  glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
  glOrtho(left, right, bottom, top, -1, 1);
  glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
  ```

  否则所有顶点都落在默认裁剪空间外，**一个像素都画不出来**；
* 只可靠支持 `GL_QUADS`；`GL_TRIANGLE_FAN` / `GL_QUAD_STRIP` 会被静默丢弃
  → 圆角、圆形全部用四边形拼；
* 每批绘制前重设“1 个纹理单元 + `glDisable(GL_TEXTURE_2D)`”；
* 纹理用 `glGenTextures` / `glBindTexture` / `glTexImage2D`（与 LockYourTexture 一致）；
* `glTexSubImage2D` 上传字形子图时**必须设 `GL_UNPACK_ROW_LENGTH = 图集宽度`**，
  否则 GL 按子图宽度读行，采样出来的 alpha 全是 0，文字完全透明；
* 字形图集是自上而下上传的 DIB，所以 `V` 坐标是 `y/texH`（不是 `1-y/texH`）。

**输入**

* 窗口装饰必须用 `xplm_WindowDecorationSelfDecorated`；
  `xplm_WindowDecorationNone` 的窗口**点击会穿过边界**（SDK 注释原话），收不到任何鼠标事件；
* **鼠标坐标和窗口几何不在同一空间**：X-Plane 按 UI 缩放（本机 150%）上报鼠标位置
  （2560×1440 屏幕上报的是 1707×960 空间），而窗口几何用真实像素。
  直接用 `XPLMGetMouseLocationGlobal()` 会让点击整体偏移约 1/3 屏宽。
  正确做法是用系统光标 + 屏幕高度翻转：

  ```cpp
  POINT pt; GetCursorPos(&pt);
  int gx = pt.x, gy = screenH - pt.y;
  ```

* 拖动不依赖 `xplm_MouseDrag`，而是每帧轮询 `GetAsyncKeyState(VK_LBUTTON)` + 光标位置。

---

## 10. 配置文件

`X-Plane 12/Output/preferences/VRAMManager.prf`

```ini
auto=1         # 自动管理
autopurge=1    # 自动应急清理
show=1         # 启动时显示面板
debug=0        # 1=输出 PROBE/心跳日志
lang=0         # 0=英文，1=中文
uiscale=0      # 0=按屏幕自动
target=82.0    # 目标显存占用 %
restore=68.0   # 低于该值恢复渲染距离
min=0.550      # 渲染距离最低系数
max=1.000      # 最高系数（=X-Plane 原值）
step=0.040     # 调整步长
winx=467  winy=681   # 面板位置
```

---

## 11. 重新编译

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
cd "X-Plane 12\Resources\plugins\VRAMManager"
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

> 若 X-Plane 正在运行，`win.xpl` 被占用会导致链接失败（Permission denied）——先关游戏。

| 文件 | 内容 |
|---|---|
| `src/vram.cpp` | PDH 三级显存来源 + DXGI 预算/显卡信息 |
| `src/control.cpp` | dataref 解析/延迟重试/全量扫描、自动策略、清理状态机、冲突扫描 |
| `src/panel.cpp` | 自绘面板：布局、绘制、鼠标交互、拖动 |
| `src/gfx.cpp` | 2D 图元（纯四边形：圆角矩形、圆形、X 按钮、阴影） |
| `src/font.cpp` | GDI 字形图集 + 文本绘制 |
| `src/ui_menu.cpp` | Plugins 菜单、命令、中英文表、About |
| `src/plugin.cpp` | 入口、配置读写、飞行循环、消息处理 |

---

## 12. 卸载

删除 `Resources/plugins/VRAMManager` 目录和 `Output/preferences/VRAMManager.prf` 即可。
