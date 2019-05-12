#version 450 core

layout (location = 0) in vec2 inUV;

layout (binding = 0) uniform sampler2D s_font;

layout (location = 0) out vec4 outFragColor;

layout(push_constant) uniform TextColor {
	vec4 color;
};

const float smoothing = 1.0/16.0;
const vec2 shadowOffset = vec2(-1.0/512.0);
const vec4 glowColor = vec4(vec3(1), 1.0);
const float glowMin = 0.2;
const float glowMax = 0.8;

vec4 shadowGlowText(void)
{
	float value = texture(s_font, inUV).r;
	float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, value);

	float glowDst = texture(s_font, inUV + shadowOffset).r;
	vec4 glow = glowColor * smoothstep(glowMin, glowMax, glowDst);
	glow.a = glowDst;

	//float mask = color.a-alpha;
	float mask = 1.0-alpha;

	vec4 base = vec4(color.rgb, color.a*value);
	return mix(base, glow, mask);
}

float sampleAlpha(float alpha_bias, float dist_range) {
	float value = texture(s_font, inUV).r + alpha_bias - 0.5f;
	float dist  = value * dot(vec2(dist_range, dist_range), 1.0f / fwidth(inUV.xy)) / 512;
	return clamp(dist + 0.5f, 0.0f, 1.0f);
}

vec4 shadowedText(void)
{
	float r_alpha_center = sampleAlpha(0.0f, 5.0f);
	float r_alpha_shadow = sampleAlpha(0.3f, 5.0f);

	vec4 r_center = vec4(color.rgb, color.a * r_alpha_center);
	vec4 r_shadow = vec4(0.0f, 0.0f, 0.0f, r_alpha_shadow);

	vec4 o_color = mix(r_shadow, r_center, r_alpha_center);
	o_color.rgb *= o_color.a;
	return o_color;
}

vec4 plainText(void)
{
	float value = texture(s_font, inUV).r;
	vec4 shadow = vec4(vec3(0.3), texture(s_font, inUV + shadowOffset).r);
	return mix(vec4(color.rgb, value * color.a), shadow, 1.0-value);
}

void main(void)
{
	outFragColor = plainText();
}
