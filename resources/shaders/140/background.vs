#version 140

in vec3 v_position;

out vec2 tex_coord;

void main()
{
	tex_coord = v_position.xy * 0.5 + 0.5;
    gl_Position = vec4(v_position, 1.0);
}
