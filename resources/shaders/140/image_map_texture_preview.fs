#version 140

uniform sampler2D uniform_texture;
uniform vec4 uniform_color;
uniform vec2 z_range;
uniform bool image_map_cycle_preview;
uniform int image_map_highlight_filament_id;
uniform bool transparent_wrap_u;
uniform bool transparent_wrap_v;

in vec2 intensity;
in float world_z;
in vec2 tex_coord;

out vec4 out_color;

void main()
{
    if (world_z < z_range.x || world_z > z_range.y)
        discard;
    if ((transparent_wrap_u && (tex_coord.x < 0.0 || tex_coord.x > 1.0)) ||
        (transparent_wrap_v && (tex_coord.y < 0.0 || tex_coord.y > 1.0)))
        discard;

    vec4 sampled = texture(uniform_texture, tex_coord);
    vec3 color = sampled.rgb;
    if (image_map_cycle_preview && image_map_highlight_filament_id > 0) {
        int assigned_filament_id = int(floor(sampled.a * 255.0 + 0.5));
        if (assigned_filament_id == image_map_highlight_filament_id)
            color = mix(color, vec3(0.0, 0.72, 0.64), 0.18);
        else {
            float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
            color = mix(vec3(luminance), vec3(0.08), 0.68);
        }
    }
    float alpha = image_map_cycle_preview ? uniform_color.a : sampled.a * uniform_color.a;
    if (alpha <= 0.001)
        discard;
    out_color = vec4(vec3(intensity.y) + color * intensity.x, alpha);
}
