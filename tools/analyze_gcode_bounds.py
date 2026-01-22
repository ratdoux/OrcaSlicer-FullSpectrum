#!/usr/bin/env python3
"""
G-code边界超限分析工具
Analyzes G-code files to find moves that exceed build volume boundaries.

用法 / Usage:
    python analyze_gcode_bounds.py <gcode_file> [options]

示例 / Examples:
    python analyze_gcode_bounds.py output.gcode --bed-size 200 200 250
    python analyze_gcode_bounds.py output.gcode --bed-type circle --radius 100
"""

import re
import sys
import argparse
from enum import Enum
from dataclasses import dataclass
from typing import List, Tuple, Optional
import math


class BedType(Enum):
    RECTANGLE = "rectangle"
    CIRCLE = "circle"

class MoveType(Enum):
    TRAVEL = "Travel"
    EXTRUDE = "Extrude"
    ARC_CW = "Arc CW (G2)"        # 顺时针弧线
    ARC_CCW = "Arc CCW (G3)"      # 逆时针弧线
    RETRACT = "Retract"
    UNKNOWN = "Unknown"

class ViolationType(Enum):
    X_MIN = "X < Min"
    X_MAX = "X > Max"
    Y_MIN = "Y < Min"
    Y_MAX = "Y > Max"
    Z_MAX = "Z > Max"
    RADIUS = "Radius > Max (Circle bed)"

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
    distance_out: float  # 超出距离 (mm)

    def __str__(self):
        vio_str = ", ".join([v.value for v in self.violation_types])
        return (f"Line {self.line_num}: {self.move_type.value} - {vio_str}\n"
                f"  Position: X={self.position.x:.3f} Y={self.position.y:.3f} "
                f"Z={self.position.z:.3f} E={self.position.e:.3f}\n"
                f"  Out by: {self.distance_out:.3f} mm\n"
                f"  G-code: {self.line_content.strip()}")


class GCodeAnalyzer:
    def __init__(self, bed_type: BedType, bed_min: Tuple[float, float],
                 bed_max: Tuple[float, float], max_z: float, radius: float = None):
        self.bed_type = bed_type
        self.bed_min = bed_min
        self.bed_max = bed_max
        self.max_z = max_z
        self.radius = radius  # For circle bed
        self.center = ((bed_max[0] + bed_min[0]) / 2,
                      (bed_max[1] + bed_min[1]) / 2) if bed_type == BedType.CIRCLE else None

        self.current_pos = Position()
        self.violations: List[Violation] = []

        # 统计
        self.total_moves = 0
        self.travel_moves = 0
        self.extrude_moves = 0

    def parse_gcode_file(self, filename: str):
        """解析G-code文件"""
        print(f"正在分析文件: {filename}")
        print(f"床类型: {self.bed_type.value}")

        if self.bed_type == BedType.RECTANGLE:
            print(f"床边界: X[{self.bed_min[0]:.1f}, {self.bed_max[0]:.1f}] "
                  f"Y[{self.bed_min[1]:.1f}, {self.bed_max[1]:.1f}] "
                  f"Z[0, {self.max_z:.1f}]")
        else:
            print(f"床中心: ({self.center[0]:.1f}, {self.center[1]:.1f})")
            print(f"床半径: {self.radius:.1f} mm, Z[0, {self.max_z:.1f}]")

        print("=" * 70)

        try:
            with open(filename, 'r', encoding='utf-8') as f:
                for line_num, line in enumerate(f, 1):
                    self._parse_line(line_num, line)
        except FileNotFoundError:
            print(f"错误: 文件未找到 '{filename}'")
            sys.exit(1)
        except Exception as e:
            print(f"错误: 读取文件时出错: {e}")
            sys.exit(1)

    def _parse_line(self, line_num: int, line: str):
        """解析单行G-code"""
        # 移除注释
        if ';' in line:
            code_part = line[:line.index(';')]
            comment = line[line.index(';'):]
        else:
            code_part = line
            comment = ""

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

        # 判断移动类型
        move_type = self._classify_move(code_part, self.current_pos, new_pos)

        if move_type == MoveType.TRAVEL:
            self.travel_moves += 1
        elif move_type == MoveType.EXTRUDE:
            self.extrude_moves += 1
        self.total_moves += 1

        # 检查边界 - Only check XY bounds if X or Y actually moved
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
        start_x = self.current_pos.x
        start_y = self.current_pos.y
        start_z = self.current_pos.z

        i = float(i_match.group(1)) if i_match else 0.0
        j = float(j_match.group(1)) if j_match else 0.0

        center_x = start_x + i
        center_y = start_y + j
        radius = math.sqrt(i * i + j * j)

        end_x = float(x_match.group(1)) if x_match else None
        end_y = float(y_match.group(1)) if y_match else None
        end_z = float(z_match.group(1)) if z_match else start_z
        e = float(e_match.group(1)) if e_match else self.current_pos.e

        if end_x is None and end_y is None:
            end_angle = math.atan2(start_y - center_y, start_x - center_x) + (2 * math.pi if g_code == 3 else -2 * math.pi)
            end_x = center_x + radius * math.cos(end_angle)
            end_y = center_y + radius * math.sin(end_angle)
        elif end_x is None:
            end_x = start_x
        elif end_y is None:
            end_y = start_y

        start_angle = math.atan2(start_y - center_y, start_x - center_x)
        end_angle = math.atan2(end_y - center_y, end_x - center_x)

        if g_code == 2:
            if end_angle > start_angle:
                end_angle -= 2 * math.pi
            angle_sweep = start_angle - end_angle
        else:
            if end_angle < start_angle:
                end_angle += 2 * math.pi
            angle_sweep = end_angle - start_angle

        num_samples = max(8, int(abs(angle_sweep) * radius / 5))

        move_type = MoveType.ARC_CCW if g_code == 3 else MoveType.ARC_CW
        if move_type == MoveType.ARC_CW:
            self.travel_moves += 1
        else:
            self.extrude_moves += 1
        self.total_moves += 1

        for n in range(num_samples + 1):
            t = n / num_samples
            angle = start_angle + (angle_sweep * t if g_code == 3 else -angle_sweep * t)

            sample_x = center_x + radius * math.cos(angle)
            sample_y = center_y + radius * math.sin(angle)
            sample_z = start_z + (end_z - start_z) * t

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
                break

        self.current_pos.x = end_x
        self.current_pos.y = end_y
        self.current_pos.z = end_z
        self.current_pos.e = e

    def _classify_move(self, code: str, old_pos: Position, new_pos: Position) -> MoveType:
        """分类移动类型"""
        # G0 通常是快速移动（Travel）
        if code.startswith('G0'):
            return MoveType.TRAVEL

        # G1 可能是Travel或Extrude，看E值
        if code.startswith('G1'):
            if abs(new_pos.e - old_pos.e) > 0.001:  # 有挤出
                return MoveType.EXTRUDE
            else:
                return MoveType.TRAVEL

        # G2/G3 是弧线，通常是挤出
        if code.startswith('G2') or code.startswith('G3'):
            return MoveType.EXTRUDE

        return MoveType.UNKNOWN

    def _check_bounds(self, pos: Position) -> List[ViolationType]:
        """检查坐标是否超出边界"""
        violations = []
        epsilon = 0.01  # 允许的误差

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

        # Z轴检查
        if self.max_z > 0 and pos.z > self.max_z + epsilon:
            violations.append(ViolationType.Z_MAX)

        return violations

    def _calculate_distance_out(self, pos: Position) -> float:
        """计算超出边界的距离"""
        if self.bed_type == BedType.RECTANGLE:
            dx = max(0, self.bed_min[0] - pos.x, pos.x - self.bed_max[0])
            dy = max(0, self.bed_min[1] - pos.y, pos.y - self.bed_max[1])
            dz = max(0, pos.z - self.max_z) if self.max_z > 0 else 0
            return math.sqrt(dx**2 + dy**2 + dz**2)

        elif self.bed_type == BedType.CIRCLE:
            dist = math.sqrt((pos.x - self.center[0])**2 + (pos.y - self.center[1])**2)
            return max(0, dist - self.radius)

        return 0.0

    def print_report(self):
        """打印分析报告"""
        print("\n" + "=" * 70)
        print("分析报告 / Analysis Report")
        print("=" * 70)

        print(f"\n总移动数: {self.total_moves}")
        print(f"  - Travel移动: {self.travel_moves}")
        print(f"  - Extrude移动: {self.extrude_moves}")
        print(f"  - 其他: {self.total_moves - self.travel_moves - self.extrude_moves}")

        print(f"\n发现超限: {len(self.violations)} 处")

        if not self.violations:
            print("\n✅ 所有移动都在边界内！")
            return

        # 按类型分组
        travel_violations = [v for v in self.violations if v.move_type == MoveType.TRAVEL]
        extrude_violations = [v for v in self.violations if v.move_type == MoveType.EXTRUDE]

        print(f"  - Travel超限: {len(travel_violations)}")
        print(f"  - Extrude超限: {len(extrude_violations)}")

        # 按超限类型统计
        print("\n超限类型统计:")
        from collections import Counter
        all_vio_types = []
        for v in self.violations:
            all_vio_types.extend(v.violation_types)
        vio_counter = Counter(all_vio_types)
        for vio_type, count in vio_counter.most_common():
            print(f"  {vio_type.value}: {count} 次")

        # 详细列出超限
        print("\n" + "=" * 70)
        print("详细超限列表 (前50个):")
        print("=" * 70)

        for i, violation in enumerate(self.violations[:50], 1):
            print(f"\n[{i}] {violation}")

        if len(self.violations) > 50:
            print(f"\n... 还有 {len(self.violations) - 50} 个超限未显示")

        # 保存到文件
        output_file = "gcode_violations.txt"
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write("G-code边界超限详细报告\n")
            f.write("=" * 70 + "\n\n")

            for i, violation in enumerate(self.violations, 1):
                f.write(f"[{i}] {violation}\n\n")

        print(f"\n完整报告已保存到: {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description='分析G-code文件的边界超限问题',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 矩形床 200x200x250mm
  python %(prog)s output.gcode --bed-size 200 200 250

  # 矩形床，指定原点偏移
  python %(prog)s output.gcode --bed-size 200 200 250 --bed-origin 0 0

  # 圆形床（如Delta打印机）
  python %(prog)s output.gcode --bed-type circle --radius 100 --max-z 250
        """
    )

    parser.add_argument('gcode_file', help='G-code文件路径')
    parser.add_argument('--bed-type', choices=['rectangle', 'circle'],
                       default='rectangle', help='床类型 (默认: rectangle)')
    parser.add_argument('--bed-size', type=float, nargs=3, metavar=('X', 'Y', 'Z'),
                       help='床尺寸 X Y Z (mm), 例如: 200 200 250')
    parser.add_argument('--bed-origin', type=float, nargs=2, metavar=('X', 'Y'),
                       default=(0, 0), help='床原点坐标 (默认: 0 0)')
    parser.add_argument('--radius', type=float, help='圆形床半径 (mm)')
    parser.add_argument('--max-z', type=float, help='最大Z高度 (mm)')

    args = parser.parse_args()

    # 解析床参数
    bed_type = BedType(args.bed_type)

    if bed_type == BedType.RECTANGLE:
        if not args.bed_size:
            print("错误: 矩形床需要指定 --bed-size")
            sys.exit(1)
        bed_min = (args.bed_origin[0], args.bed_origin[1])
        bed_max = (args.bed_origin[0] + args.bed_size[0],
                  args.bed_origin[1] + args.bed_size[1])
        max_z = args.bed_size[2]
        radius = None

    elif bed_type == BedType.CIRCLE:
        if not args.radius or not args.max_z:
            print("错误: 圆形床需要指定 --radius 和 --max-z")
            sys.exit(1)
        bed_min = (-args.radius, -args.radius)
        bed_max = (args.radius, args.radius)
        max_z = args.max_z
        radius = args.radius

    # 分析G-code
    analyzer = GCodeAnalyzer(bed_type, bed_min, bed_max, max_z, radius)
    analyzer.parse_gcode_file(args.gcode_file)
    analyzer.print_report()


if __name__ == '__main__':
    main()
