#version 140

uniform sampler2D mask_texture;
uniform sampler2D edge_texture;
uniform sampler2D glow_texture;
uniform vec3 outline_color;

const float fillAlpha = 0.4;
const float outlineExclusionLow = 0.25;
const float outlineExclusionHigh = 0.75;
const float edgeStrength = 3.0;
const float edgeGlow = 0.8;

in vec2 tex_coord;
out vec4 frag_color;

void main()
{
    float coverage = texture(mask_texture, tex_coord).a;
    float edgeCoverage = texture(edge_texture, tex_coord).a;
    float glowCoverage = texture(glow_texture, tex_coord).a;
    float outlineExclusion = smoothstep(outlineExclusionLow, outlineExclusionHigh, coverage);
    float outsideFill = 1.0 - outlineExclusion;
    float fillCoverage = coverage * fillAlpha;
    float edgeAlpha = clamp(edgeStrength * edgeCoverage * outsideFill, 0.0, 1.0);
    float glowAlpha = clamp(edgeStrength * edgeGlow * glowCoverage * outsideFill, 0.0, 1.0);

    // Precompose normal-alpha Fill and Edge followed by additive Glow.
    float compositeAlpha = fillCoverage + edgeAlpha * (1.0 - fillCoverage);
    vec3 compositeColor = vec3(fillAlpha) * fillCoverage * (1.0 - edgeAlpha) + outline_color * (edgeAlpha + glowAlpha);
    frag_color = vec4(compositeColor, compositeAlpha);
}
