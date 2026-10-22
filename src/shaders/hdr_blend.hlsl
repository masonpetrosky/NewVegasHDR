// HDR versions of the game's ISHDRBLENDINSHADER pixel shaders: blend the bloom into the exposed scene, apply the image
// space modifier's cinematic grading and fade, and tonemap. The plugin hands these to the game in place of its own (or
// Vanilla Plus Tonemapper's) when the game creates them, so the game sets their constants and textures exactly as for
// the originals. Built per variant (CINEMATIC, ALPHA_MASK) and per shader model: ps_2_b pairs with the game's vs_1_1
// vertex shader, ps_3_0 with the vs_3_0 that Vanilla Plus Tonemapper ships.
//
// The output stays in the game's gamma space, so the HUD and the later image space passes blend over it as before; it
// only goes above 1 where highlights used to clip. The plugin decodes the finished frame to scRGB when it is presented.
//
// Two looks, picked per shader by the plugin: the game's own (no curve; the 8-bit target clipped everything above 1)
// and Vanilla Plus Tonemapper's (an extended Reinhard curve on luminance in linear light, white at 2.5). Tonemap.z = 0
// reproduces the original SDR output exactly (checked by tests/harness.cpp); 1 keeps the highlights the SDR target
// threw away, up to a soft shoulder below the display's peak. Vanilla Plus's curve squeezes nearly everything under
// white, so in HDR it can be stretched over more of the display's range (Decode.z).

#ifndef CINEMATIC
#define CINEMATIC 0
#endif
#ifndef ALPHA_MASK
#define ALPHA_MASK 0
#endif

sampler2D Bloom : register(s0);  // the game's Src0: bloom in rgb, adapted average luminance in a
sampler2D Scene : register(s1);  // DestBlend: the HDR scene, its alpha marks masked pixels for ALPHA_MASK

float4 HDRParam : register(c1);  // x: luminance the exposure adapts to
float4 BlurScale : register(c2);  // z: output alpha (the game leaves it unset at times: NaN)
float4 Cinematic : register(c19);  // x saturation, y contrast pivot, z contrast, w brightness
float4 Tint : register(c20);  // rgb colour, a amount
float4 Fade : register(c22);  // rgb colour, a amount
float4 UseAlphaMask : register(c23);  // w: how much of the result masked pixels get

// Set by the plugin. x: peak brightness in units of paper white; y: 1 = Vanilla Plus Tonemapper's curve, 0 = none;
// z: 1 = HDR, 0 = the original SDR output; w: start of the shoulder as a fraction of the peak.
float4 Tonemap : register(c24);
// x: the gamma the plugin decodes the frame with, y: 1 / x; z: how far Vanilla Plus's curve is stretched (1 = as in
// SDR); w: what the target keeps of the output alpha (see Alpha).
float4 Decode : register(c25);

// Vanilla Plus Tonemapper's own conversions (part of its look, whatever the decode gamma).
float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToGamma(float3 c) { return pow(max(c, 0), 1 / 2.2); }

// Leaves everything below the knee alone and rolls the rest off exponentially towards the peak, by the brightest
// channel so that hues stay put. Continuous in value and slope at the knee.
float3 Shoulder(float3 lin) {
  float peak = Tonemap.x;
  float knee = Tonemap.w * peak;
  float m = max(max(lin.r, lin.g), lin.b);
  float rolled = peak - (peak - knee) * exp((knee - m) / (peak - knee));
  return lin * (m > knee ? rolled / m : 1);
}

// The output alpha as the target the game asked for keeps it. w = 0: as it is (a 16-bit float target of the game's
// own); 1: clipped to [0, 1], NaN to 0 (an 8-bit target the plugin made 16-bit float); 2: one (the back buffer, which
// the game made X8R8G8B8).
float Alpha() {
  return Decode.w > 1.5 ? 1 : (Decode.w > 0.5 ? saturate(BlurScale.z) : BlurScale.z);
}

float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : COLOR0 {
  const bool vanillaPlus = Tonemap.y > 0.5;
  const float3 luma = vanillaPlus ? float3(0.2116, 0.7152, 0.0722) : float3(0.299, 0.587, 0.114);

  // Exposure and bloom, as in the game.
  float4 bloom = tex2D(Bloom, uv0);
  float adapted = max(bloom.a, HDRParam.x);
  float3 color = HDRParam.x / adapted * tex2D(Scene, uv1).rgb + max(bloom.rgb * (0.5 / adapted), 0);

#if CINEMATIC
  // Saturation, tint, brightness and contrast in gamma space, as in the game.
  float grey = dot(color, luma);
  color = lerp(grey, color, Cinematic.x);
  color = lerp(color, grey * Tint.rgb, Tint.a);
  color = (Cinematic.w * color - Cinematic.y) * Cinematic.z + Cinematic.y;
#endif

  if (vanillaPlus) {
    // Vanilla Plus Tonemapper: a slight contrast curve in log2 space, then extended Reinhard on luminance. Running the
    // Reinhard on luminance / Decode.z stretches it over that much more range: shadows and mid-tones barely move,
    // while what it squeezed towards white (skies, sunlit ground) goes brighter.
    float3 lin = ToLinear(color);
    lin = max(exp2((log2(lin + 1e-4) - 0.18) * 1.02 + 0.18) - 1e-4, 0);
    float y = dot(lin, luma) / Decode.z;
    color = ToGamma(lin * (y * 0.16 + 1) / (y + 1));
  }

#if CINEMATIC
  color = lerp(color, Fade.rgb, Fade.a);
#endif

#if ALPHA_MASK
  // Masked pixels (scene alpha 0) keep the untouched scene, clipped as the SDR target clipped it; the rest get
  // UseAlphaMask.w of the result over the untouched scene.
  float4 raw = tex2D(Scene, uv0);
  color = raw.a == 0 ? saturate(raw.rgb) : lerp(raw.rgb, color, UseAlphaMask.w);
#endif

  // The SDR target clipped everything here at 0 and 1. HDR keeps what lies above 1, rolled off towards the peak in the
  // light the plugin decodes the frame to.
  if (Tonemap.z > 0.5)
    return float4(pow(max(Shoulder(pow(max(color, 0), Decode.x)), 0), Decode.y), Alpha());
  return float4(saturate(color), Alpha());
}
