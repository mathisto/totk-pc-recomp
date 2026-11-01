#version 450
// The game's tonemap pass for HDR output (runtime/vk_hdr.c vk_hdr_shader_swap puts it in place of the translated
// shader when TOTK_HDR=1). Same inputs as the game's: fp_t_tcb_8 (bloom, clamped to 10000 as the game does) plus
// fp_t_tcb_A (the scene), weights in c1[0] and the saturation grade in c3[0]. The game maps luminance and each channel
// with 1 - e^-x, blends the two by the luminance curve squared, clamps to 0..1 and grades saturation. Here the curve is
// the game's up to SHOULDER (where it reaches 0.7 of the picture's white), then rolls off from there with the game's
// slope toward the display's peak, so shadows, mid-tones and sunlit surfaces look as in SDR and only the brightest
// highlights (the sun, speculars, bloom) go above the picture's white. A straight line above mid-grey, as before, put
// most of a sunny scene at two to three times its SDR brightness and looked blown out. The grade works on the colour
// divided by its largest channel above 1; a roll-off on the largest channel keeps any channel under the peak. The
// output is relative to the HUD's white (1.0), which the present maps to TOTK_HDR_UI_NITS.
// Approach after RenoDX's TotK mod by souperman9 and Musa (github.com/clshortfuse/renodx, src/games/totk, MIT): its
// "extended vanilla" curve, with a shoulder in place of the linear extension.
// Bindings are those of the game's translated shader: vk_hdr.c rewrites them from the original's names.
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0, std140) uniform SupportBuffer {
    vec4 pad[88];
    vec4 hdr;   // byte 1408 (vk_exec_draw.c support_fill): game white / HUD white, peak / game white
} support_buffer;
layout(set = 0, binding = 1) uniform sampler2D fp_t_tcb_8;
layout(set = 0, binding = 2) uniform sampler2D fp_t_tcb_A;
layout(set = 0, binding = 3, std140) uniform C1 { vec4 v[4096]; } fp_c1;
layout(set = 0, binding = 4, std140) uniform C3 { vec4 v[4096]; } fp_c3;

const float SHOULDER = 1.2039728;   // -ln(0.3): the game's curve is 0.7 there, its slope 0.3

// The game's 1 - e^-x below SHOULDER; above, an exponential from there toward peak that starts with the same slope.
float curve(float x, float peak) {
    if (x <= SHOULDER) return 1.0 - exp(-x);
    float r = max(peak - 0.7, 0.3);
    return 0.7 + r * (1.0 - exp(-(x - SHOULDER) * 0.3 / r));
}

vec3 curve3(vec3 x, float peak) { return vec3(curve(x.r, peak), curve(x.g, peak), curve(x.b, peak)); }

float maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }

void main() {
    vec3 x = min(texture(fp_t_tcb_8, uv.xy).rgb, vec3(10000.0)) + texture(fp_t_tcb_A, uv.xy).rgb;
    vec4 w = fp_c1.v[0], g = fp_c3.v[0];
    float game = support_buffer.hdr.x > 0.0 ? support_buffer.hdr.x : 1.0;
    float peak = support_buffer.hdr.y > 1.0 ? support_buffer.hdr.y : 1.0;

    float lum = max(x.r * 0.2989 + x.g * w.x + x.b * w.y, 0.0);
    float sdr = 1.0 - exp(-lum);
    vec3 by_lum = lum > 0.0 ? x * (curve(lum, peak) / lum) : x;
    vec3 c = max(mix(by_lum, curve3(x, peak), sdr * sdr), vec3(0.0));

    // the game's saturation grade, on the colour scaled into 0..1 by its largest channel
    float s = max(maxc(c), 1.0);
    vec3 n = c / s;
    float m = maxc(n);
    float d = n.r * 0.6666667 + (n.g + n.b) * w.z - 1.0;
    float grade = g.y * (1.0 - d * d) + g.x;
    c = s * ((n - m) * grade + m);

    // roll-off of the largest channel from 0.8 of the peak to the peak (the curve already rolls off the luminance)
    float top = maxc(c), knee = 0.8 * peak;
    if (top > knee) {
        float r = peak - knee;
        c *= (knee + r * (1.0 - exp(-(top - knee) / r))) / top;
    }
    color = vec4(max(c, vec3(0.0)) * game, 0.0);
}
