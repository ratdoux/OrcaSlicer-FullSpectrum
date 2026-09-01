#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 u_uvTransformMatrix;

attribute vec3 v_position;

varying vec2 tex_coord;

void main()
{
    vec2 texCoord;
    texCoord.x = v_position.x;
    texCoord.y = -v_position.y;
    tex_coord = (u_uvTransformMatrix * vec3(texCoord, 1.0)).xy;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
