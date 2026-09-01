#version 140

uniform sampler2D source_texture;
uniform vec2 sample_offset_uv;
uniform bool sample_horizontal;
uniform bool sample_vertical;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    float coverage;
    if (sample_horizontal && sample_vertical)
    {
        coverage = texture(source_texture, tex_coord + vec2(-sample_offset_uv.x, -sample_offset_uv.y)).a;
        coverage += texture(source_texture, tex_coord + vec2(sample_offset_uv.x, -sample_offset_uv.y)).a;
        coverage += texture(source_texture, tex_coord + vec2(-sample_offset_uv.x, sample_offset_uv.y)).a;
        coverage += texture(source_texture, tex_coord + vec2(sample_offset_uv.x, sample_offset_uv.y)).a;
        coverage *= 0.25;
    }
    else if (sample_horizontal)
    {
        coverage = texture(source_texture, tex_coord - vec2(sample_offset_uv.x, 0.0)).a;
        coverage += texture(source_texture, tex_coord + vec2(sample_offset_uv.x, 0.0)).a;
        coverage *= 0.5;
    }
    else if (sample_vertical)
    {
        coverage = texture(source_texture, tex_coord - vec2(0.0, sample_offset_uv.y)).a;
        coverage += texture(source_texture, tex_coord + vec2(0.0, sample_offset_uv.y)).a;
        coverage *= 0.5;
    }
    else
        coverage = texture(source_texture, tex_coord).a;

    frag_color = vec4(0.0, 0.0, 0.0, coverage);
}
