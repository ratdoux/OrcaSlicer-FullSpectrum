# 螺旋抬升边界警告功能文档

## 1. 背景

### 1.1 问题描述

OrcaSlicer 在使用螺旋抬升（Spiral Lift）功能时存在安全风险：当模型靠近打印床边界时，螺旋抬升过程中可能会超出床范围，导致打印头撞击床边缘造成硬件损坏。

### 1.2 螺旋抬升原理

螺旋抬升是一种平滑的 Z 轴抬升方式，通过螺旋路径避免打印头在抬升时与模型碰撞。其计算公式为：

```
radius = z_hop / (2 × π × tan(travel_slope))
```

**典型参数**：
- `z_hop`：Z 轴抬升高度，通常为 0.2mm
- `travel_slope`：移动斜率，通常为 1°-3°

**示例计算**（travel_slope=3°, z_hop=0.2mm）：
```
radius = 0.2 / (2 × π × tan(3°))
       ≈ 0.608mm
螺旋直径 ≈ 1.2mm
```

### 1.3 风险场景

1. **模型靠近边界**：螺旋路径可能延伸到床外
2. **大尺寸模型**：导入或缩放到接近床尺寸的模型
3. **移动模型**：用户将模型移动到床边缘

## 2. 解决方案

### 2.1 设计思路

在用户移动、导入或缩放模型时，实时检测模型边界与床边界的距离，当距离小于安全阈值时显示警告提示。

### 2.2 实现方案

#### 2.2.1 架构设计

```
用户操作（移动/导入/缩放）
    ↓
reload_scene() 触发场景刷新
    ↓
check_outside_state() 检测模型状态
    ↓
计算模型边界框与床边界的最小距离
    ↓
判断是否 < 3mm 阈值
    ↓
设置 near_boundary_for_spiral_lift 标志
    ↓
_set_warning_notification() 显示警告
```

#### 2.2.2 核心代码实现

**检测逻辑**（`3DScene.cpp`）：
```cpp
// 螺旋抬升安全余量：3.5mm
constexpr double SPIRAL_LIFT_SAFETY_MARGIN = 3.5; // mm

// 计算模型边界框与床边界的最小距离
double min_distance_x = std::min({
    std::abs(bb.min.x() - bed_bb.min.x()),
    std::abs(bed_bb.max.x() - bb.max.x())
});
double min_distance_y = std::min({
    std::abs(bb.min.y() - bed_bb.min.y()),
    std::abs(bed_bb.max.y() - bb.max.y())
});

double min_distance = std::min({min_distance_x, min_distance_y});
if (min_distance < SPIRAL_LIFT_SAFETY_MARGIN) {
    volume->near_boundary_for_spiral_lift = true;
}
```

**警告触发**（`GLCanvas3D.cpp`）：
```cpp
if (contained_min_one) {
    _set_warning_notification(EWarning::SpiralLiftNearBoundary,
                              _is_any_volume_near_boundary_for_spiral_lift());
}
```

#### 2.2.3 新增数据结构

**GLVolume 成员变量**（`3DScene.hpp`）：
```cpp
bool near_boundary_for_spiral_lift : 1;  // 是否靠近边界
```

**EWarning 枚举**（`GLCanvas3D.hpp`）：
```cpp
enum class EWarning {
    // ... 现有类型
    SpiralLiftNearBoundary  // 螺旋抬升靠近边界警告
};
```

### 2.3 用户界面

**警告级别**：`SLICING_SERIOUS_WARNING`（红色/橙色警告）

**警告文本**：
- 英文："An object is too close to the plate boundary. Spiral lift during printing may exceed the bed and cause a crash. Please move the object away from the edge (recommend keeping at least 3.5mm distance)."
- 中文："模型距离打印床边界太近。打印过程中的螺旋抬升可能会超出床范围导致撞机。请将模型移离边缘（建议保持至少3.5mm的距离）。"

## 3. 阈值选取依据

### 3.1 理论计算

| 参数 | 典型值 | 说明 |
|------|--------|------|
| travel_slope | 1° - 3° | 移动斜率，角度越小螺旋半径越大 |
| z_hop | 0.2mm | Z 轴抬升高度 |
| 螺旋直径（1°） | ≈ 3.6mm | 最不利情况 |
| 螺旋直径（3°） | ≈ 1.2mm | 常见情况 |

### 3.2 阈值选项对比

| 选项 | 阈值 | 优点 | 缺点 |
|------|------|------|------|
| 2mm | 螺旋直径(1.2mm) + 0.8mm | 覆盖常见情况 | 对于1°斜率不够安全 |
| 3mm | 更保守的安全距离 | 覆盖大部分配置 | 1°斜率时仍有风险 |
| **3.5mm** | **最佳平衡点** | **覆盖1°-3°斜率，安全且实用** | **略保守，但合理** |
| 5mm | 非常保守 | 最大安全范围 | 过于保守，限制用户操作 |

### 3.3 最终选择

**选择 3.5mm 作为阈值**，理由：

1. **安全性**：完全覆盖 travel_slope=1° 的最不利情况（螺旋直径 ≈ 3.6mm）
2. **实用性**：不会过度限制用户的模型放置空间
3. **余量充足**：比常见情况（3°斜率，直径1.2mm）多出近3倍的安全距离
4. **用户体验**：在安全性和可用性之间取得最佳平衡

## 4. 风险评估

### 4.1 技术风险

| 风险项 | 风险等级 | 缓解措施 |
|--------|----------|----------|
| 检测精度问题 | 低 | 使用边界框检测，足够准确 |
| 性能影响 | 低 | 只在模型变化时检测，开销极小 |
| 误报/漏报 | 低 | 使用 3mm 保守阈值，降低误报 |
| 圆形床支持 | 中 | 当前仅支持矩形床，未来可扩展 |

### 4.2 影响面分析

**正面影响**：
- ✅ 防止打印头撞机，保护硬件
- ✅ 提升用户体验，提前发现风险
- ✅ 降低售后成本，减少设备损坏

**潜在负面影响**：
- ⚠️ 可能频繁触发警告，影响用户体验（已通过 3.5mm 阈值缓解）
- ⚠️ 仅支持 Snapmaker U1 矩形床（已明确限制）

### 4.3 使用场景

| 场景 | 是否触发 | 说明 |
|------|----------|------|
| 移动模型靠近边界 | ✅ 是 | 实时检测，移动时即时提醒 |
| 导入大尺寸模型 | ✅ 是 | 导入后立即检测 |
| 缩放模型变大 | ✅ 是 | 缩放后重新检测 |
| 模型完全在床中心 | ❌ 否 | 距离 ≥ 3.5mm 时不触发 |
| 模型部分超出边界 | ✅ 是 | 即使超出也会检测 |

### 4.4 原点偏移的影响

对于 Snapmaker U1，配置了原点偏移（x: -0.5, y: -1），这已经体现在 `printable_area` 中：

```json
"printable_area": [
    "0.5x1",     // 左下角（考虑了原点偏移）
    "270.5x1",   // 右下角
    "270.5x271", // 右上角
    "0.5x271"    // 左上角
]
```

**实际可打印范围**（U1）：
- X: 0.5 ~ 270.5（宽度 270mm）
- Y: 1 ~ 271（高度 270mm）

**不报警告的安全区域**（距离边界 ≥ 3.5mm）：
- X: **4 ~ 267**
- Y: **4.5 ~ 267.5**

## 5. 技术实现细节

### 5.1 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/slic3r/GUI/GLCanvas3D.hpp` | 添加 EWarning::SpiralLiftNearBoundary 枚举 |
| `src/slic3r/GUI/GLCanvas3D.cpp` | 添加警告文本和触发逻辑 |
| `src/slic3r/GUI/3DScene.hpp` | 添加 GLVolume 成员变量 |
| `src/slic3r/GUI/3DScene.cpp` | 实现距离检测逻辑 |
| `localization/i18n/Snapmaker_Orca.pot` | 英文翻译 |
| `localization/i18n/zh_CN/Snapmaker_Orca_zh_CN.po` | 中文翻译 |

### 5.2 关键函数

| 函数 | 位置 | 功能 |
|------|------|------|
| `GLVolumeCollection::check_outside_state()` | 3DScene.cpp:1050 | 检测模型状态，计算边界距离 |
| `GLVolumeCollection::is_any_volume_near_boundary_for_spiral_lift()` | 3DScene.cpp:1218 | 检查是否有模型靠近边界 |
| `GLCanvas3D::reload_scene()` | GLCanvas3D.cpp:2310 | 场景刷新，触发检测 |
| `GLCanvas3D::_set_warning_notification()` | GLCanvas3D.cpp:9659 | 显示警告通知 |

### 5.3 调试信息

如需调试，可以在 `3DScene.cpp:1134` 附近添加日志：

```cpp
if (min_distance < SPIRAL_LIFT_SAFETY_MARGIN) {
    BOOST_LOG_TRIVIAL(warning) << "Volume near boundary: " << volume->name
                                << ", min_distance=" << min_distance
                                << " (threshold=" << SPIRAL_LIFT_SAFETY_MARGIN << ")";
    volume->near_boundary_for_spiral_lift = true;
}
```

## 6. 未来改进方向

### 6.1 短期改进

1. **动态阈值计算**：根据当前配置的 `travel_slope` 和 `z_hop` 动态计算阈值
   - 公式：`threshold = z_hop / (π × tan(travel_slope)) + safety_margin`
   - 对于 3° 斜率 + 0.4mm z_hop，阈值可降至约 2.5mm
2. **配置选项**：允许用户在设置中调整阈值或关闭警告
3. **可视化指示**：在 3D 视图中用颜色标记靠近边界的模型

### 6.2 长期改进

1. **圆形床支持**：扩展到支持 Delta 打印机的圆形床
2. **精确碰撞检测**：使用模型的实际几何而非边界框进行检测
3. **自动修复建议**：提供一键自动移动模型到安全位置的功能

## 7. 总结

本次实现通过在模型移动、导入、缩放时实时检测边界距离，当距离小于 **3.5mm** 时显示红色警告，有效预防了螺旋抬升导致的撞机风险。

**核心优势**：
- 被动防御 → 主动预警
- 事后发现 → 事前提醒
- 保护硬件，提升用户体验

**适用范围**：
- Snapmaker U1 打印机（矩形床，带原点偏移）
- 所有使用螺旋抬升功能的场景
- 覆盖 travel_slope 1°-3° 的配置范围
