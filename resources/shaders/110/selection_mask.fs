#version 110

uniform vec3 output_color;

void main()
{
    gl_FragColor = vec4(output_color, 1.0);
}
