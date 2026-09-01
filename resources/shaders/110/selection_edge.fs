#version 110

uniform sampler2D mask_texture;
uniform vec2 inverse_texture_size;

varying vec2 tex_coord;

void main()
{
    float leftCoverage = texture2D(mask_texture, tex_coord - vec2(inverse_texture_size.x, 0.0)).a;
    float rightCoverage = texture2D(mask_texture, tex_coord + vec2(inverse_texture_size.x, 0.0)).a;
    float bottomCoverage = texture2D(mask_texture, tex_coord - vec2(0.0, inverse_texture_size.y)).a;
    float topCoverage = texture2D(mask_texture, tex_coord + vec2(0.0, inverse_texture_size.y)).a;
    float horizontalEdge = (rightCoverage - leftCoverage) * 0.5;
    float verticalEdge = (topCoverage - bottomCoverage) * 0.5;
    float edge = length(vec2(horizontalEdge, verticalEdge));

    gl_FragColor = vec4(0.0, 0.0, 0.0, edge);
}
