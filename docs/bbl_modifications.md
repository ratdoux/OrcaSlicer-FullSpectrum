# BBL机型显示问题修复总结

## 问题描述

清空`OrcaFilamentLibrary.json`的`filament_list`数组后，机型下拉框中看不到Bambu机型，但其他机型正常显示。

## 问题根因

1. **OrcaFilamentLibrary机制**：
   - `OrcaFilamentLibrary`总是第一个被加载（`PresetBundle.cpp:1225-1231`）
   - 其`filament_list`为空时，`m_config_maps`和`m_filament_id_maps`被设置为空
   - 这作为跨vendor继承的基础

2. **BBL.json的依赖问题**：
   - `BBL.json`的`filament_list`包含了大量第三方品牌的耗材
   - 这些品牌包括：FusRock、Polymaker、eSUN、SUNLU、Overture、AliZ等
   - 这些耗材的`@base.json`文件存储在`OrcaFilamentLibrary/filament/`目录下
   - 示例：`BBL/filament/AliZ/AliZ PA-CF @P1-X1.json` 继承自 `AliZ PA-CF @base`
   - 而 `AliZ PA-CF @base.json` 在 `OrcaFilamentLibrary/filament/AliZ/` 目录下

3. **加载失败链**：
   - 加载BBL vendor时，`AliZ PA-CF @P1-X1.json` 尝试继承 `AliZ PA-CF @base`
   - 但 `@base`文件不在BBL目录下，而在OrcaFilamentLibrary目录下
   - 由于OrcaFilamentLibrary的`filament_list`为空，这些`@base`文件没有被加载
   - 继承失败，抛出异常，整个BBL vendor加载失败
   - 结果：Bambu机型不显示

## 解决方案

### 最小风险方案（已采用）

只修改配置文件，不修改代码逻辑。

#### 修改内容

**文件：`resources/profiles/BBL.json`**

1. **清理`filament_list`**：
   - 移除所有依赖OrcaFilamentLibrary的第三方品牌耗材
   - 只保留Bambu品牌和Generic品牌的耗材
   - 从959个耗材条目减少到512个
   - 移除的品牌：FusRock、Polymaker（PolyLite、Fiberon、Panchroma等）、eSUN、SUNLU、Overture、AliZ等

2. **升级版本号**：
   - 从 `02.00.00.54` 升级到 `02.00.00.55`
   - 确保老用户升级软件后能重新加载配置

#### 保留的内容

**BBL.json的`filament_list`中保留：**
- 所有 `fdm_filament_*` 基础文件（在BBL目录下存在）
- 所有 `Bambu` 品牌的耗材
- 所有 `Generic` 品牌的耗材
- 所有 `@BBL` 结尾的耗材（BBL专用）

#### 修改影响

- **新用户**：直接使用清理后的配置，无问题
- **老用户**：升级软件后，系统检测到版本号变化，自动重新加载配置
- **功能**：用户可以选择Bambu机型，无法使用OrcaFilamentLibrary中的第三方耗材

## 技术细节

### 修改统计

```
原始：959个耗材条目
清理后：512个耗材条目
移除：447个耗材条目
```

### 关键代码位置

**PresetBundle.cpp中的相关逻辑（未修改，仅作参考）**：
- `1225-1231`：OrcaFilamentLibrary优先加载
- `2977-2996`：继承解析逻辑
- `3188-3191`：设置m_config_maps

### 日志信息

**问题出现时的错误日志**：
```
can not find inherits AliZ PA-CF @base for AliZ PA-CF @P1-X1
got error when parse filament setting from .../BBL/filament/AliZ/AliZ PA-CF @P1-X1.json
```

## 验证步骤

1. 编译代码：
   ```bash
   build_release_vs2022.bat slicer
   ```

2. 运行程序并验证：
   - 机型下拉框中可以看到Bambu机型（X1 Carbon、X1、P1P、P1S、A1 mini、A1等）
   - 耗材列表中只有Bambu和Generic品牌的耗材
   - 不再出现第三方品牌的耗材（FusRock、Polymaker等）

3. 检查日志：
   - 不再有 `can not find inherits` 相关的错误
   - BBL vendor正常加载

## 注意事项

### 版本号管理

以后修改vendor配置文件时，记得升级版本号：
- **小改动**（bug修复）：升级最后一位（如 `.54` → `.55`）
- **中等改动**（新增预设）：升级中间两位（如 `00.00` → `00.01`）
- **大改动**（结构调整）：升级前两位（如 `02.00` → `03.00`）

### OrcaFilamentLibrary

- `OrcaFilamentLibrary.json` 的 `filament_list` 保持为空
- `machine_model_list` 和 `machine_list` 也保持为空
- 这符合产品定位：不提供跨vendor的system耗材库

## 相关文件

### 修改的文件
- `resources/profiles/BBL.json`

### 未修改的文件
- `resources/profiles/OrcaFilamentLibrary.json`（保持空状态）
- `src/libslic3r/PresetBundle.cpp`（保持原样）

### 删除的文件
- `scripts/clean_bbl_json.py`（临时清理脚本，已删除）

## 总结

通过清理BBL.json中的filament_list，移除依赖OrcaFilamentLibrary的第三方耗材，实现了：
- ✅ 用户可以选择Bambu机型
- ✅ 用户无法使用OrcaFilamentLibrary中的耗材
- ✅ 最小代码修改风险（只改配置文件）
- ✅ 兼容新老用户（版本号升级）

---

**修改日期**：2026-01-22
**分支**：feature_transfer_lxy
**修改人**：Claude Code Assistant
