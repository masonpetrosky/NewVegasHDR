// Decodes the finished frame (the game's gamma space, HUD included, above 1 only for highlights) to scRGB for the HDR
// swap chain: linear light where 1.0 is 80 nits. Drawn by the plugin just before the frame is presented, and once more
// just after it with Encode = {1, 65504, 1} to put the frame back as the game drew it.

sampler2D Frame : register(s0);  // a copy of the back buffer, point sampled

float4 Encode : register(c0);  // x: paper white / 80 nits, y: peak / 80 nits, z: decoding gamma
float4 Size : register(c1);  // xy: 1 / back buffer size

float4 VSMain(float4 position : POSITION) : POSITION { return position; }

float4 PSMain(float2 pixel : VPOS) : COLOR0 {
  float3 c = tex2Dlod(Frame, float4((pixel + 0.5) * Size.xy, 0, 0)).rgb;
  // Gamma 1 skips the pow, which comes out a hair low and then rounds down a 16-bit float step when written.
  float3 lin = (Encode.z == 1 ? c : pow(max(c, 0), Encode.z)) * Encode.x;
  return float4(min(lin, Encode.y), 1);
}
