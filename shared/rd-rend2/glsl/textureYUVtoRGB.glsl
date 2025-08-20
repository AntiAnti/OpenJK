/*[Vertex]*/
#if defined(USE_VERTICES)
in vec3 attr_Position;
in vec4 attr_TexCoord0;

uniform mat4 u_ModelViewProjectionMatrix;
#endif
out vec2 var_Tex1;


void main()
{
#if defined(USE_VERTICES)
    gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
    var_Tex1 = attr_TexCoord0.st;
#else
    vec2 position = vec2(2.0 * float(gl_VertexID & 2) - 1.0, 4.0 * float(gl_VertexID & 1) - 1.0);
    gl_Position = vec4(position, 0.0, 1.0);
    var_Tex1 = position * 0.5 + vec2(0.5);
#endif
}

/*[Fragment]*/
uniform sampler2D u_DiffuseY;
uniform sampler2D u_DiffuseU;
uniform sampler2D u_DiffuseV;

in vec2 var_Tex1;

out vec4 out_Color;

void main()
{
    float y = 1.164 * (texture(u_DiffuseY, var_Tex1).r - 0.0627);
    float u = texture(u_DiffuseU, var_Tex1).r - 0.5;
    float v = texture(u_DiffuseV, var_Tex1).r - 0.5;

    /* --- BT.709 --- */
    /*
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    */

    /* --- BT.601 --- */
    float r = y + 1.596 * v;
    float g = y - 0.392 * u - 0.813 * v;
    float b = y + 2.017 * u;

    out_Color = vec4(r, g, b, 1.0);
}