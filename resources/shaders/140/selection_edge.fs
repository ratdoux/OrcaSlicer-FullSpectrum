#version 140

uniform sampler2D mask_texture;
uniform vec2 inverse_texture_size;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    float leftCoverage = texture(mask_texture, tex_coord - vec2(inverse_texture_size.x, 0.0)).a;
    float rightCoverage = texture(mask_texture, tex_coord + vec2(inverse_texture_size.x, 0.0)).a;
    float bottomCoverage = texture(mask_texture, tex_coord - vec2(0.0, inverse_texture_size.y)).a;
    float topCoverage = texture(mask_texture, tex_coord + vec2(0.0, inverse_texture_size.y)).a;
    float horizontalEdge = (rightCoverage - leftCoverage) * 0.5;
    float verticalEdge = (topCoverage - bottomCoverage) * 0.5;
    float edge = length(vec2(horizontalEdge, verticalEdge));

    frag_color = vec4(0.0, 0.0, 0.0, edge);
}
