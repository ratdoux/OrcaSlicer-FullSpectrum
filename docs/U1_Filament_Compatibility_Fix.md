# U1设备耗材兼容性检查修复

## 问题描述

当连接 Snapmaker U1 设备时，如果设备上编辑的耗材是 "Snapmaker PETG"，耗材下拉框的 "Machine Filament" 部分会显示该耗材。然而，`Snapmaker PETG.json` 配置文件的 `compatible_printers` 列表中**不包含** U1 设备（仅包含 A250/A350 系列），导致显示了一个不兼容的耗材配置。

### 现象

- 引导页和选择耗材页**没有** "Snapmaker PETG @U1" 选项（正常）
- 连接 U1 设备后，下拉框 "Machine Filament" 部分**有** "Snapmaker PETG"（问题）
- 但 `Snapmaker PETG` 的 `compatible_printers` 不包含 U1，配置不匹配

## 根本原因

### 代码位置

`src/slic3r/GUI/PresetComboBoxes.cpp`

- 第一处：第 1151-1164 行（`PlaterPresetComboBox::update()` 函数）
- 第二处：第 1725-1738 行（另一个同名函数，可能是不同类）

### 问题代码

```cpp
// 修改前的代码
for (auto iter = machine_filaments.begin(); iter != machine_filaments.end();) {
    std::string filament_name = iter->second.first;
    // 1. 精确匹配名称
    auto item_iter = std::find_if(filaments.begin(), filaments.end(),
                            [&filament_name, this](auto& f) { return f.name == filament_name; });

    // 2. 如果没找到，尝试带 @U1 后缀
    if (item_iter == filaments.end()) {
        item_iter = std::find_if(filaments.begin(), filaments.end(),
                            [&filament_name, this](auto& f) { return f.name == filament_name + " @U1"; });
    }

    // 3. 只要找到就添加，没有检查 is_compatible！
    if (item_iter != filaments.end()) {
        const_cast<Preset&>(*item_iter).is_visible = true;
        // ... 添加到下拉框
    }
}
```

### 缺少的检查

代码只检查了耗材名称是否匹配，**没有检查** `is_compatible` 标志。这个标志是由 `PresetCollection::update_compatible_internal()` 函数根据 `compatible_printers` 配置计算的。

### 耗材同步流程

1. 设备连接时，通过 `SSWCP_Instance::update_filament_info()`（SSWCP.cpp:1440-1588）同步耗材
2. 设备发送：`filament_vendor="Snapmaker"`, `filament_type="PETG"`, `filament_sub_type="NONE"`
3. 构建耗材名称：`"Snapmaker PETG"`（**不带 @U1 后缀**）
4. 存储到 `wxGetApp().preset_bundle->machine_filaments`
5. 下拉框更新时尝试匹配此名称

## 修复方案

### 修改内容

在两次匹配的 lambda 表达式中都增加 `&& f.is_compatible` 检查：

```cpp
// 修改后的代码
for (auto iter = machine_filaments.begin(); iter != machine_filaments.end();) {
    std::string filament_name = iter->second.first;

    // 第一次匹配: 精确匹配名称 + 兼容性检查
    auto item_iter = std::find_if(filaments.begin(), filaments.end(),
                             [&filament_name, this](auto& f) { return f.name == filament_name && f.is_compatible; });

    // 第二次匹配: 如果第一次失败，尝试带 @U1 后缀 + 兼容性检查
    if (item_iter == filaments.end()) {
        item_iter = std::find_if(filaments.begin(), filaments.end(),
                            [&filament_name, this](auto& f) { return f.name == filament_name + " @U1" && f.is_compatible; });
    }

    if (item_iter != filaments.end()) {
        const_cast<Preset&>(*item_iter).is_visible = true;
        // ... 添加到下拉框
    } else {
        iter = machine_filaments.erase(iter);
    }
}
```

### 修复逻辑

1. **第一次匹配**：精确匹配 "Snapmaker PETG" + 检查 `is_compatible`
   - 如果 `Snapmaker PETG.json` 的 `compatible_printers` 不包含 U1
   - 则 `is_compatible = false`，匹配失败

2. **第二次匹配**：尝试 "Snapmaker PETG @U1" + 检查 `is_compatible`
   - 如果在 Snapmaker.json 中注册了 "Snapmaker PETG @U1"
   - 且其 `compatible_printers` 包含 U1
   - 则匹配成功

3. **结果**：只有**名称匹配且兼容**的耗材才会显示

## 技术细节

### is_compatible 计算逻辑

位置：`src/libslic3r/Preset.cpp:639-670`

```cpp
bool is_compatible_with_printer(const PresetWithVendorProfile &preset,
                                const PresetWithVendorProfile &active_printer,
                                const DynamicPrintConfig *extra_config)
{
    auto *compatible_printers = preset.preset.config.option("compatible_printers");
    bool has_compatible_printers = compatible_printers != nullptr &&
                                   !compatible_printers->values.empty();

    return preset.preset.is_default ||
           active_printer.preset.name.empty() ||
           !has_compatible_printers ||
           std::find(compatible_printers->values.begin(),
                     compatible_printers->values.end(),
                     active_printer.preset.name) != compatible_printers->values.end();
}
```

### 下拉框填充顺序

`PlaterPresetComboBox::update()` 函数按以下顺序填充下拉框：

1. **Machine Filament** - 从设备同步的耗材（本次修复部分）
2. **Project-inside presets** - 项目内嵌预设
3. **User presets** - 用户自定义预设
4. **System presets** - 系统预设（本来就检查 `is_compatible`，第 1073 行）

### 两处修改的原因

在 `PresetComboBoxes.cpp` 中有两处几乎相同的代码，可能属于不同的类或重载函数：

- 第 1151-1164 行：`PlaterPresetComboBox::update()`
- 第 1725-1738 行：另一个相同的逻辑（可能是不同 UI 组件）

两处都需要修改以确保一致性。

## 相关配置文件

### Snapmaker PETG.json

位置：`resources/profiles/Snapmaker/filament/Snapmaker PETG.json`

```json
{
  "type": "filament",
  "name": "Snapmaker PETG",
  "compatible_printers": [
    "Snapmaker A250 (0.4 nozzle)",
    "Snapmaker A250 (0.6 nozzle)",
    ...
    "Snapmaker A350 QSKit (0.8 nozzle)"
  ]
}
```

**不包含 U1 设备**

### Snapmaker PETG @U1.json

位置：`resources/profiles/Snapmaker/filament/Snapmaker PETG @U1.json`

```json
{
  "type": "filament",
  "name": "Snapmaker PETG @U1",
  "compatible_printers": [
    "Snapmaker U1 (0.4 nozzle)"
  ]
}
```

**文件存在但未在 Snapmaker.json 中注册**

## 后续建议

要让 U1 设备正确识别 Snapmaker PETG 耗材，需要以下操作之一：

### 选项 A：注册 @U1 专用配置（推荐）

在 `resources/profiles/Snapmaker.json` 的 `filament_list` 中添加：

```json
{
  "name": "Snapmaker PETG @U1",
  "sub_path": "filament/Snapmaker PETG @U1.json"
}
```

### 选项 B：扩展通用配置的兼容性

在 `resources/profiles/Snapmaker/filament/Snapmaker PETG.json` 的 `compatible_printers` 中添加：

```json
"compatible_printers": [
  ...existing...,
  "Snapmaker U1 (0.4 nozzle)"
]
```

## 测试建议

1. **测试不兼容耗材不显示**：
   - 连接 U1 设备
   - 确认下拉框 "Machine Filament" 部分不显示 "Snapmaker PETG"（如果不兼容）

2. **测试兼容耗材正常显示**：
   - 在 Snapmaker.json 中注册 "Snapmaker PETG @U1"
   - 连接 U1 设备
   - 确认下拉框正确显示 "Snapmaker PETG @U1"

3. **测试其他耗材不受影响**：
   - 确认其他已注册的 U1 兼容耗材（如 "Snapmaker ABS @U1"）正常显示

## 修改文件

- `src/slic3r/GUI/PresetComboBoxes.cpp`（两处修改）

## 修改日期

2026-01-22
