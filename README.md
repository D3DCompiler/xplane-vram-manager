# VRAM Manager（显存管理器）for X-Plane 12

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/D3DCompiler/xplane-vram-manager)](https://github.com/D3DCompiler/xplane-vram-manager/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)](https://github.com/D3DCompiler/xplane-vram-manager)


一个专门管**显存**的 X-Plane 12 插件：把显存占用压到你设定的目标值并稳住，
**不牺牲画质**——贴图分辨率全程不动，所以座舱和地景不会变糊。

> 和其它优化插件的区别：它们调的是帧率，调的往往是物体密度、反射、阴影这些你能看出来的东西；
> 这个插件只动**纹理驻留距离**和**渲染距离**，并且会盯着 X-Plane 别偷偷降你的贴图。

---

## 装在哪

把 `VRAM-Manager` 文件夹整个放进：

```
X-Plane 12\Resources\plugins\
```

最终路径应该是：

```
X-Plane 12\Resources\plugins\VRAM-Manager\64\win.xpl
```

进游戏后，菜单栏 **Plugins → VRAM Manager** 里就能找到它。

---

## 面板

```
┌──────────────────────────────────────────────────┐
│ ● VRAM Manager                          v1.9.18 ✕│
│  4.85 GB                                    63%  │
│  ████████████████████████░░░░░░░░░░░░░░░░░░      │
│  of 7.71 GB dedicated VRAM                       │
│  Render distance 100%   Texture level 4   FPS 58 │
│                                                  │
│  [✓] Auto manage render distance   [ ] Auto rescue│
│  [ Cloud trim: Auto            ON ]              │
│                                                  │
│  Target VRAM (purge above this)              62% │
│  Minimum render distance                     75% │
│  Texture budget                       default    │
│  Texture paging distance               6.0 km    │
│                                                  │
│  [ Purge now ] [ Restore ] [ Save ] [ Hide ]     │
│  Holding at 100%                                 │
└──────────────────────────────────────────────────┘
```

| 位置 | 含义 |
|---|---|
| 大数字 / 百分比 | 当前 X-Plane 进程占用的显存，颜色随占用从绿变黄再变红 |
| Render distance | 渲染距离，**100% = X-Plane 原本的值**，被压时会显示 85%、70% 之类 |
| Texture level | X-Plane 的纹理等级，**只显示不修改**，用来判断是不是它自己降的 |
| FPS / `[VRAM limited]` | 帧率；方括号提示当前是什么在限制（显存 / 帧率 / 贴图等级） |
| 标题栏 | 按住可拖动窗口，位置会被记住 |

---

## 三个核心功能

### 1. 自动管理（默认开启）

显存超过 **Target VRAM** 就开始降，降到目标以下自动恢复。它会按"最不容易看出来"的顺序动手：

1. 先缩短**纹理驻留距离**（远处地景贴图不再常驻，近处和座舱完全不受影响）；
2. 还不够才缩短**渲染距离**（默认最低只到 75%）；
3. 云层 / 阴影 / 光照探针的修剪（Cloud trim 选 Auto 时）最后才动。

恢复时顺序相反。

### 2. 一键清理（Purge now）

把纹理驻留距离临时收到 600 米约 2 秒，强制 X-Plane 卸载远处的贴图，然后恢复。
适合落地后、进近前这类"想立刻腾出显存"的时刻。

### 3. Cloud trim（云层/渲染目标修剪）

四档：**Off / Light / Max / Auto**

- **Light**：降水粒子图和云阴影步进，看不出来；
- **Max**：再加上云阴影图、阴影相机贴图、光照探针、IBL 探针、散射缓存减半——分辨率减半，肉眼很难分辨；
- **Auto**：按显存压力自动升档降档（推荐）。

**注意**：云体的形状（噪声体分辨率）永远不动，所以云不会"被削平"。

---

## 为什么贴图不会变糊

X-Plane 在显存吃紧时会自己把贴图精度砍半，那才是变糊的原因。
本插件**从不写纹理等级**，只做两件事：

- 提前把显存压到安全区，让 X-Plane 没必要降贴图；
- 万一它真的降了，面板会用琥珀色提示 `X-Plane lowered textures to N`，并稍微多留一点余量帮它升回来。

如果你同时装了 LockYourTexture 之类的锁纹理插件，两边不冲突——本插件不会去写那个值。

---

## 常见问题

**Q：显存没降下来？**
先看面板百分比和 `Target VRAM` 的差距，再到 `Log.txt` 搜 `texture paging ->`。
如果驻留距离已经压到 2.5 km 还降不下来，说明这套地景的贴图量确实大，可以把
`Target VRAM` 再调低、或者把 `Texture budget` 拉到 `1.2~1.5` 让它用更多显存换清晰度。

**Q：帧率掉了？**
先确认面板是不是开着——关掉面板再测。另外配置里这几项如果被打开会明显影响帧率，确认它们是 0：

```
priority=0   timeres=0   microsweep=0   texbudgetauto=0
```

**Q：想要更清晰 / 更省？**

- 更清晰：`Texture budget` 往右拉（X-Plane 会更晚缩贴图，显存占用会升高）；
- 更省：`Texture paging distance` 往左拉（4 km 以下远景贴图会明显变简单）。

**Q：和别的优化插件冲突吗？**
不抢控制权。别的插件改渲染距离时，本插件会重新接管，但不会关闭自适应逻辑；
检测到潜在冲突只写日志，不影响功能。

---

## 配置文件

`X-Plane 12\Output\preferences\VRAMManager.prf`（首次运行自动生成）

| 键 | 说明 |
|---|---|
| `auto` | 自动管理开关（1/0） |
| `target` / `restore` | 目标显存百分比 / 恢复阈值 |
| `min` | 渲染距离最低比例（0.75 = 最多压到 75%） |
| `paging` | 纹理驻留距离（米），0 = 用 X-Plane 原值 10000 |
| `texbudget` | 纹理预算倍率（1.0 = 原值，越大越清晰也越吃显存） |
| `cloudtrim` | 0=Off 1=Light 2=Max 3=Auto |
| `show` | 启动时是否显示面板 |

面板上改完会自动保存，手改配置需要重启游戏生效。

---

## 日志与反馈

所有信息都在 `X-Plane 12\Log.txt` 里，搜索 `[VRAMManager]`。

遇到问题请附上这几行：

```
VRAM: ===== VRAM Manager v... loaded =====
VRAM: texture paging distance = ... m (original)
VRAM: texture paging -> ... m (usage ..., render distance ...)
VRAM: first sample src=... used=... GB
```

---

## 从源码构建（可跳过）

已经装好插件的用户不需要这一节。

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

---

版本号规则：`x.x.xx`（最后两位为构建编号）。更新内容见 [CHANGELOG.md](CHANGELOG.md)。

---

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。可自由使用、修改、再分发（含商用），
保留版权声明即可。X-Plane SDK 的授权归 Laminar Research 所有，见 `SDK/license.txt`。

---

作者：TheCompiler · https://github.com/D3DCompiler
