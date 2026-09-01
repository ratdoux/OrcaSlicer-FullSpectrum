#version 110

uniform sampler2D source_texture;
uniform vec2 sample_step_uv;
uniform float center_weight;
uniform vec4 sample_offsets;
uniform vec4 sample_weights;
uniform int symmetric_sample_count;

varying vec2 tex_coord;

float sampleSymmetricPair(float offset)
{
    vec2 uvOffset = sample_step_uv * offset;
    return texture2D(source_texture, tex_coord + uvOffset).a +
           texture2D(source_texture, tex_coord - uvOffset).a;
}

void main()
{
    float blurred = texture2D(source_texture, tex_coord).a * center_weight;
    if (symmetric_sample_count > 0)
        blurred += sampleSymmetricPair(sample_offsets.x) * sample_weights.x;
    if (symmetric_sample_count > 1)
        blurred += sampleSymmetricPair(sample_offsets.y) * sample_weights.y;
    if (symmetric_sample_count > 2)
        blurred += sampleSymmetricPair(sample_offsets.z) * sample_weights.z;
    if (symmetric_sample_count > 3)
        blurred += sampleSymmetricPair(sample_offsets.w) * sample_weights.w;

    gl_FragColor = vec4(0.0, 0.0, 0.0, blurred);
}
