// Stock U1 scripts from resources/profiles/Snapmaker/machine. Comparison only.
// Preserve internal whitespace and quoted template strings; ignore comment-only lines.
static const std::pair<const char*, const char*> U1_STOCK_SCRIPTS[] = {
    {"before_layer_change_gcode", R"U1(G92 E0
TIMELAPSE_TAKE_FRAME
DEFECT_DETECTION_DETECT
)U1"},
    {"change_filament_gcode", R"U1({
local max_speed_toolchange = 350.0;
local wait_for_extruder_temp = true;
position[2] = position[2] + 2.0;
local speed_toolchange = max_speed_toolchange;
if travel_speed < max_speed_toolchange then
speed_toolchange = travel_speed;
endif
"G91
G1 Z1.5 F1800
G90
";
"G1 F" + (speed_toolchange * 60) + "
";
if wait_for_extruder_temp and not((layer_num < 0) and (next_extruder == initial_tool)) then
"
";
"; " + layer_num + "
";
if layer_num == 0 then
"M109 S" + first_layer_temperature[next_extruder] + " T" + next_extruder + "
";
else
"M109 S" + temperature[next_extruder] + " T" + next_extruder + "
";
endif
endif
"M400" + "
";
"T" + next_extruder + "
";
if filament_type[next_extruder] == "PVA" then
"SET_VELOCITY_LIMIT ACCEL=3000
";
else
endif
if previous_extruder != next_extruder and initial_extruder != next_extruder then
"SM_PRINT_PREEXTRUDE_FILAMENT INDEX=" + next_extruder + "
";
endif
"G90
";
}
)U1"},
    {"layer_change_gcode", R"U1(SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER={layer_num+1}
)U1"},
    {"machine_end_gcode", R"U1({if print_sequence == "by object"}
G91
G1 X2 Y2 Z1 F24000
G90
G1 Z{max_layer_z+2} F600
{endif}
PRINT_END
TIMELAPSE_STOP
)U1"},
    {"machine_start_gcode", R"U1(PRINT_START
DEFECT_DETECTION_START
SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER=0
TIMELAPSE_START
M140 S{bed_temperature_initial_layer_single}
M104 T{initial_extruder} S140
M204 S10000
{if chamber_cooling_mode==0}
SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=600
{elsif chamber_cooling_mode==2}
SET_PURIFIER_MODE MODE=1 DESIRE_TEMP=42 ALARM_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=0
{else}
SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=0 FAN_SPEED=0.6 DELAY_OFF=600
{endif}
G28 X Y
DEFECT_DETECT_NOODLE_FIRST
T{initial_extruder}
G90
DEFECT_DETECTION_DETECT_BED
SM_PRINT_CHECK_SWITCH_EXTRUDER
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=0
SM_PRINT_FLOW_CALIBRATE EXTRUDER=0
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=1
SM_PRINT_FLOW_CALIBRATE EXTRUDER=1
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=2
SM_PRINT_FLOW_CALIBRATE EXTRUDER=2
SM_PRINT_AUTO_FEED EXTRUDER=3
SM_PRINT_FLOW_CALIBRATE EXTRUDER=3
M104 S0 T0 A0
M104 S0 T1 A0
M104 S0 T2 A0
M104 S0 T3 A0
M104 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}
T{initial_extruder}
M106 S255
M106 P2 S0
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}
ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
G28 Z I140 J140
DETECT_BED_PLATE
G90
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 S{nozzle_temperature[initial_extruder] - 50}
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_1
M104 S{nozzle_temperature[initial_extruder] - 90}
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_2
M106 S255
M109 S{nozzle_temperature[initial_extruder] - 90}
M190 S{bed_temperature_initial_layer_single}
M107 P2
G90
G0 Z5 F10000
WAIT_CHAMBER_TEMP TIMEOUT=180
{if curr_bed_type=="High Temp Plate"}
G28 Z Z_OFFSET -0.07
{elsif curr_bed_type=="Graphic Effect Plate"}
G28 Z Z_OFFSET -0.07
{else}
G28 Z
{endif}
{if curr_bed_type=="High Temp Plate"}
BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07
{elsif curr_bed_type=="Graphic Effect Plate"}
BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07
{else}
BED_MESH_CALIBRATE PROBE_COUNT=11,11
{endif}
G90
G1 Z1.5
G0 X85 Y1 Z2 F18000
M109 S{nozzle_temperature_initial_layer[initial_extruder]}
G1 Z0.2
M83
G1 X185 E15 F360
G1 Z1.5
G90
M106 S0
)U1"},
    {"machine_start_gcode", R"U1(PRINT_START
DEFECT_DETECTION_START
SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER=0
TIMELAPSE_START
M140 S{bed_temperature_initial_layer_single}
M104 T{initial_extruder} S140
M204 S10000
{if chamber_cooling_mode==0}
SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=600
{elsif chamber_cooling_mode==2}
SET_PURIFIER_MODE MODE=1 DESIRE_TEMP=42 ALARM_TEMP=45 FAN_SPEED=0.6 DELAY_OFF=0
{else}
SET_PURIFIER_MODE MODE=3 DESIRE_TEMP=0 FAN_SPEED=0.6 DELAY_OFF=600
{endif}
G28 X Y
DEFECT_DETECT_NOODLE_FIRST
T{initial_extruder}
G90
DEFECT_DETECTION_DETECT_BED
SM_PRINT_CHECK_SWITCH_EXTRUDER
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=0
SM_PRINT_FLOW_CALIBRATE EXTRUDER=0
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=1
SM_PRINT_FLOW_CALIBRATE EXTRUDER=1
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=2
SM_PRINT_FLOW_CALIBRATE EXTRUDER=2
SM_PRINT_AUTO_FEED EXTRUDER=3
SM_PRINT_FLOW_CALIBRATE EXTRUDER=3
M104 S0 T0 A0
M104 S0 T1 A0
M104 S0 T2 A0
M104 S0 T3 A0
M104 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}
T{initial_extruder}
M106 S255
M106 P2 S0
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}
ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
G28 Z I140 J140
DETECT_BED_PLATE
G90
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 S{nozzle_temperature[initial_extruder] - 50}
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_1
M104 S{nozzle_temperature[initial_extruder] - 90}
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_2
M106 S255
M109 S{nozzle_temperature[initial_extruder] - 90}
M190 S{bed_temperature_initial_layer_single}
M107 P2
G90
G0 Z5 F10000
WAIT_CHAMBER_TEMP TIMEOUT=180
{if curr_bed_type=="High Temp Plate"}
G28 Z Z_OFFSET -0.07
{elsif curr_bed_type=="Graphic Effect Plate"}
G28 Z Z_OFFSET -0.07
{else}
G28 Z
{endif}
{if curr_bed_type=="Smooth PEI Plate"}
BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07
{elsif curr_bed_type=="Graphic Effect Plate"}
BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07
{else}
BED_MESH_CALIBRATE PROBE_COUNT=11,11
{endif}
G90
G1 Z1.5
G0 X85 Y1 Z2 F18000
M109 S{nozzle_temperature_initial_layer[initial_extruder]}
G1 Z0.2
M83
G1 X185 E15 F360
G1 Z1.5
G90
M106 S0
)U1"},
    // Installed stock U1 template dated 20260605, captured in the test fixture.
    {"change_filament_gcode", R"U1({
local move_z = 1.5;
local max_speed_toolchange = 350.0;
local wait_for_extruder_temp = true;
position[2] = position[2] + 2.0;
local speed_toolchange = max_speed_toolchange;
if travel_speed < max_speed_toolchange then
speed_toolchange = travel_speed;
endif
if print_sequence == "by object" then
if max_layer_z < ((max_print_height - z_offset) - 2) then
move_z = z_offset + min(((max_layer_z - z_offset) + 2), max_print_height);
endif
endif
"G91
G1 Z" + move_z + " F600
G90
";
"G1 F" + (speed_toolchange * 60) + "
";
if wait_for_extruder_temp and not((layer_num < 0) and (next_extruder == initial_tool)) then
"
";
"; " + layer_num + "
";
if layer_num == 0 then
"M109 S" + first_layer_temperature[next_extruder] + " T" + next_extruder + "
";
else
"M109 S" + temperature[next_extruder] + " T" + next_extruder + "
";
endif
endif
"M400" + "
";
"T" + next_extruder + "
";
if filament_type[next_extruder] == "PVA" then
"SET_VELOCITY_LIMIT ACCEL=3000
";
else
endif
if previous_extruder != next_extruder and initial_extruder != next_extruder then
"SM_PRINT_PREEXTRUDE_FILAMENT INDEX=" + next_extruder + "
";
endif
"G90
";
}
)U1"},
};
