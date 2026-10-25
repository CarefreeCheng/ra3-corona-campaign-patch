# 技术细节与踩坑记录

面向想改代码、或想知道"为什么这么绕"的人。所有地址都基于 `ra3_1.12.game` **v1.12.3444.25830**（32 位，加载基址 `0x00400000`，镜像大小 `0x0093B000`）。

## 关键地址与结构

| 名称 | 地址 / 偏移 | 说明 |
| --- | --- | --- |
| 扩展建筑栏开关 | `0x1D` | 建造页 `CommandSet` 换成扩展版（`POST /toggle?f=0x1D&on=1`） |
| 科技门旁路开关 | `0x1E` | 对 10 栋 T4 直接 `setEnabled(true)`，绕过科技前置；**会导致"没高科也能造"**，所以默认关 |
| Thing vtable | `0x00BED890` | 单位/建筑实例的第一个 dword |
| Thing + 0x04 | — | `ObjectType*`（类型定义指针） |
| ObjectType + 0x08 | — | 类型名的 SAGE 哈希 |
| ObjectType + 0x1C | — | `char*` 类型名 |
| CommandSet vtable | `0x00BE9178` | 按钮表对象 |
| CS + 0x04 | — | inner 指针；`inner+4` = count，`inner+8` = 按钮指针数组 |
| 按钮 def + 0x04 | — | sub 指针；`sub+0x1C` = 产出的 `ObjectType*` |
| Proto 查表函数 | `0x007E4230` | `__thiscall(ecx=this, hash)`，`ret 4`；前导 7 字节 `83 EC 08 / 56 / 8D 71 28` |
| 建造页构建 | `0x00ACB330` | 一局只跑一次，所以 `0x1D` 要在**进对局前**打开 |
| 建造者 CS 探测点 | `0x00ACB352` | 上游文档里的 hook 点（本项目未使用） |

## 哈希

**两种哈希不能混用**：

- 脚本名（flag / timer）：FNV-1，起始 `2166136261`，`h = h * 16777619; h ^= c`，键是 8 字节 `{作用域哈希, FNV(name)}`。
- 单位 / 建筑 / CommandSet 名：SAGE SuperFastHash，参考实现见 `Ra3-Copilot/unit-codes/tools/sage_hash.py`。

本项目里出现的哈希：

| 名字 | 哈希 |
| --- | --- |
| `SovietPowerPlantAdvanced` | `0x524BBD01` |
| `SovietPowerPlantAdvanced_Enhanced` | `0x3C61BBA2` |
| `SovietPowerPlant` | `0xCF8D9F20` |
| `SovietBarracks` | `0x80D4EA1E` |
| `SovietRefinery` | `0xC45AAEAB` |

## 为什么用"换类型"而不是"加按钮"

- 按钮表是只读数据，加一格要重建整张表（尺寸、排序、UI 索引全变），风险高。
- 扩展模式本身的做法就是"每个建筑有一个 `_Enhanced` 变体，带着自己的按钮表"。
- 补丁只要让"创建建筑时查到的类型"变成 `_Enhanced` 变体，后面所有逻辑（技能、升级、数值）都自动是扩展版的。

## 为什么必须在"创建之前"

实测：建筑的命令栏在**出生那一刻**按类型缓存。给已经造好的建筑改 `Thing+0x04` 的类型指针，按钮不会刷新（选中、取消、重选都不行）；反过来把新建建筑的类型改回原版，按钮也**不会消失**。

所以 `csprobe.dll` 改的是 `0x007E4230` 的**入参哈希**：

```
原逻辑: type = Lookup(SageHash("SovietPowerPlantAdvanced"))
改之后: type = Lookup(SageHash("SovietPowerPlantAdvanced_Enhanced"))
```

只改一个参数、不动栈、不改控制流，所以比"函数体 detour"安全得多。

## 加载时序（重要）

```
启动 -> xinput1_3.dll 加载（我们的代理）
      -> 转发真实 xinput
      -> 等窗口 + 20 秒
      -> LoadLibrary(ra3copilot.dll)   // 太早会崩
      -> LoadLibrary(csprobe.dll)      // 内部还要重试最多 5 分钟，等日冕数据进内存
      -> CreateProcess(ra3fix_helper.exe)
```

`csprobe.dll` 的配对表靠扫内存里所有 `*_Enhanced` 标识符自建（上限 512 对），**第一次会扫空**（日冕数据未加载），所以有 `150 次 × 2 秒` 的重试循环。

## 踩坑记录（按痛苦程度排序）

1. **导出按序号导入**：游戏用 `#2` 这种序号导入 `XINPUT1_3.dll`。`.def` 里只写函数名时链接器按**字母序**编号，`#2` 变成了 `XInputSetState`，参数被当成结构体指针写入 → 栈被踩烂 → `0xC0000005` + `StackHash_e0ac`（BEX）。修复：`.def` 里显式写 `XInputGetState @2` 等。
2. **cdecl vs stdcall**：`0x004DFA70` 结尾是裸 `ret`（参数由调用方清理）。我在桩里写了 `ret 8`，多弹 8 字节，当场不炸、10 秒后才炸。
3. **指令边界**：inline hook 的跳转必须落在**完整指令之后**。`0x004DFA70` 的前导是 7 字节（`sub esp,8` + `mov eax,[esp+0Ch]`），按 6 字节切会落到指令中间。
4. **中文路径**：`LoadLibraryW` 传 `D:\一次尝试\...` 会返回 `ERROR_MOD_NOT_FOUND (126)`。所有注入都用 ASCII 路径（`%TEMP%\r3c`）。
5. **进程目录不是安装目录**：游戏 exe 在 `Data\ra3_1.12.game`，Windows 只会在 `Data\` 里找同名 DLL；根目录那份永远不会被加载（两份都放是为了 `helper` / 卸载方便）。
6. **不要用 PowerShell 改写含中文的源码**：`Get-Content -Raw` + `WriteAllText` 会把 UTF-8 当 ANSI 再存一遍，中文字符串全变乱码、还会吞引号（`CS1010`/`CS1519`）。用编辑器或写文件工具。
7. **`.bat` 里不要写中文**：cmd 在 `chcp 65001` 生效前就按本地代码页解析整个文件，中文字节里的 `&`/`|` 会把命令行劈开。本项目里的 `.bat` 一律纯 ASCII；需要中文提示的用 GBK + `chcp 936`。
8. **沙箱**：开发时的沙箱不允许写游戏目录，所以安装/卸载必须由用户双击执行（脚本会自己查进程占用）。

## 为什么不用 `dsound.dll` 当载体

试过，不行：这引擎的 DirectSound 初始化路径扛不住被代理，即使"纯转发"也会在初始化阶段崩（`WER` 报 `BEX` / `c0000005` / 异常数据 `8`，即 DEP）。`xinput1_3.dll` 只被手柄轮询触碰，风险最低。

（注意：日冕自己带了一个 `CoronaResources\DSound.dll` 代理，但它是给启动器用的，`UseDSoundDll` 开关默认 `false`。）

## 协议栏为什么不修

协议栏的格位是**玩家模板（PlayerTemplate）**自带的清单，在开局就按模板建好。`PLAYER_GRANT_SPECIAL_POWER` 能让引擎"拥有"技能（`/protocol?act=owned` 数量会 +1），但**不会长出新格子**。要修就得换玩家模板，副作用远大于收益。

顺带否定掉的两条路：

- **不是 `CommandSet`**：把战役和"遭遇战 + 扩展"的全部 CommandSet 导出对比（战役 343 个 / 遭遇战 342 个），**没有任何一个 `SpecialPower_*` 按钮**。
- **`/resolve` 查不到技能**：它只索引单位/建筑原型表，对 `SpecialPower_*` 一律返回 `template 00000000`。
