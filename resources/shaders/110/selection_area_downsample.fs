#version 110

uniform sampler2D source_texture;
uniform vec2 sample_offset_uv;
uniform bool sample_horizontal;
uniform bool sample_vertical;

varying vec2 tex_coord;

void main()
{
    float coverage;
    if (sample_horizontal && sample_vertical)
    {
        coverage = texture2D(source_texture, tex_coord + vec2(-sample_offset_uv.x, -sample_offset_uv.y)).a;
        coverage += texture2D(source_texture, tex_coord + vec2(sample_offset_uv.x, -sample_offset_uv.y)).a;
        coverage += texture2D(source_texture, tex_coord + vec2(-sample_offset_uv.x, sample_offset_uv.y)).a;
        coverage += texture2D(source_texture, tex_coord + vec2(sample_offset_uv.x, sample_offset_uv.y)).a;
        coverage *= 0.25;
    }
    else if (sample_horizontal)
    {
        coverage = texture2D(source_texture, tex_coord - vec2(sample_offset_uv.x, 0.0)).a;
        coverage += texture2D(source_texture, tex_coord + vec2(sample_offset_uv.x, 0.0)).a;
        coverage *= 0.5;
    }
    else if (sample_vertical)
    {
        coverage = texture2D(source_texture, tex_coord - vec2(0.0, sample_offset_uv.y)).a;
        coverage += texture2D(source_texture, tex_coord + vec2(0.0, sample_offset_uv.y)).a;
        coverage *= 0.5;
    }
    else
        coverage = texture2D(source_texture, tex_coord).a;

    gl_FragColor = vec4(0.0, 0.0, 0.0, coverage);
}
