#version 140

#define INTENSITY_CORRECTION 0.6

const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE   (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR  (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS 20.0

const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT   0.3

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;

in vec3 v_position;
in vec3 v_normal;
in vec2 v_tex_coord;

out vec2 intensity;
out float world_z;
out vec2 tex_coord;

void main()
{
    vec3 eye_normal = normalize(view_normal_matrix * v_normal);
    float ndotl = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);
    intensity.x = INTENSITY_AMBIENT + ndotl * LIGHT_TOP_DIFFUSE;

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0),
                                           LIGHT_TOP_SHININESS);
    ndotl = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += ndotl * LIGHT_FRONT_DIFFUSE;

    world_z = (volume_world_matrix * vec4(v_position, 1.0)).z;
    tex_coord = v_tex_coord;
    gl_Position = projection_matrix * position;
}
