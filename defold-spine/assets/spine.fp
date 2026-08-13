#version 140

in mediump vec2 var_texcoord0;
in lowp vec4 var_color;
in lowp vec3 var_darkcolor;

uniform lowp sampler2D texture_sampler;

uniform fs_uniforms
{
    mediump vec4 tint;
};

out vec4 out_fragColor;

void main()
{
    // Pre-multiply alpha since var_color and all runtime textures already are
    lowp vec4 tint_pm = vec4(tint.xyz * tint.w, tint.w);
    lowp vec4 color_pm = var_color * tint_pm;
    lowp vec3 darkcolor_pm = var_darkcolor * tint_pm.rgb;

    lowp vec4 tex = texture(texture_sampler, var_texcoord0.xy);

    lowp vec3 dark_rgb = (tex.aaa - tex.rgb) * darkcolor_pm;
    lowp vec3 light_rgb = tex.rgb * color_pm.rgb;

    out_fragColor = vec4(dark_rgb + light_rgb, tex.a * color_pm.a);
}
