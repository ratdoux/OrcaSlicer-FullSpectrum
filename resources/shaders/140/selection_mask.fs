#version 140

uniform vec3 output_color;

out vec4 frag_color;

void main()
{
    frag_color = vec4(output_color, 1.0);
}
