# KQPro V1.1.4 二进制兼容证据

分析日期：2026-09-09。对象是工作区内 `氪奇Pro-V1.1.4` 的原版文件；仅进行磁盘文件的只读 PE 解析和反汇编，没有修改原版、执行登录或读取账号数据。

## 1. 结论和适用范围

V1.1.4 的三个桥接函数仍具备现有扩展使用的语义和 Windows x64 参数传递方式，但 RVA 已经变化。必须登记独立的 `1.1.4` profile，不能继续使用 V1.1.3 的地址。建议配置如下：

| 入口 | V1.1.4 RVA | 校验字节数 | 可执行节内精确匹配数量 | 策略 |
|---|---:|---:|---:|---|
| ServiceGetter | `0x13FDA0` | 16 | 1 | 只调用，不安装 trampoline |
| Dispatch | `0x140270` | 17 | 1 | 允许复制已验证的完整、无重定位前导指令 |
| CommandSender | `0x140DC0` | 28 | 1 | 只调用，不安装 trampoline |

Qt Core、Gui、Widgets、Network 的文件版本仍为 `6.6.3.0`；QCefView 的目标导出、主 frame ID 和本任务涉及的调用边界与现有桥接相符。此结论支持进行 V1.1.4 适配和本地加载验证，不等于已经证明在线登录、背包响应和所有 UI 操作成功。

## 2. 文件身份

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `KQProV1.1.4.exe` | 2524160 | `8B4230AF89224BAF924FE16019DFA31F7A2A694E750B2E46B7785F3CAA1A356B` |
| `QCefView.dll` | 1328128 | `4EC7458E58F1F1940C9446AE86EE370CAD2ABE3222D2A45C0C3431B5F1D9D147` |
| `Qt6Core.dll` | 6326416 | `225BD38093416C825F2E3220213F64E1079E9AB20F4738DECC0FC6EB992E8A9E` |
| `Qt6Widgets.dll` | 6469776 | `C71E65B882A84F47114590784A256F14BA19202EC30B218CE4841B2C7256060B` |

EXE、QCefView、Qt6Core、Qt6Gui、Qt6Widgets 均为 `IMAGE_FILE_MACHINE_AMD64 (0x8664)`、PE32+ (`0x20B`)。EXE 首选 ImageBase 为 `0x140000000`，SizeOfImage 为 `0x26F000`。

EXE 与 QCefView 没有可读的 FileVersion/ProductVersion 文本。EXE 版本识别需要使用受支持文件名中的 `1.1.4`；不要因为缺少版本资源直接判为 `1.1.3`。

EXE `.text` 信息：RVA `0x1000`、VirtualSize `0x151C9F`、文件偏移 `0x400`、Characteristics `0x60000020`。下面的唯一性检查覆盖全部可执行节；本文件实际只有 `.text` 可执行。

## 3. 三入口精确签名

```text
ServiceGetter / RVA 0x13FDA0 / 16 bytes:
40 53 48 83 EC 40 48 8B 05 9B F2 11 00 48 85 C0

Dispatch / RVA 0x140270 / 17 bytes:
48 89 5C 24 10 55 56 57 41 56 41 57 48 8D 6C 24 D1

CommandSender / RVA 0x140DC0 / 28 bytes:
48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9
48 81 EC 00 01 00 00
```

发送函数的旧 21 字节签名在 V1.1.4 中匹配 **7** 处：`0x20560`、`0xB4950`、`0xB7A80`、`0xD5800`、`0xDCF70`、`0x10ABB0`、`0x140DC0`。补充完整的 `sub rsp, 0x100` 后，28 字节签名只匹配目标函数，且不需要扩大当前 profile 的 32 字节容量。

`.pdata` 的 `RUNTIME_FUNCTION` 记录独立证实了函数起点与边界：

| 函数 | BeginAddress | EndAddress（不含） | UnwindData RVA |
|---|---:|---:|---:|
| ServiceGetter | `0x13FDA0` | `0x13FE44` | `0x23C858` |
| Dispatch | `0x140270` | `0x140CBC` | `0x23D008` |
| CommandSender | `0x140DC0` | `0x141144` | `0x23C998` |

旧版入口在新 EXE 中已是其他内容。例如旧 Dispatch `0x115920` 处开始为 `8B 77 08 48 3B DE 74 11`，旧 ServiceGetter `0x115450` 处开始为 `EB 0B 41 8B CB 41 8B EA`。禁止只修改版本白名单而保留旧入口地址。

## 4. 函数语义与参数证据

### ServiceGetter

`0x13FDA6`：`mov rax, qword ptr [rip + 0x11F29B]`，读取全局单例指针 RVA `0x25F048`。非空时直接走返回路径；为空时打印 `WebViewService not initialized!`，分配 `0x20` 字节对象、调用 `QObject` 构造函数、初始化对象的 `+0x10/+0x18` 字段，随后在 `0x13FE34` 写回同一个全局单例。函数不需要入参，通过 RAX 返回对象指针，符合 `void* (__fastcall*)()`。

该函数可以创建尚未绑定 WebView 的服务对象；“返回非空服务对象”本身不证明游戏页面已经就绪。CommandSender 会另行检查服务的 `+0x10` 字段。

### Dispatch

函数内三个分支分别在以下地址比较方法名：

| 指令 RVA | 字符串 RVA | 内容 |
|---|---:|---|
| `0x1402A8` | `0x178B10` | `recivedata` |
| `0x1407E5` | `0x178C48` | `cutdata` |
| `0x140BDA` | `0x178C80` | `otherreturn` |

`0x140296` 把 R9 保存为 RSI；紧接着用 R9/RSI 作为 `QString::operator==(const char*)` 的 this 参数。因此第四个参数仍是方法名 `QString` 引用。

`0x14029C` 从 `[rbp + 0x7F]` 读取第五个参数到 RDI。按本函数前导指令计算，这等于函数入口时的 `[rsp + 0x28]`，符合 Windows x64 的第五个参数位置。随后读取 `[rdi + 0x10]` 判断列表是否为空、从 `[rdi + 8]` 取得首个元素，并调用 `QVariant::toString()`。三个目标分支都遵循该形式，符合现有 `const QList<QVariant>& arguments`。

现有桥接原样转发前三个机器字参数，读取第四和第五个参数，再调用原函数的设计可沿用。不应把此函数误当成只有 `method/arguments` 两个参数的普通回调。

### CommandSender

`0x140DEA` 把 RDX 保存到 R15，作为后续 QString 操作对象；`0x140DED` 把 RCX 保存到 R13，作为服务对象。`0x140DF8` 检查 `[rcx + 0x10]`，该字段为空则走原版警告/返回路径。

`0x140EE2` 构造分隔符 `0x7C ('|')`，`0x140F06` 调用 `QString::split`，随后检查结果至少有三段。`0x140F19` 引用 RVA `0x178CE0` 的字符串：

```javascript
document.myFlash.senddata('%1','%2',JSON.parse('%3'),'xml',-1);
```

后续通过 `QtPrivate::argToQString` 插入前三段。`0x1410CB` 从 `[r13 + 0x10]` 读取 QCefView 对象，`0x1410F5` 调用原版导入的 `QCefView::executeJavascript`。这与 `void (__fastcall*)(void* service, const QString* wire)` 和 `Ext|cmd|json` 协议一致。

## 5. Dispatch trampoline 的字节安全性

允许复制的 17 字节恰好覆盖以下 7 条完整指令：

```asm
140270  48 89 5C 24 10  mov [rsp+0x10], rbx
140275  55              push rbp
140276  56              push rsi
140277  57              push rdi
140278  41 56           push r14
14027A  41 57           push r15
14027C  48 8D 6C 24 D1  lea rbp, [rsp-0x2f]
; trampoline 跳回 RVA 0x140281
140281  48 81 EC E0 00 00 00  sub rsp, 0xe0
```

这 17 字节没有 RIP 相对寻址、相对 call/jmp 或条件分支；指令长度总和为 `5+1+1+1+2+2+5=17`，大于现有 14 字节绝对跳转。以相同寄存器和栈状态执行副本，再绝对跳回 `0x140281`，无需修正前导指令中的地址。

**不要把 Dispatch 签名任意延长到超过 24 字节。** 下一条 `0x140288` 开始的 `mov rax, [rip + 0x117AF1]` 是 RIP 相对指令，当前简单复制的 trampoline 不具备该类重定位能力。Getter 的 16 字节校验串已经包含 RIP 相对指令，所以它只可用于识别和直接调用，不能据此授予同样的 trampoline 策略。

这里证明的是指令复制和跳回位置的正确性；没有额外证明当前 hook 安装器的多线程写入原子性或 trampoline 区域的 Windows 异常展开能力。

## 6. Qt / QCefView 边界

QCefView 导出：

```text
RVA 0x91A0  ?executeJavascript@QCefView@@QEAA_NAEB_JAEBVQString@@1@Z
RVA 0x39110 ?invokeMethod@QCefView@@QEAAXAEBHAEB_JAEBVQString@@AEBV?$QList@VQVariant@@@@@Z
RVA 0xECB70 ?MainFrameID@QCefView@@2_JB = 0 (int64)
RVA 0xECB78 ?AllFrameID@QCefView@@2_JB = -1 (int64)
```

`executeJavascript` 导出入口实际执行 `mov rcx, [rcx + 0x28]` 后跳到 QCefView 私有实现，原样保留 RDX/R8/R9。原版 CommandSender 的调用点传入 `this`、`MainFrameID` 地址、脚本 QString 地址、脚本 URL QString 地址；与扩展的 `bool (__fastcall*)(void*, const qint64*, const QString*, const QString*)` 一致。现有 `frameId = 0` 符合该 QCefView 的 MainFrameID。

分析开始时的 `release/KQPetInventory.dll`（SHA-256 `201BCDE4E7CBD2B7CBE6A1AE0C91CA8C6218EF6ED5AD67C5B1CC50C1BBE809FE`）对原版 Qt 库的全部命名导入均能找到对应导出：Qt6Widgets 694 个、Qt6Core 326 个、Qt6Gui 46 个、Qt6Network 14 个，缺失均为 0。这是已知构建的静态加载依赖证据；最终构建仍应重新验证。

读取 Qt 导出时应为 pefile 设置 `max_symbol_exports=65536`。默认值 8192 会截断 Qt6Widgets/Qt6Gui 的命名导出解析，导致误报尾部导出缺失。

## 7. 最小复查程序

以下脚本只读取原版 EXE，复核文件身份、三签名唯一性、函数边界及 Dispatch 复制范围。依赖 `pefile==2024.8.26`、`capstone==5.0.9`；可安装在独立的临时 Python 目录。路径可按工作区位置调整。

```python
import hashlib
from pathlib import Path
import pefile
import capstone

path = Path(r"D:\奥奇工程\氪奇Pro-V1.1.4\KQProV1.1.4.exe")
assert hashlib.sha256(path.read_bytes()).hexdigest().upper() == (
    "8B4230AF89224BAF924FE16019DFA31F7A2A694E750B2E46B7785F3CAA1A356B"
)
pe = pefile.PE(str(path), max_symbol_exports=65536)
endpoints = {
    "getter": (0x13FDA0, "40 53 48 83 EC 40 48 8B 05 9B F2 11 00 48 85 C0"),
    "dispatch": (0x140270, "48 89 5C 24 10 55 56 57 41 56 41 57 48 8D 6C 24 D1"),
    "sender": (0x140DC0, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 "
                         "48 8D 6C 24 D9 48 81 EC 00 01 00 00"),
}
for name, (rva, hex_bytes) in endpoints.items():
    signature = bytes.fromhex(hex_bytes)
    assert pe.get_data(rva, len(signature)) == signature
    matches = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        data, start = section.get_data(), 0
        while (offset := data.find(signature, start)) >= 0:
            matches.append(section.VirtualAddress + offset)
            start = offset + 1
    assert matches == [rva], (name, matches)
    entry = next(e.struct for e in pe.DIRECTORY_ENTRY_EXCEPTION
                 if e.struct.BeginAddress == rva)
    print(name, hex(rva), len(signature), "unique", hex(entry.EndAddress))

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
decoder.detail = True
prologue = pe.get_data(0x140270, 17)
instructions = list(decoder.disasm(prologue, 0x140270))
assert sum(i.size for i in instructions) == 17
for instruction in instructions:
    assert not instruction.group(capstone.CS_GRP_JUMP)
    assert not instruction.group(capstone.CS_GRP_CALL)
    assert not any(op.type == capstone.CS_OP_MEM and
                   op.mem.base == capstone.x86.X86_REG_RIP
                   for op in instruction.operands)
    print(hex(instruction.address), instruction.mnemonic, instruction.op_str)
```

在线验证应另行记录登录后的原版页面是否正常、Hook 兼容报告是否通过、背包/仓库读取是否返回真实数据。本次只读静态分析未执行这些账号内操作。

## 8. 重新构建 DLL 的静态依赖复核

2026-09-09 对当时重新构建的 `build-qt663/bin/Release/KQPetInventory.dll` 再次进行了只读 PE 检查。本次构建共 1343 个直接命名导入、0 个序号导入、0 个延迟导入，全部可由 V1.1.4 同目录的运行库或本机 Windows System32 库解析，无新增缺失。

| 导入库 | 命名导入数 | 解析来源 | 缺失数 |
|---|---:|---|---:|
| Qt6Widgets.dll | 735 | V1.1.4 原版目录 | 0 |
| Qt6Gui.dll | 51 | V1.1.4 原版目录 | 0 |
| Qt6Network.dll | 14 | V1.1.4 原版目录 | 0 |
| Qt6Core.dll | 428 | V1.1.4 原版目录 | 0 |
| MSVCP140.dll | 51 | V1.1.4 原版目录 | 0 |
| VCRUNTIME140.dll | 12 | V1.1.4 原版目录 | 0 |
| VCRUNTIME140_1.dll | 1 | V1.1.4 原版目录 | 0 |
| VERSION.dll | 3 | Windows System32 | 0 |
| KERNEL32.dll | 28 | Windows System32 | 0 |
| USER32.dll | 1 | Windows System32 | 0 |
| api-ms-win-crt-string-l1-1-0.dll | 5 | API-set → System32/ucrtbase.dll | 0 |
| api-ms-win-crt-heap-l1-1-0.dll | 3 | API-set → System32/ucrtbase.dll | 0 |
| api-ms-win-crt-runtime-l1-1-0.dll | 11 | API-set → System32/ucrtbase.dll | 0 |

检查还沿 5 条 KERNEL32 导出转发链解析到最终实现，检查了最终目标符号及所有读取到的导出镜像的 AMD64 架构。系统库选择读取本机 KnownDLLs；API-set 使用本机 `System32/apisetschema.dll` 的版本 6 名称表、默认 host 与 HashedLength 键。`SleepConditionVariableSRW` 的旧 revision 合约转发可经同一 schema 键解析到 `kernelbase.dll`，未误当成缺失的独立 DLL 文件。

这次检查没有调用 `LoadLibrary` 或运行扩展，也没有递归审计所有 Qt/系统 DLL 自身的完整依赖图；覆盖的是扩展的全部直接导入及其导出转发链。此处记录兼容依赖检查结果，不绑定构建产物哈希，也不自动涵盖后续构建。

复查脚本保存在本机临时目录：`%TEMP%/kqpet-verify-final-dependencies.py`；机器可读输出为 `%TEMP%/kqpet-final-dependency-report.json`。使用 Python 3.14 和此前安装在 `%TEMP%/kqpet-binary-analysis` 的 pefile，可在构建更新后重新执行：

```powershell
python "$env:TEMP\kqpet-verify-final-dependencies.py" `
  --extension 'D:\奥奇工程\KQPetInventoryExtension\build-qt663\bin\Release\KQPetInventory.dll' `
  --target-dir 'D:\奥奇工程\氪奇Pro-V1.1.4'
```
