#!/usr/bin/env python3
"""
G-code边界超限检查工具 - GUI版本
G-code Boundary Violation Checker - GUI Version

带有图形界面的G-code边界检测工具
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import re
import math
from enum import Enum
from dataclasses import dataclass
from typing import List, Tuple
import threading
from pathlib import Path


class BedType(Enum):
    RECTANGLE = "矩形床 (Rectangle)"
    CIRCLE = "圆形床 (Circle)"


class MoveType(Enum):
    TRAVEL = "Travel"
    EXTRUDE = "Extrude"
    ARC_CW = "Arc CW (G2)"        # 顺时针弧线
    ARC_CCW = "Arc CCW (G3)"      # 逆时针弧线
    RETRACT = "Retract"
    UNKNOWN = "Unknown"


class ViolationType(Enum):
    X_MIN = "X < 最小值"
    X_MAX = "X > 最大值"
    Y_MIN = "Y < 最小值"
    Y_MAX = "Y > 最大值"
    Z_MAX = "Z > 最大值"
    RADIUS = "半径超限 (圆形床)"


@dataclass
class Position:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    e: float = 0.0

    def copy(self):
        return Position(self.x, self.y, self.z, self.e)


@dataclass
class Violation:
    line_num: int
    line_content: str
    position: Position
    move_type: MoveType
    violation_types: List[ViolationType]
    distance_out: float

    def __str__(self):
        vio_str = ", ".join([v.value for v in self.violation_types])
        return (f"行 {self.line_num}: {self.move_type.value} - {vio_str}\n"
                f"  位置: X={self.position.x:.3f} Y={self.position.y:.3f} "
                f"Z={self.position.z:.3f} E={self.position.e:.3f}\n"
                f"  超出: {self.distance_out:.3f} mm\n"
                f"  代码: {self.line_content.strip()}")


class GCodeAnalyzer:
    def __init__(self, bed_type: BedType, bed_min: Tuple[float, float],
                 bed_max: Tuple[float, float], max_z: float, radius: float = None,
                 progress_callback=None):
        self.bed_type = bed_type
        self.bed_min = bed_min
        self.bed_max = bed_max
        self.max_z = max_z
        self.radius = radius
        self.center = ((bed_max[0] + bed_min[0]) / 2,
                      (bed_max[1] + bed_min[1]) / 2) if bed_type == BedType.CIRCLE else None
        self.progress_callback = progress_callback

        self.current_pos = Position()
        self.violations: List[Violation] = []
        self.total_moves = 0
        self.travel_moves = 0
        self.extrude_moves = 0
        self.total_lines = 0

    def parse_gcode_file(self, filename: str):
        """解析G-code文件"""
        try:
            # 先计算总行数
            with open(filename, 'r', encoding='utf-8') as f:
                self.total_lines = sum(1 for _ in f)

            # 解析文件
            with open(filename, 'r', encoding='utf-8') as f:
                for line_num, line in enumerate(f, 1):
                    self._parse_line(line_num, line)

                    # 更新进度
                    if self.progress_callback and line_num % 100 == 0:
                        progress = (line_num / self.total_lines) * 100
                        self.progress_callback(progress, line_num, self.total_lines)

            return True
        except Exception as e:
            return str(e)

    def _parse_line(self, line_num: int, line: str):
        """解析单行G-code"""
        if ';' in line:
            code_part = line[:line.index(';')]
        else:
            code_part = line

        code_part = code_part.strip().upper()
        if not code_part:
            return

        # Check for G0/G1/G2/G3 commands (using word boundary to avoid matching G28, G29, etc.)
        g_match = re.match(r'G([0-3])\b', code_part)
        if not g_match:
            return

        g_code = int(g_match.group(1))

        # Parse coordinates
        x_match = re.search(r'X([-+]?\d*\.?\d+)', code_part)
        y_match = re.search(r'Y([-+]?\d*\.?\d+)', code_part)
        z_match = re.search(r'Z([-+]?\d*\.?\d+)', code_part)
        e_match = re.search(r'E([-+]?\d*\.?\d+)', code_part)
        i_match = re.search(r'I([-+]?\d*\.?\d+)', code_part)
        j_match = re.search(r'J([-+]?\d*\.?\d+)', code_part)

        # G2/G3 arc commands
        if g_code in [2, 3] and (i_match or j_match):
            self._parse_arc(line_num, line, code_part, g_code, x_match, y_match,
                           z_match, e_match, i_match, j_match)
            return

        # G0/G1 linear moves
        new_pos = self.current_pos.copy()
        has_move = False
        has_xy_move = False

        if x_match:
            new_pos.x = float(x_match.group(1))
            has_move = True
            has_xy_move = True
        if y_match:
            new_pos.y = float(y_match.group(1))
            has_move = True
            has_xy_move = True
        if z_match:
            new_pos.z = float(z_match.group(1))
            has_move = True
        if e_match:
            new_pos.e = float(e_match.group(1))

        if not has_move:
            return

        move_type = self._classify_move(code_part, self.current_pos, new_pos)

        if move_type == MoveType.TRAVEL:
            self.travel_moves += 1
        elif move_type == MoveType.EXTRUDE:
            self.extrude_moves += 1
        self.total_moves += 1

        # Only check XY bounds if X or Y actually moved
        if has_xy_move:
            violations = self._check_bounds(new_pos)
            if violations:
                distance = self._calculate_distance_out(new_pos)
                self.violations.append(Violation(
                    line_num=line_num,
                    line_content=line,
                    position=new_pos.copy(),
                    move_type=move_type,
                    violation_types=violations,
                    distance_out=distance
                ))

        self.current_pos = new_pos

    def _parse_arc(self, line_num: int, line: str, code_part: str, g_code: int,
                   x_match, y_match, z_match, e_match, i_match, j_match):
        """Parse G2/G3 arc commands and check arc path for boundary violations"""
        # Current position is the start of the arc
        start_x = self.current_pos.x
        start_y = self.current_pos.y
        start_z = self.current_pos.z

        # Parse I, J (offsets from start to center)
        i = float(i_match.group(1)) if i_match else 0.0
        j = float(j_match.group(1)) if j_match else 0.0

        # Calculate arc center
        center_x = start_x + i
        center_y = start_y + j
        radius = math.sqrt(i * i + j * j)

        # Parse X, Y if present (end point)
        end_x = float(x_match.group(1)) if x_match else None
        end_y = float(y_match.group(1)) if y_match else None
        end_z = float(z_match.group(1)) if z_match else start_z
        e = float(e_match.group(1)) if e_match else self.current_pos.e

        # If no X/Y specified, do a full circle (360 degrees)
        if end_x is None and end_y is None:
            # For full circle, calculate end point as start point
            end_angle = math.atan2(start_y - center_y, start_x - center_x) + (2 * math.pi if g_code == 3 else -2 * math.pi)
            end_x = center_x + radius * math.cos(end_angle)
            end_y = center_y + radius * math.sin(end_angle)
        elif end_x is None:
            end_x = start_x
        elif end_y is None:
            end_y = start_y

        # Calculate start and end angles
        start_angle = math.atan2(start_y - center_y, start_x - center_x)
        end_angle = math.atan2(end_y - center_y, end_x - center_x)

        # Determine arc direction and angle sweep
        if g_code == 2:  # Clockwise
            if end_angle > start_angle:
                end_angle -= 2 * math.pi
            angle_sweep = start_angle - end_angle
        else:  # G3: Counter-clockwise
            if end_angle < start_angle:
                end_angle += 2 * math.pi
            angle_sweep = end_angle - start_angle

        # Sample points along the arc and check each
        num_samples = max(8, int(abs(angle_sweep) * radius / 5))  # At least 8 points, or 1 per 5mm of arc length

        move_type = MoveType.ARC_CCW if g_code == 3 else MoveType.ARC_CW
        if move_type == MoveType.ARC_CW:
            self.travel_moves += 1
        else:
            self.extrude_moves += 1
        self.total_moves += 1

        # Check arc samples
        for n in range(num_samples + 1):
            t = n / num_samples
            angle = start_angle + (angle_sweep * t if g_code == 3 else -angle_sweep * t)

            sample_x = center_x + radius * math.cos(angle)
            sample_y = center_y + radius * math.sin(angle)
            sample_z = start_z + (end_z - start_z) * t  # Interpolate Z

            # Check this point
            sample_pos = Position(sample_x, sample_y, sample_z, e)
            violations = self._check_bounds(sample_pos)

            if violations:
                distance = self._calculate_distance_out(sample_pos)
                self.violations.append(Violation(
                    line_num=line_num,
                    line_content=line,
                    position=sample_pos,
                    move_type=move_type,
                    violation_types=violations,
                    distance_out=distance
                ))
                break  # Only record first violation on this arc

        # Update current position to arc end
        self.current_pos.x = end_x
        self.current_pos.y = end_y
        self.current_pos.z = end_z
        self.current_pos.e = e

    def _classify_move(self, code: str, old_pos: Position, new_pos: Position) -> MoveType:
        if code.startswith('G0'):
            return MoveType.TRAVEL
        if code.startswith('G1'):
            if abs(new_pos.e - old_pos.e) > 0.001:
                return MoveType.EXTRUDE
            else:
                return MoveType.TRAVEL
        if code.startswith('G2') or code.startswith('G3'):
            return MoveType.EXTRUDE
        return MoveType.UNKNOWN

    def _check_bounds(self, pos: Position) -> List[ViolationType]:
        violations = []
        epsilon = 0.01

        if self.bed_type == BedType.RECTANGLE:
            if pos.x < self.bed_min[0] - epsilon:
                violations.append(ViolationType.X_MIN)
            if pos.x > self.bed_max[0] + epsilon:
                violations.append(ViolationType.X_MAX)
            if pos.y < self.bed_min[1] - epsilon:
                violations.append(ViolationType.Y_MIN)
            if pos.y > self.bed_max[1] + epsilon:
                violations.append(ViolationType.Y_MAX)
        elif self.bed_type == BedType.CIRCLE:
            dist = math.sqrt((pos.x - self.center[0])**2 + (pos.y - self.center[1])**2)
            if dist > self.radius + epsilon:
                violations.append(ViolationType.RADIUS)

        if self.max_z > 0 and pos.z > self.max_z + epsilon:
            violations.append(ViolationType.Z_MAX)

        return violations

    def _calculate_distance_out(self, pos: Position) -> float:
        if self.bed_type == BedType.RECTANGLE:
            dx = max(0, self.bed_min[0] - pos.x, pos.x - self.bed_max[0])
            dy = max(0, self.bed_min[1] - pos.y, pos.y - self.bed_max[1])
            dz = max(0, pos.z - self.max_z) if self.max_z > 0 else 0
            return math.sqrt(dx**2 + dy**2 + dz**2)
        elif self.bed_type == BedType.CIRCLE:
            dist = math.sqrt((pos.x - self.center[0])**2 + (pos.y - self.center[1])**2)
            return max(0, dist - self.radius)
        return 0.0

    def get_report(self) -> str:
        """生成报告"""
        report = []
        report.append("=" * 70)
        report.append("G-code边界超限分析报告")
        report.append("=" * 70)
        report.append("")

        # 床信息
        if self.bed_type == BedType.RECTANGLE:
            report.append(f"床类型: 矩形")
            report.append(f"床边界: X[{self.bed_min[0]:.1f}, {self.bed_max[0]:.1f}] "
                         f"Y[{self.bed_min[1]:.1f}, {self.bed_max[1]:.1f}] "
                         f"Z[0, {self.max_z:.1f}]")
        else:
            report.append(f"床类型: 圆形")
            report.append(f"床中心: ({self.center[0]:.1f}, {self.center[1]:.1f})")
            report.append(f"床半径: {self.radius:.1f} mm, Z[0, {self.max_z:.1f}]")

        report.append("")
        report.append(f"总行数: {self.total_lines}")
        report.append(f"总移动数: {self.total_moves}")
        report.append(f"  - Travel移动: {self.travel_moves}")
        report.append(f"  - Extrude移动: {self.extrude_moves}")
        report.append("")

        # 超限统计
        report.append(f"发现超限: {len(self.violations)} 处")

        if not self.violations:
            report.append("")
            report.append("✅ 所有移动都在边界内！")
            return "\n".join(report)

        travel_violations = [v for v in self.violations if v.move_type == MoveType.TRAVEL]
        extrude_violations = [v for v in self.violations if v.move_type == MoveType.EXTRUDE]

        report.append(f"  - Travel超限: {len(travel_violations)}")
        report.append(f"  - Extrude超限: {len(extrude_violations)}")
        report.append("")

        # 超限类型统计
        from collections import Counter
        all_vio_types = []
        for v in self.violations:
            all_vio_types.extend(v.violation_types)
        vio_counter = Counter(all_vio_types)

        report.append("超限类型统计:")
        for vio_type, count in vio_counter.most_common():
            report.append(f"  {vio_type.value}: {count} 次")
        report.append("")

        # 详细列表（前100个）
        report.append("=" * 70)
        report.append(f"详细超限列表 (前100个):")
        report.append("=" * 70)
        report.append("")

        for i, violation in enumerate(self.violations[:100], 1):
            report.append(f"[{i}] {violation}")
            report.append("")

        if len(self.violations) > 100:
            report.append(f"... 还有 {len(self.violations) - 100} 个超限未显示")

        return "\n".join(report)


class GCodeBoundaryCheckerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("G-code边界超限检查工具")
        self.root.geometry("900x700")

        # 变量
        self.gcode_file = tk.StringVar()
        self.bed_type = tk.StringVar(value="rectangle")
        self.bed_x = tk.DoubleVar(value=200.0)
        self.bed_y = tk.DoubleVar(value=200.0)
        self.bed_z = tk.DoubleVar(value=250.0)
        self.bed_radius = tk.DoubleVar(value=100.0)
        self.origin_x = tk.DoubleVar(value=0.0)
        self.origin_y = tk.DoubleVar(value=0.0)

        self.analyzer = None
        self.analyzing = False

        self.create_widgets()

    def create_widgets(self):
        # 主框架
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(0, weight=1)
        main_frame.rowconfigure(4, weight=1)

        # 1. 文件选择
        file_frame = ttk.LabelFrame(main_frame, text="1. 选择G-code文件", padding="10")
        file_frame.grid(row=0, column=0, sticky=(tk.W, tk.E), pady=5)
        file_frame.columnconfigure(1, weight=1)

        ttk.Entry(file_frame, textvariable=self.gcode_file, width=50).grid(
            row=0, column=0, sticky=(tk.W, tk.E), padx=5)
        ttk.Button(file_frame, text="浏览...", command=self.browse_file).grid(
            row=0, column=1, padx=5)

        # 2. 床参数
        bed_frame = ttk.LabelFrame(main_frame, text="2. 床参数配置", padding="10")
        bed_frame.grid(row=1, column=0, sticky=(tk.W, tk.E), pady=5)

        # 床类型选择
        type_frame = ttk.Frame(bed_frame)
        type_frame.grid(row=0, column=0, columnspan=3, sticky=tk.W, pady=5)

        ttk.Label(type_frame, text="床类型:").pack(side=tk.LEFT, padx=5)
        ttk.Radiobutton(type_frame, text="矩形床", variable=self.bed_type,
                       value="rectangle", command=self.update_bed_type).pack(side=tk.LEFT, padx=5)
        ttk.Radiobutton(type_frame, text="圆形床 (Delta)", variable=self.bed_type,
                       value="circle", command=self.update_bed_type).pack(side=tk.LEFT, padx=5)

        # 矩形床参数
        self.rect_frame = ttk.Frame(bed_frame)
        self.rect_frame.grid(row=1, column=0, columnspan=3, sticky=tk.W, pady=5)

        ttk.Label(self.rect_frame, text="尺寸 (mm):").grid(row=0, column=0, padx=5)
        ttk.Label(self.rect_frame, text="X:").grid(row=0, column=1)
        ttk.Entry(self.rect_frame, textvariable=self.bed_x, width=10).grid(row=0, column=2, padx=2)
        ttk.Label(self.rect_frame, text="Y:").grid(row=0, column=3)
        ttk.Entry(self.rect_frame, textvariable=self.bed_y, width=10).grid(row=0, column=4, padx=2)
        ttk.Label(self.rect_frame, text="Z:").grid(row=0, column=5)
        ttk.Entry(self.rect_frame, textvariable=self.bed_z, width=10).grid(row=0, column=6, padx=2)

        ttk.Label(self.rect_frame, text="原点:").grid(row=1, column=0, padx=5, pady=5)
        ttk.Label(self.rect_frame, text="X:").grid(row=1, column=1)
        ttk.Entry(self.rect_frame, textvariable=self.origin_x, width=10).grid(row=1, column=2, padx=2)
        ttk.Label(self.rect_frame, text="Y:").grid(row=1, column=3)
        ttk.Entry(self.rect_frame, textvariable=self.origin_y, width=10).grid(row=1, column=4, padx=2)

        # 圆形床参数
        self.circle_frame = ttk.Frame(bed_frame)
        self.circle_frame.grid(row=1, column=0, columnspan=3, sticky=tk.W, pady=5)

        ttk.Label(self.circle_frame, text="半径 (mm):").grid(row=0, column=0, padx=5)
        ttk.Entry(self.circle_frame, textvariable=self.bed_radius, width=10).grid(row=0, column=1, padx=2)
        ttk.Label(self.circle_frame, text="Z高度:").grid(row=0, column=2, padx=5)
        ttk.Entry(self.circle_frame, textvariable=self.bed_z, width=10).grid(row=0, column=3, padx=2)

        self.circle_frame.grid_remove()  # 初始隐藏

        # 快速预设
        preset_frame = ttk.Frame(bed_frame)
        preset_frame.grid(row=2, column=0, columnspan=3, sticky=tk.W, pady=5)

        ttk.Label(preset_frame, text="快速预设:").pack(side=tk.LEFT, padx=5)
        ttk.Button(preset_frame, text="200×200×250",
                  command=lambda: self.apply_preset(200, 200, 250)).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="220×220×250",
                  command=lambda: self.apply_preset(220, 220, 250)).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="250×250×300",
                  command=lambda: self.apply_preset(250, 250, 300)).pack(side=tk.LEFT, padx=2)
        ttk.Button(preset_frame, text="300×300×400",
                  command=lambda: self.apply_preset(300, 300, 400)).pack(side=tk.LEFT, padx=2)

        # 3. 控制按钮
        control_frame = ttk.Frame(main_frame)
        control_frame.grid(row=2, column=0, pady=10)

        self.analyze_btn = ttk.Button(control_frame, text="开始分析",
                                      command=self.start_analysis, width=15)
        self.analyze_btn.pack(side=tk.LEFT, padx=5)

        self.save_btn = ttk.Button(control_frame, text="保存报告",
                                   command=self.save_report, width=15, state=tk.DISABLED)
        self.save_btn.pack(side=tk.LEFT, padx=5)

        ttk.Button(control_frame, text="清除结果",
                  command=self.clear_results, width=15).pack(side=tk.LEFT, padx=5)

        # 4. 进度条
        self.progress_var = tk.DoubleVar()
        self.progress_label = ttk.Label(main_frame, text="")
        self.progress_label.grid(row=3, column=0, sticky=tk.W)

        self.progress_bar = ttk.Progressbar(main_frame, variable=self.progress_var,
                                           maximum=100, mode='determinate')
        self.progress_bar.grid(row=3, column=0, sticky=(tk.W, tk.E), pady=5)

        # 5. 结果显示
        result_frame = ttk.LabelFrame(main_frame, text="分析结果", padding="10")
        result_frame.grid(row=4, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        result_frame.columnconfigure(0, weight=1)
        result_frame.rowconfigure(0, weight=1)

        self.result_text = scrolledtext.ScrolledText(result_frame, width=80, height=20,
                                                     font=('Courier New', 9))
        self.result_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

    def browse_file(self):
        filename = filedialog.askopenfilename(
            title="选择G-code文件",
            filetypes=[("G-code文件", "*.gcode *.GCODE *.gco *.GCO"),
                      ("所有文件", "*.*")]
        )
        if filename:
            self.gcode_file.set(filename)

    def update_bed_type(self):
        if self.bed_type.get() == "rectangle":
            self.rect_frame.grid()
            self.circle_frame.grid_remove()
        else:
            self.rect_frame.grid_remove()
            self.circle_frame.grid()

    def apply_preset(self, x, y, z):
        self.bed_x.set(x)
        self.bed_y.set(y)
        self.bed_z.set(z)
        self.bed_type.set("rectangle")
        self.update_bed_type()

    def update_progress(self, progress, current, total):
        self.progress_var.set(progress)
        self.progress_label.config(text=f"正在分析... {current}/{total} 行 ({progress:.1f}%)")

    def start_analysis(self):
        # 验证输入
        if not self.gcode_file.get():
            messagebox.showerror("错误", "请选择G-code文件")
            return

        if not Path(self.gcode_file.get()).exists():
            messagebox.showerror("错误", "文件不存在")
            return

        # 禁用按钮
        self.analyze_btn.config(state=tk.DISABLED)
        self.save_btn.config(state=tk.DISABLED)
        self.result_text.delete(1.0, tk.END)
        self.progress_var.set(0)

        # 在后台线程中分析
        thread = threading.Thread(target=self.run_analysis)
        thread.daemon = True
        thread.start()

    def run_analysis(self):
        try:
            # 准备参数
            if self.bed_type.get() == "rectangle":
                bed_type = BedType.RECTANGLE
                bed_min = (self.origin_x.get(), self.origin_y.get())
                bed_max = (self.origin_x.get() + self.bed_x.get(),
                          self.origin_y.get() + self.bed_y.get())
                max_z = self.bed_z.get()
                radius = None
            else:
                bed_type = BedType.CIRCLE
                radius = self.bed_radius.get()
                bed_min = (-radius, -radius)
                bed_max = (radius, radius)
                max_z = self.bed_z.get()

            # 创建分析器
            self.analyzer = GCodeAnalyzer(bed_type, bed_min, bed_max, max_z, radius,
                                         progress_callback=self.update_progress)

            # 分析文件
            result = self.analyzer.parse_gcode_file(self.gcode_file.get())

            if result is not True:
                self.root.after(0, lambda: messagebox.showerror("错误", f"分析失败: {result}"))
                self.root.after(0, lambda: self.analyze_btn.config(state=tk.NORMAL))
                return

            # 生成报告
            report = self.analyzer.get_report()

            # 更新UI
            self.root.after(0, lambda: self.display_results(report))

        except Exception as e:
            self.root.after(0, lambda: messagebox.showerror("错误", f"分析出错: {str(e)}"))
            self.root.after(0, lambda: self.analyze_btn.config(state=tk.NORMAL))

    def display_results(self, report):
        self.result_text.delete(1.0, tk.END)
        self.result_text.insert(1.0, report)

        # 高亮显示
        if "✅" in report:
            self.result_text.tag_config("success", foreground="green", font=('Courier New', 9, 'bold'))
            start = self.result_text.search("✅", 1.0, tk.END)
            if start:
                end = f"{start}+1c"
                self.result_text.tag_add("success", start, end)

        self.progress_label.config(text="分析完成!")
        self.progress_var.set(100)
        self.analyze_btn.config(state=tk.NORMAL)
        self.save_btn.config(state=tk.NORMAL)

        # 如果有超限，弹出提示
        if self.analyzer and len(self.analyzer.violations) > 0:
            messagebox.showwarning("发现超限",
                                 f"发现 {len(self.analyzer.violations)} 处边界超限！\n"
                                 f"请查看详细报告。")
        else:
            messagebox.showinfo("检查完成", "✅ 所有移动都在边界内！")

    def save_report(self):
        if not self.analyzer:
            return

        filename = filedialog.asksaveasfilename(
            title="保存报告",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile="gcode_boundary_report.txt"
        )

        if filename:
            try:
                with open(filename, 'w', encoding='utf-8') as f:
                    f.write(self.result_text.get(1.0, tk.END))
                messagebox.showinfo("保存成功", f"报告已保存到:\n{filename}")
            except Exception as e:
                messagebox.showerror("保存失败", f"保存出错: {str(e)}")

    def clear_results(self):
        self.result_text.delete(1.0, tk.END)
        self.progress_var.set(0)
        self.progress_label.config(text="")
        self.analyzer = None
        self.save_btn.config(state=tk.DISABLED)


def main():
    root = tk.Tk()
    app = GCodeBoundaryCheckerGUI(root)
    root.mainloop()


if __name__ == '__main__':
    main()
