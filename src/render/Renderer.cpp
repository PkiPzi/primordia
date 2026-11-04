#include "render/Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sim/World.hpp"

namespace pm {

namespace {

constexpr uint32_t RF_SELECTED = 256, RF_HOVER = 512;

const char* QUAD_VS = R"(#version 330 core
layout(location=0) in vec2 aPos;
uniform vec2 uFb;
out vec2 vScreen;
out vec2 vUv;
void main(){
  vUv = aPos*0.5+0.5;
  vScreen = vec2(vUv.x*uFb.x, (1.0-vUv.y)*uFb.y);
  gl_Position = vec4(aPos,0.0,1.0);
})";

const char* TERRAIN_FS = R"(#version 330 core
in vec2 vScreen;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oGlow;
uniform sampler2D uStatic;
uniform sampler2D uDyn;
uniform sampler2D uTerritory;
uniform sampler2D uRain;
uniform vec2 uRainOffset; uniform float uRainScale; uniform int uClouds;
uniform vec2 uCenter; uniform float uZoom; uniform vec2 uViewPx; uniform vec2 uWorld; uniform vec2 uTexel;
uniform float uSea, uTime, uSeasonSin, uSeasonAmp, uClimate;
uniform int uOverlay; uniform int uPher;
float hash(vec2 p){ p = fract(p*vec2(123.34,456.21)); p += dot(p,p+45.32); return fract(p.x*p.y); }
float vnoise(vec2 p){ vec2 i=floor(p), f=fract(p); vec2 u=f*f*(3.0-2.0*f);
  return mix(mix(hash(i),hash(i+vec2(1,0)),u.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),u.x), u.y); }
vec3 heat(float t){ t=clamp(t,0.,1.);
  vec3 a = mix(vec3(0.16,0.28,0.80), vec3(0.95,0.92,0.55), smoothstep(0.0,0.55,t));
  return mix(a, vec3(0.88,0.22,0.16), smoothstep(0.55,1.0,t)); }
void main(){
  vec2 w = (vScreen - uViewPx)/uZoom + uCenter;
  oGlow = vec4(0.0);
  if (w.x < 0.0 || w.y < 0.0 || w.x > uWorld.x || w.y > uWorld.y) {
    vec2 g = abs(fract(w/256.0)-0.5);
    float grid = smoothstep(0.485, 0.5, max(g.x,g.y));
    oColor = vec4(vec3(0.030,0.038,0.050) + grid*0.03, 1.0);
    return;
  }
  vec2 uv = w/uWorld;
  vec4 st = texture(uStatic, uv);
  float detail = (vnoise(w*0.045)-0.5)*0.014 + (vnoise(w*0.19)-0.5)*0.005;
  float e = st.r + detail;
  float fert = st.g; float tbase = st.b*3.0-1.5; float moist = st.a;
  vec2 dyn = texture(uDyn, uv).rg;
  float lat = uv.y*2.0-1.0;
  float temp = tbase - uSeasonAmp*lat*uSeasonSin + uClimate;
  float eL = texture(uStatic, uv - vec2(uTexel.x,0.0)).r, eR = texture(uStatic, uv + vec2(uTexel.x,0.0)).r;
  float eU = texture(uStatic, uv - vec2(0.0,uTexel.y)).r, eD = texture(uStatic, uv + vec2(0.0,uTexel.y)).r;
  vec3 n = normalize(vec3((eL-eR)*14.0, (eU-eD)*14.0, 1.0));
  float shade = clamp(dot(n, normalize(vec3(-0.55,-0.6,0.6))), 0.0, 1.0);
  vec3 col;
  bool land = e >= uSea;
  if (!land) {
    float depth = clamp((uSea - e)/max(uSea,0.05)*2.5, 0.0, 1.0);
    col = mix(vec3(0.17,0.56,0.62), vec3(0.03,0.11,0.24), smoothstep(0.0,0.85,depth));
    float wave = sin(w.x*0.035 + uTime*1.3 + sin(w.y*0.02+uTime*0.7)*2.0) * sin(w.y*0.03 - uTime*1.1);
    col += vec3(0.035,0.05,0.06)*wave*(1.0-depth);
    float foam = smoothstep(0.03, 0.0, uSea - e) * (0.55+0.45*sin(uTime*2.0 + w.x*0.05 + w.y*0.03));
    col = mix(col, vec3(0.86,0.95,0.95), foam*0.5);
    float ice = smoothstep(-0.38, -0.62, temp);
    col = mix(col, vec3(0.78,0.87,0.93)*(0.9+0.1*vnoise(w*0.05)), ice*0.88);
  } else {
    float h = (e - uSea)/max(1.0-uSea, 0.01);
    vec3 dry = vec3(0.64,0.54,0.37), wet = vec3(0.31,0.33,0.21), sand = vec3(0.82,0.76,0.57), rock = vec3(0.47,0.45,0.44);
    col = mix(dry, wet, clamp(moist*1.2-0.1,0.0,1.0));
    col = mix(col, vec3(0.30,0.38,0.22), fert*0.35); // fertile soil reads darker and greener
    col = mix(sand, col, smoothstep(0.0, 0.03, h));
    col = mix(col, rock, smoothstep(0.42, 0.8, h));
    float warm = clamp(temp*0.8+0.45, 0.0, 1.0);
    vec3 lush = mix(vec3(0.19,0.40,0.20), vec3(0.24,0.62,0.22), warm);
    lush = mix(lush, vec3(0.50,0.58,0.20), smoothstep(0.45, 1.2, temp));
    float veg = clamp(sqrt(dyn.r)*1.05, 0.0, 1.0);
    col = mix(col, lush*(0.9+0.2*vnoise(w*0.12)), veg*0.92);
    // Close-up detail: grass tufts on vegetated ground, pebbles on bare ground.
    float zoomDetail = smoothstep(0.8, 2.5, uZoom);
    if (zoomDetail > 0.0) {
      vec2 gc = w*0.55; vec2 gi = floor(gc); vec2 gf = fract(gc) - 0.5;
      float h = hash(gi);
      vec2 off = vec2(hash(gi+7.1), hash(gi+3.7)) - 0.5;
      float blade = 1.0 - smoothstep(0.0, 0.18, length((gf - off*0.6)*vec2(2.2, 0.8)));
      col = mix(col, col*mix(1.25, 0.7, h), blade*veg*zoomDetail*0.7);
      float pebble = 1.0 - smoothstep(0.12, 0.2, length(gf - off*0.5));
      col = mix(col, col*1.18, pebble*(1.0-veg)*step(0.82, h)*zoomDetail*0.6);
    }
    col *= 0.93 + 0.14*(vnoise(w*0.08)-0.5);
    float snow = smoothstep(-0.3, -0.56, temp);
    col = mix(col, vec3(0.93,0.95,0.98), snow*0.9);
    col *= 0.80 + 0.32*shade;
  }
  if (uPher == 1 && dyn.g > 0.01) {
    col = mix(col, vec3(0.86,0.38,0.96), clamp(dyn.g,0.0,1.0)*0.5);
    oGlow = vec4(vec3(0.5,0.15,0.6)*dyn.g*0.3, 1.0);
  }
  if (uOverlay == 1) col = mix(col, heat(temp*0.5+0.5), 0.75);
  else if (uOverlay == 2 && land) col = mix(col, mix(vec3(0.28,0.2,0.12), vec3(0.35,0.92,0.32), fert), 0.78);
  else if (uOverlay == 3 && land) col = mix(col, mix(vec3(0.08,0.08,0.06), vec3(0.45,1.0,0.35), dyn.r), 0.82);
  else if (uOverlay == 4) col = mix(col*0.35, vec3(0.92,0.42,1.0), clamp(dyn.g,0.0,1.0));
  float rain = 0.0;
  if (uClouds >= 0) rain = texture(uRain, w / uRainScale + uRainOffset / 64.0).r;
  if (uClouds == 1) {
    // Cloud shadow drifting with the wind (fades out when zoomed in close).
    float zoomFade = 1.0 - smoothstep(0.6, 2.0, uZoom);
    col *= 1.0 - 0.28 * rain * (0.4 + 0.6*zoomFade);
    col = mix(col, vec3(0.92,0.95,1.0), rain * 0.12 * zoomFade);
  }
  if (uOverlay == 6) col = mix(col*0.5, mix(vec3(0.75,0.6,0.35), vec3(0.25,0.55,1.0), rain), 0.75);
  else if (uOverlay == 5) { vec4 t = texture(uTerritory, uv); col = mix(col*0.55, t.rgb*0.9 + col*0.1, t.a*0.7); }
  oColor = vec4(col,1.0);
})";

const char* CREATURE_VS = R"(#version 330 core
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 iA;
layout(location=2) in vec4 iColor;
layout(location=3) in vec4 iTraits;
layout(location=4) in vec4 iAnim;
layout(location=5) in vec4 iState;
layout(location=6) in vec4 iExtra;
uniform vec2 uCenter; uniform float uZoom; uniform vec2 uViewPx; uniform vec2 uFb;
out vec2 vLocal;
flat out vec4 vColor; flat out vec4 vTraits; flat out vec4 vAnim; flat out vec4 vState; flat out vec4 vExtra;
const float EXT = 2.4;
void main(){
  vec2 local = aCorner*EXT;
  vLocal = local;
  float c = cos(iA.z), s = sin(iA.z);
  vec2 world = iA.xy + iA.w*vec2(local.x*c - local.y*s, local.x*s + local.y*c);
  vec2 scr = (world - uCenter)*uZoom + uViewPx;
  gl_Position = vec4(scr.x/uFb.x*2.0-1.0, 1.0 - scr.y/uFb.y*2.0, 0.0, 1.0);
  vColor = iColor; vTraits = iTraits; vAnim = iAnim; vState = iState; vExtra = iExtra;
})";

const char* CREATURE_FS = R"(#version 330 core
in vec2 vLocal;
flat in vec4 vColor; flat in vec4 vTraits; flat in vec4 vAnim; flat in vec4 vState; flat in vec4 vExtra;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oGlow;
uniform float uTime;
uniform int uSignals;
uniform float uNight;
uniform int uBlob;
float sdEllipse(vec2 p, vec2 r){ float k0 = length(p/r); float k1 = length(p/(r*r)); return k0*(k0-1.0)/max(k1,1e-4); }
float smin(float a, float b, float k){ float h = clamp(0.5+0.5*(b-a)/k,0.0,1.0); return mix(b,a,h)-k*h*(1.0-h); }
float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main(){
  vec2 p = vLocal;
  float pattern = vTraits.x, diet = vTraits.y, armor = vTraits.z, weapon = vTraits.w;
  float fov = vAnim.x, phase = vAnim.y, signal = vAnim.z;
  int flags = int(vAnim.w + 0.5);
  float health = vState.x, eyeSize = vState.y, thrust = vState.z, toxin = vState.w;
  float aa = max(fwidth(p.x), 0.002) * 1.1;

  vec2 br = vec2(1.0, mix(0.88, 0.70, diet));
  float body = sdEllipse(p - vec2(0.05*diet, 0.0), br);
  float shape;
  if (uBlob == 1) {
    // Primer-style blob: a squishy circle that bobs as it moves.
    float squish = 1.0 + 0.06*sin(phase*1.5)*(0.3 + abs(thrust));
    body = length(p*vec2(squish, 1.0/squish)) - 1.0;
    shape = body;
  } else {
    float t = clamp((-p.x - 0.6)/1.15, 0.0, 1.0);
    float wave = sin(phase - p.x*4.0) * 0.24 * t * (0.35 + 0.65*abs(thrust));
    float tail = abs(p.y - wave) - mix(0.30, 0.05, t);
    tail = max(tail, max(p.x + 0.55, -p.x - 1.8));
    shape = smin(body, tail, 0.18);
  }
  float jaws = 1e3;
  float horns = 1e3;
  if (weapon > 0.15 && diet > 0.45) {
    float js = 0.10 + 0.24*weapon;
    vec2 q = vec2(p.x - 0.92, abs(p.y) - 0.26 + (p.x-0.92)*0.35);
    jaws = sdEllipse(q, vec2(js*1.5, js*0.5));
    shape = min(shape, jaws);
  } else if (weapon > 0.3) {
    // Herbivore horns: two curved spikes sweeping back from the brow.
    float hl = 0.25 + 0.45*weapon;
    vec2 q = vec2(p.x - 0.35, abs(p.y) - 0.62);
    q = vec2(q.x*0.8 + q.y*0.6, -q.x*0.6 + q.y*0.8);
    float t = clamp(q.x / hl, 0.0, 1.0);
    horns = max(abs(q.y - 0.35*t*t*hl) - mix(0.11, 0.02, t), max(-q.x, q.x - hl));
    shape = min(shape, horns);
  }
  float alpha = 1.0 - smoothstep(-aa, aa, shape);
  // Soft drop shadow, light from the upper left in world space.
  float ang = vExtra.z;
  vec2 lw = vec2(0.22, 0.32);
  vec2 lo = vec2(lw.x*cos(ang) + lw.y*sin(ang), -lw.x*sin(ang) + lw.y*cos(ang));
  vec2 ps = p - lo;
  float shadowD = (uBlob == 1) ? length(ps) - 1.0 : sdEllipse(ps, br);
  float shadowA = (1.0 - smoothstep(-0.25, 0.35, shadowD)) * 0.32 * vColor.a;

  vec3 glow = vec3(0.0); float glowA = 0.0;
  if (uSignals == 1 && abs(signal) > 0.15) {
    vec3 sc = signal > 0.0 ? vec3(0.30,0.90,1.00) : vec3(1.00,0.35,0.80);
    float g = exp(-max(shape,0.0)*2.6) * (abs(signal)-0.15) * 1.2;
    glow += sc*g; glowA = max(glowA, g);
  }
  bool selected = (flags & 256) != 0, hover = (flags & 512) != 0;
  if (selected || hover) {
    float ringR = 1.75 + (selected ? 0.08*sin(uTime*4.0) : 0.0);
    float rd = abs(length(p) - ringR) - (selected ? 0.07 : 0.04);
    float ring = 1.0 - smoothstep(-aa, aa, rd);
    vec3 rc = selected ? vec3(1.0,0.86,0.35) : vec3(0.9);
    glow += rc*ring*(selected?0.8:0.3); glowA = max(glowA, ring);
    if (alpha < ring) { oColor = vec4(rc, ring*0.95); oGlow = vec4(glow, glowA); return; }
  }
  if (alpha <= 0.002) {
    if (glowA <= 0.002 && shadowA <= 0.004) discard;
    oColor = vec4(0.0, 0.0, 0.0, shadowA); oGlow = vec4(glow, min(glowA,1.0)); return;
  }

  vec3 base = vColor.rgb;
  float r = length(p*vec2(1.0,1.2));
  vec3 col = base * (1.06 - 0.28*smoothstep(0.2, 1.05, r));
  col = mix(col, min(col*1.3 + 0.06, vec3(1.0)), smoothstep(0.5, -0.4, p.y + p.x*0.3)*0.3);
  float inBody = step(body, 0.0);
  float pk = pattern*3.0;
  if (pk > 1.0 && pk < 2.0) {
    float s = smoothstep(-0.1, 0.1, sin(p.x*10.0 + 1.0) - 0.25);
    col = mix(col, col*0.58, s*(pk-1.0)*inBody);
  } else if (pk >= 2.0) {
    vec2 g = p*3.2; vec2 id = floor(g); vec2 f = fract(g)-0.5; float h = hash(id);
    float spot = 1.0 - smoothstep(0.18, 0.26, length(f + (h-0.5)*0.3));
    col = mix(col, col*0.5, spot*(pk-2.0)*inBody*step(0.35,h));
  }
  if (toxin > 0.3) {
    vec2 g = p*4.5 + 0.3; vec2 f = fract(g)-0.5; float h = hash(floor(g));
    float dot = 1.0 - smoothstep(0.08, 0.13, length(f));
    col = mix(col, vec3(0.75,1.0,0.25), dot*inBody*step(0.55,h)*smoothstep(0.3,0.8,toxin));
  }
  if (horns < 1e2) {
    float hm = 1.0 - smoothstep(-aa, aa, horns);
    col = mix(col, vec3(0.88,0.84,0.72), hm*step(0.0, body));
  }
  if (jaws < 1e2) {
    float jm = 1.0 - smoothstep(-aa, aa, jaws);
    col = mix(col, ((flags & 2) != 0) ? vec3(1.0,0.45,0.4) : vec3(0.93,0.89,0.80), jm*step(0.0, body));
  }
  if (armor > 0.5) {
    vec2 g = p*vec2(5.0, 6.0); g.x += 0.5*mod(floor(g.y), 2.0);
    float sc = smoothstep(0.32, 0.45, length(fract(g) - 0.5));
    col = mix(col, col*0.78, sc*inBody*smoothstep(0.5, 0.9, armor));
  }
  float rim = 0.07 + 0.13*armor;
  float outline = smoothstep(-rim - aa, -rim + aa, shape);
  vec3 outlineCol = col*0.32;
  if (armor > 0.4) {
    float plates = step(0.5, fract(atan(p.y,p.x)*2.5));
    outlineCol = mix(outlineCol, col*0.65, plates*0.6);
  }
  col = mix(col, outlineCol, outline);

  float ea = clamp(fov*0.2, 0.28, 1.2);
  float er = (0.17 + 0.13*eyeSize) * (uBlob == 1 ? 1.35 : 1.0) * mix(1.35, 1.0, clamp(vExtra.w, 0.0, 1.0));
  float blink = step(0.992, fract(phase*0.013));
  for (int k = 0; k < 2; k++) {
    float sgn = k == 0 ? 1.0 : -1.0;
    vec2 ec = vec2(cos(ea), sgn*sin(ea))*0.56 + vec2(0.08,0.0);
    vec2 d = p - ec; d.y /= mix(1.0, 0.15, blink);
    float de = length(d) - er;
    float em = 1.0 - smoothstep(-aa, aa, de);
    vec2 pc = ec + vec2(er*0.38, 0.0);
    float pm = 1.0 - smoothstep(-aa, aa, length(p - pc) - er*0.55);
    vec3 ecol = mix(vec3(0.97), vec3(0.04), pm*(1.0-blink));
    float hl = 1.0 - smoothstep(-aa, aa, length(p - pc - vec2(-er*0.18, -er*0.22)) - er*0.17);
    ecol = mix(ecol, vec3(1.0), hl*(1.0-blink));
    ecol = mix(ecol, vec3(0.08), smoothstep(-0.05-aa, -0.05+aa, de)*0.85);
    float shine = uNight * vExtra.x * pm * (1.0-blink);
    ecol = mix(ecol, vec3(0.75,1.0,0.45), shine);
    glow += vec3(0.6,1.0,0.35) * shine * em * 1.5;
    col = mix(col, ecol, em);
  }
  if ((flags & 1) != 0) { // eating: little mouth
    float mo = 1.0 - smoothstep(-aa, aa, sdEllipse(p - vec2(0.9, 0.0), vec2(0.1, 0.16 + 0.06*sin(uTime*18.0))));
    col = mix(col, vec3(0.25,0.08,0.08), mo*0.85);
  }
  if ((flags & 16) != 0) col = mix(col, vec3(1.0,0.15,0.12), 0.35);
  if ((flags & 64) != 0) col = mix(col, vec3(0.55,1.0,0.2), 0.3);
  if ((flags & 8) != 0) col = mix(col, vec3(0.25,0.55,0.75), 0.18);
  if ((flags & 1024) != 0) {
    float pulse = 0.5 + 0.5*sin(uTime*5.0 + phase);
    col = mix(col, vec3(0.55,0.85,0.3), 0.3 + 0.15*pulse);
    vec2 g = p*5.0; float h = hash(floor(g) + 3.0);
    float blot = 1.0 - smoothstep(0.12, 0.2, length(fract(g)-0.5));
    col = mix(col, vec3(0.35,0.5,0.15), blot*inBody*step(0.6,h)*0.8);
  }
  col *= mix(0.55, 1.0, clamp(health*1.4, 0.0, 1.0));
  float outA = alpha*vColor.a + shadowA*(1.0 - alpha*vColor.a);
  col = col * (alpha*vColor.a) / max(outA, 1e-4);
  oColor = vec4(col, outA);
  oGlow = vec4(glow + col*0.04*alpha, max(max(glowA, alpha*0.15), length(glow)));
})";

const char* CORPSE_VS = R"(#version 330 core
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 iA;
layout(location=2) in vec4 iB;
uniform vec2 uCenter; uniform float uZoom; uniform vec2 uViewPx; uniform vec2 uFb;
out vec2 vLocal; flat out vec4 vA; flat out vec4 vB;
void main(){
  vLocal = aCorner*1.3;
  vec2 world = iA.xy + vLocal*iA.z;
  vec2 scr = (world - uCenter)*uZoom + uViewPx;
  gl_Position = vec4(scr.x/uFb.x*2.0-1.0, 1.0 - scr.y/uFb.y*2.0, 0.0, 1.0);
  vA = iA; vB = iB;
})";

const char* CORPSE_FS = R"(#version 330 core
in vec2 vLocal; flat in vec4 vA; flat in vec4 vB;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oGlow;
void main(){
  vec2 p = vLocal;
  float fresh = vA.w, toxin = vB.y, seed = vB.z;
  float ang = atan(p.y, p.x);
  float rr = 0.85 + 0.1*sin(ang*5.0 + seed*6.0) + 0.06*sin(ang*3.0 - seed*3.0);
  float d = length(p) - rr;
  float aa = max(fwidth(p.x), 0.002)*1.2;
  float a = 1.0 - smoothstep(-aa, aa, d);
  if (a <= 0.0) discard;
  vec3 col = mix(vec3(0.32,0.13,0.10), vec3(0.72,0.20,0.17), fresh);
  col *= 0.8 + 0.3*smoothstep(0.9, 0.0, length(p + vec2(0.25,0.25)));
  float bone = 1.0 - smoothstep(0.04, 0.07, abs(p.y - 0.3*sin(p.x*3.0 + seed)) );
  col = mix(col, vec3(0.92,0.88,0.80), bone*step(abs(p.x), 0.5)*0.8);
  if (toxin > 0.3) col = mix(col, vec3(0.5,0.85,0.2), 0.35*toxin);
  col = mix(col, col*0.4, smoothstep(-0.12-aa, -0.12+aa, d));
  oColor = vec4(col, a*(0.45+0.55*fresh));
  oGlow = vec4(0.0);
})";

const char* PARTICLE_VS = R"(#version 330 core
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 iA;
layout(location=2) in vec4 iColor;
uniform vec2 uCenter; uniform float uZoom; uniform vec2 uViewPx; uniform vec2 uFb;
out vec2 vLocal; flat out vec4 vColor; flat out float vKind;
void main(){
  vLocal = aCorner;
  vec2 world = iA.xy + aCorner*iA.z;
  vec2 scr = (world - uCenter)*uZoom + uViewPx;
  gl_Position = vec4(scr.x/uFb.x*2.0-1.0, 1.0 - scr.y/uFb.y*2.0, 0.0, 1.0);
  vColor = iColor; vKind = iA.w;
})";

const char* PARTICLE_FS = R"(#version 330 core
in vec2 vLocal; flat in vec4 vColor; flat in float vKind;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oGlow;
void main(){
  float r = length(vLocal);
  float a;
  if (vKind < 0.5) a = exp(-r*r*4.0);
  else if (vKind < 1.5) { float aa = fwidth(r)*1.5; a = 1.0 - smoothstep(0.0, 0.04 + aa, abs(r - 0.92)); }
  else if (vKind < 2.5) { vec2 q = abs(vLocal); a = max(exp(-q.x*14.0)*exp(-q.y*2.5), exp(-q.y*14.0)*exp(-q.x*2.5)); a = max(a, exp(-r*r*12.0)); }
  else {
    // heart
    vec2 q = vLocal * vec2(1.0, -1.0) * 1.25 + vec2(0.0, 0.25);
    q.x = abs(q.x);
    float d = length(q - vec2(0.35, 0.25)) - 0.4;
    d = min(d, max(q.y*0.9 + q.x - 0.55, -q.y - 0.75 + q.x*0.2));
    float aa = fwidth(d)*1.2;
    a = 1.0 - smoothstep(-aa, aa, d);
  }
  a *= vColor.a;
  if (a < 0.003) discard;
  oColor = vec4(vColor.rgb, a*0.85);
  oGlow = vec4(vColor.rgb*1.4, a);
})";

const char* OVERLAY_VS = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aColor;
uniform vec2 uCenter; uniform float uZoom; uniform vec2 uViewPx; uniform vec2 uFb;
out vec4 vColor;
void main(){
  vec2 scr = (aPos - uCenter)*uZoom + uViewPx;
  gl_Position = vec4(scr.x/uFb.x*2.0-1.0, 1.0 - scr.y/uFb.y*2.0, 0.0, 1.0);
  vColor = aColor;
})";

const char* OVERLAY_FS = R"(#version 330 core
in vec4 vColor;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oGlow;
void main(){ oColor = vColor; oGlow = vec4(vColor.rgb*0.4, vColor.a*0.5); })";

const char* BLUR_FS = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uTex; uniform vec2 uDir;
void main(){
  vec3 c = texture(uTex, vUv).rgb * 0.227027;
  c += texture(uTex, vUv + uDir*1.3846).rgb * 0.316216;
  c += texture(uTex, vUv - uDir*1.3846).rgb * 0.316216;
  c += texture(uTex, vUv + uDir*3.2308).rgb * 0.070270;
  c += texture(uTex, vUv - uDir*3.2308).rgb * 0.070270;
  oColor = vec4(c, 1.0);
})";

const char* COMPOSITE_FS = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uScene; uniform sampler2D uBloom;
uniform float uBloomStrength; uniform float uVignette; uniform vec3 uAmbient;
void main(){
  vec3 c = texture(uScene, vUv).rgb * uAmbient + texture(uBloom, vUv).rgb * uBloomStrength;
  float v = smoothstep(1.25, 0.3, length((vUv-0.5)*vec2(1.25,1.0))*1.35);
  c *= mix(1.0, v, uVignette);
  c = c / (1.0 + max(c - 1.0, 0.0));
  oColor = vec4(c, 1.0);
})";

uint32_t packRGBA8(Color c) { return packColor(c); }

Color viridis(float t) {
    t = clamp01(t);
    const Color stops[5] = {{0.27f, 0.00f, 0.33f, 1}, {0.23f, 0.32f, 0.55f, 1}, {0.13f, 0.57f, 0.55f, 1},
                            {0.37f, 0.79f, 0.38f, 1}, {0.99f, 0.91f, 0.14f, 1}};
    float f = t * 4.f;
    int i = std::min(3, int(f));
    return mix(stops[i], stops[i + 1], f - float(i));
}

void setupInstanceAttribs(int firstLoc, int count, int strideFloats) {
    for (int k = 0; k < count; ++k) {
        glEnableVertexAttribArray(GLuint(firstLoc + k));
        glVertexAttribPointer(GLuint(firstLoc + k), 4, GL_FLOAT, GL_FALSE, strideFloats * 4,
                              reinterpret_cast<void*>(intptr_t(k * 16)));
        glVertexAttribDivisor(GLuint(firstLoc + k), 1);
    }
}

} // namespace

const char* colorModeName(ColorMode m) {
    static const char* names[] = {"Genes (hue)", "Species", "Diet", "Energy", "Generation",
                                  "Age",         "Thermal", "Toxin", "Brain size", "Disease"};
    return names[int(m)];
}
const char* overlayName(Overlay o) {
    static const char* names[] = {"None", "Temperature", "Fertility", "Plant biomass", "Pheromones", "Species territories", "Rainfall"};
    return names[int(o)];
}

bool Renderer::init() {
    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    glRenderer_ = r ? r : "unknown";
    terrainProg_.id = gl::compileProgram("terrain", QUAD_VS, TERRAIN_FS);
    creatureProg_.id = gl::compileProgram("creature", CREATURE_VS, CREATURE_FS);
    corpseProg_.id = gl::compileProgram("corpse", CORPSE_VS, CORPSE_FS);
    particleProg_.id = gl::compileProgram("particle", PARTICLE_VS, PARTICLE_FS);
    overlayProg_.id = gl::compileProgram("overlay", OVERLAY_VS, OVERLAY_FS);
    blurProg_.id = gl::compileProgram("blur", QUAD_VS, BLUR_FS);
    compositeProg_.id = gl::compileProgram("composite", QUAD_VS, COMPOSITE_FS);

    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    auto makeInstanced = [&](GLuint& vao, GLuint& vbo, int vec4s) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        setupInstanceAttribs(1, vec4s, vec4s * 4);
    };
    glGenVertexArrays(1, &quadVao_);
    glBindVertexArray(quadVao_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
    makeInstanced(creatureVao_, creatureVbo_, 6);
    makeInstanced(corpseVao_, corpseVbo_, 2);
    makeInstanced(particleVao_, particleVbo_, 2);

    glGenVertexArrays(1, &overlayVao_);
    glBindVertexArray(overlayVao_);
    glGenBuffers(1, &overlayVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), reinterpret_cast<void*>(8));
    glBindVertexArray(0);

    glGenTextures(1, &staticTex_);
    glGenTextures(1, &dynTex_);
    glGenTextures(1, &territoryTex_);
    glGenTextures(1, &rainTex_);
    glBindTexture(GL_TEXTURE_2D, territoryTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    minimap_.create(256, 256, 1, GL_RGBA8);
    return true;
}

void Renderer::shutdown() {
    scene_.destroy();
    blurA_.destroy();
    blurB_.destroy();
    minimap_.destroy();
    portrait_[0].destroy();
    portrait_[1].destroy();
    iconAtlas_.destroy();
    GLuint bufs[] = {quadVbo_, creatureVbo_, corpseVbo_, particleVbo_, overlayVbo_};
    glDeleteBuffers(5, bufs);
    GLuint vaos[] = {quadVao_, creatureVao_, corpseVao_, particleVao_, overlayVao_};
    glDeleteVertexArrays(5, vaos);
    glDeleteTextures(1, &staticTex_);
    glDeleteTextures(1, &dynTex_);
    glDeleteTextures(1, &territoryTex_);
    glDeleteTextures(1, &rainTex_);
    for (auto* p : {&terrainProg_, &creatureProg_, &corpseProg_, &particleProg_, &overlayProg_, &blurProg_,
                    &compositeProg_})
        glDeleteProgram(p->id);
}

void Renderer::ensureTargets(int w, int h) {
    if (scene_.w == w && scene_.h == h && scene_.fbo) return;
    scene_.create(w, h, 2, GL_RGBA16F);
    blurA_.create(std::max(1, w / 2), std::max(1, h / 2), 1, GL_RGBA16F);
    blurB_.create(std::max(1, w / 2), std::max(1, h / 2), 1, GL_RGBA16F);
}

void Renderer::setCameraUniforms(const gl::Program& p, const Camera& cam) const {
    p.set("uCenter", cam.center.x, cam.center.y);
    p.set("uZoom", cam.zoom);
    p.set("uViewPx", cam.viewCenter.x, cam.viewCenter.y);
    p.set("uFb", cam.fbSize.x, cam.fbSize.y);
}

void Renderer::uploadTerrain(const World& world) {
    const Terrain& t = world.terrain;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t.version != terrainVersion_ || t.cols != terrainW_ || t.rows != terrainH_) {
        terrainVersion_ = t.version;
        terrainW_ = t.cols;
        terrainH_ = t.rows;
        std::vector<float> px(size_t(t.cols) * t.rows * 4);
        for (size_t i = 0; i < size_t(t.cols) * t.rows; ++i) {
            px[i * 4 + 0] = t.elevation[i];
            px[i * 4 + 1] = t.fertility[i];
            px[i * 4 + 2] = clamp01((t.baseTemp[i] + 1.5f) / 3.f);
            px[i * 4 + 3] = t.moisture[i];
        }
        glBindTexture(GL_TEXTURE_2D, staticTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, t.cols, t.rows, 0, GL_RGBA, GL_FLOAT, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!t.rainTile.empty()) {
            glBindTexture(GL_TEXTURE_2D, rainTex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, Terrain::RAIN_TILE, Terrain::RAIN_TILE, 0, GL_RED, GL_FLOAT, t.rainTile.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
        glBindTexture(GL_TEXTURE_2D, dynTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, t.cols, t.rows, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        minimapDirty_ = true;
    }
    const size_t n = size_t(t.cols) * t.rows;
    dynPixels_.resize(n * 2);
    const float inv = 255.f / std::max(0.01f, world.params.plantCapacity);
    for (size_t i = 0; i < n; ++i) {
        dynPixels_[i * 2] = uint8_t(std::min(255.f, t.plant[i] * inv));
        dynPixels_[i * 2 + 1] = uint8_t(std::min(255.f, t.pheromone[i] * 127.f));
    }
    glBindTexture(GL_TEXTURE_2D, dynTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.cols, t.rows, GL_RG, GL_UNSIGNED_BYTE, dynPixels_.data());
}

void Renderer::updateTerritories(const World& world) {
    // 1) dominant species per coarse cell (sort (cell, species) keys, longest run wins)
    // 2) neighbourhood vote so ranges read as smooth regions instead of speckles
    const int N = 64;
    const float sx = float(N) / world.terrain.width, sy = float(N) / world.terrain.height;
    territoryKeys_.clear();
    for (const Creature& c : world.creatures) {
        int cx = std::clamp(int(c.pos.x * sx), 0, N - 1), cy = std::clamp(int(c.pos.y * sy), 0, N - 1);
        territoryKeys_.push_back((uint64_t(cy * N + cx) << 32) | c.speciesId);
    }
    std::sort(territoryKeys_.begin(), territoryKeys_.end());
    std::vector<uint32_t> domSpecies(size_t(N) * N, 0);
    std::vector<int> domCount(size_t(N) * N, 0);
    for (size_t a = 0; a < territoryKeys_.size();) {
        size_t b = a;
        while (b < territoryKeys_.size() && territoryKeys_[b] == territoryKeys_[a]) ++b;
        int cell = int(territoryKeys_[a] >> 32);
        if (int(b - a) > domCount[size_t(cell)]) {
            domCount[size_t(cell)] = int(b - a);
            domSpecies[size_t(cell)] = uint32_t(territoryKeys_[a] & 0xffffffffu);
        }
        a = b;
    }
    territoryPixels_.assign(size_t(N) * N * 4, 0);
    const int R = 2;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            uint32_t cand[25];
            float score[25];
            int nc = 0;
            float total = 0.f;
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                    int cell = yy * N + xx;
                    if (!domCount[size_t(cell)]) continue;
                    float wgt = float(domCount[size_t(cell)]) / (1.f + float(dx * dx + dy * dy));
                    total += wgt;
                    int k = 0;
                    while (k < nc && cand[k] != domSpecies[size_t(cell)]) ++k;
                    if (k == nc) { cand[nc] = domSpecies[size_t(cell)]; score[nc++] = 0.f; }
                    score[k] += wgt;
                }
            if (!nc) continue;
            int best = 0;
            for (int k = 1; k < nc; ++k)
                if (score[k] > score[best]) best = k;
            if (!world.species.valid(cand[best])) continue;
            Color col = world.species.get(cand[best]).color;
            uint8_t* px = &territoryPixels_[size_t(y * N + x) * 4];
            px[0] = uint8_t(col.r * 255.f);
            px[1] = uint8_t(col.g * 255.f);
            px[2] = uint8_t(col.b * 255.f);
            px[3] = uint8_t(255.f * smoothstep(0.5f, 4.f, total));
        }
    // Spread colour into empty neighbours (alpha stays 0) so bilinear filtering has no dark fringes.
    for (int pass = 0; pass < 2; ++pass)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                uint8_t* px = &territoryPixels_[size_t(y * N + x) * 4];
                if (px[3] || px[0] || px[1] || px[2]) continue;
                for (int d = 0; d < 4; ++d) {
                    int xx = x + (d == 0) - (d == 1), yy = y + (d == 2) - (d == 3);
                    if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                    const uint8_t* q = &territoryPixels_[size_t(yy * N + xx) * 4];
                    if (q[0] || q[1] || q[2]) { px[0] = q[0]; px[1] = q[1]; px[2] = q[2]; break; }
                }
            }
    glBindTexture(GL_TEXTURE_2D, territoryTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, territoryPixels_.data());
}

void Renderer::drawTerrain(const World& world, const Camera& cam, const RenderSettings& rs, bool minimap) {
    terrainProg_.use();
    setCameraUniforms(terrainProg_, cam);
    terrainProg_.set("uWorld", world.terrain.width, world.terrain.height);
    terrainProg_.set("uTexel", 1.f / float(world.terrain.cols), 1.f / float(world.terrain.rows));
    terrainProg_.set("uSea", world.terrain.seaLevel);
    terrainProg_.set("uTime", time_);
    terrainProg_.set("uSeasonSin", world.seasonSin());
    terrainProg_.set("uSeasonAmp", 0.45f * world.params.seasonStrength);
    terrainProg_.set("uClimate", world.climate());
    terrainProg_.set("uOverlay", minimap ? 0 : int(rs.overlay));
    terrainProg_.set("uPher", (!minimap && rs.showPheromones) ? 1 : 0);
    terrainProg_.set("uStatic", 0);
    terrainProg_.set("uDyn", 1);
    terrainProg_.set("uTerritory", 2);
    terrainProg_.set("uRain", 3);
    terrainProg_.set("uRainScale", world.rainScale());
    { Vec2 ro = world.rainOffset(); terrainProg_.set("uRainOffset", ro.x, ro.y); }
    terrainProg_.set("uClouds", world.params.weather ? ((rs.clouds && !minimap) ? 1 : 0) : -1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, staticTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, dynTex_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, territoryTex_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, rainTex_);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

Color Renderer::creatureColor(const World& w, const Creature& c, ColorMode mode) const {
    switch (mode) {
        case ColorMode::Genetic: return hsv(c.genome.g[G_HUE], 0.55f + 0.2f * c.genome.g[G_PATTERN], 0.95f);
        case ColorMode::Species:
            return w.species.valid(c.speciesId) ? w.species.get(c.speciesId).color : Color{0.8f, 0.8f, 0.8f, 1};
        case ColorMode::Diet: {
            float d = c.genome.g[G_DIET];
            Color herb{0.35f, 0.85f, 0.35f, 1}, omni{0.95f, 0.85f, 0.3f, 1}, carn{0.95f, 0.25f, 0.22f, 1};
            return d < 0.5f ? mix(herb, omni, d * 2.f) : mix(omni, carn, (d - 0.5f) * 2.f);
        }
        case ColorMode::Energy: {
            float e = c.energy / c.maxEnergy;
            return e < 0.5f ? mix({0.9f, 0.2f, 0.2f, 1}, {0.95f, 0.85f, 0.3f, 1}, e * 2.f)
                            : mix({0.95f, 0.85f, 0.3f, 1}, {0.3f, 0.9f, 0.45f, 1}, (e - 0.5f) * 2.f);
        }
        case ColorMode::Generation:
            return viridis(w.maxGeneration ? float(c.generation) / float(w.maxGeneration) : 0.f);
        case ColorMode::Age: return mix({0.75f, 1.f, 0.75f, 1}, {0.45f, 0.4f, 0.5f, 1}, clamp01(c.age / c.ph.lifespan));
        case ColorMode::Thermal: {
            float t = c.genome.g[G_THERMAL];
            return t < 0.5f ? mix({0.3f, 0.5f, 1.f, 1}, {0.9f, 0.9f, 0.9f, 1}, t * 2.f)
                            : mix({0.9f, 0.9f, 0.9f, 1}, {1.f, 0.35f, 0.2f, 1}, (t - 0.5f) * 2.f);
        }
        case ColorMode::Toxin: return mix({0.55f, 0.55f, 0.6f, 1}, {0.7f, 1.f, 0.15f, 1}, c.genome.g[G_TOXIN]);
        case ColorMode::Brain: return viridis(float(c.ph.connections) / 300.f);
        case ColorMode::Disease:
            if (c.infection > 0.f) return {0.6f, 1.f, 0.3f, 1};
            if (c.immuneTicks > 0.f) return {1.f, 0.78f, 0.3f, 1};
            return mix({0.45f, 0.55f, 0.75f, 1}, {0.85f, 0.85f, 1.f, 1}, c.ph.immunity);
        default: return {1, 1, 1, 1};
    }
}

void Renderer::drawCorpses(const World& world, const Camera& cam) {
    corpseInst_.clear();
    const Vec2 tl = cam.screenToWorld({0, 0}), br = cam.screenToWorld(cam.fbSize);
    for (const Corpse& m : world.corpses) {
        if (m.pos.x + m.radius < tl.x || m.pos.x - m.radius > br.x || m.pos.y + m.radius < tl.y ||
            m.pos.y - m.radius > br.y)
            continue;
        float seed = std::fmod(m.pos.x * 0.137f + m.pos.y * 0.071f, 1.f);
        corpseInst_.push_back({m.pos.x, m.pos.y, m.radius, clamp01(m.energy / std::max(1.f, m.initial)), m.hue,
                               m.toxin, seed, 0.f});
    }
    if (corpseInst_.empty()) return;
    corpseProg_.use();
    setCameraUniforms(corpseProg_, cam);
    glBindVertexArray(corpseVao_);
    glBindBuffer(GL_ARRAY_BUFFER, corpseVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(corpseInst_.size() * sizeof(CorpseInst)), corpseInst_.data(),
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(corpseInst_.size()));
}

Renderer::CreatureInst Renderer::makeInstance(const World& world, const Creature& c, const RenderSettings& rs) const {
    Color col = creatureColor(world, c, rs.colorMode);
    float alpha = 1.f;
    if (rs.highlightSpecies && c.speciesId != rs.highlightSpecies) {
        alpha = 0.28f;
        col = mix(col, {0.5f, 0.5f, 0.5f, 1}, 0.6f);
    }
    uint32_t flags = c.flags;
    if (c.id && c.id == rs.selectedId) flags |= RF_SELECTED;
    else if (c.id && c.id == rs.hoverId) flags |= RF_HOVER;
    return {c.pos.x, c.pos.y, c.angle, c.radius * rs.creatureScale,
            col.r, col.g, col.b, alpha,
            c.genome.g[G_PATTERN], c.ph.diet, c.ph.armor, c.ph.weapon,
            c.ph.fov, c.animPhase, c.signal, float(flags),
            c.health / c.maxHealth, c.genome.g[G_VISION_RANGE], c.out[OUT_THRUST], c.ph.toxin,
            c.ph.nightVision, c.infection > 0.f ? 1.f : 0.f, c.angle, c.growth};
}

void Renderer::drawCreatureInto(const CreatureInst& in, int x, int y, int size, float time) {
    // Renders one creature, facing up, centred in the given square of the currently bound framebuffer.
    CreatureInst inst = in;
    inst.x = inst.y = 0.f;
    inst.a = 1.f;
    if (inst.thrust == 0.f) inst.thrust = 0.5f;
    Camera cam;
    cam.center = {0.f, 0.42f * inst.radius};
    cam.zoom = float(size) / (inst.radius * 3.3f);
    cam.viewCenter = {float(size) * 0.5f, float(size) * 0.5f};
    cam.fbSize = {float(size), float(size)};
    glViewport(x, y, size, size);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, size, size);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    creatureProg_.use();
    setCameraUniforms(creatureProg_, cam);
    creatureProg_.set("uTime", time);
    creatureProg_.set("uSignals", 0);
    creatureProg_.set("uNight", 0.f);
    creatureProg_.set("uBlob", blobStyleForIcons_ ? 1 : 0);
    glBindVertexArray(creatureVao_);
    glBindBuffer(GL_ARRAY_BUFFER, creatureVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(CreatureInst), &inst, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

GLuint Renderer::portrait(int slot, const World& world, const Creature& c, const RenderSettings& rs, float time) {
    gl::Target& t = portrait_[slot & 1];
    if (!t.fbo) t.create(192, 192, 1, GL_RGBA8);
    RenderSettings prs = rs;
    prs.highlightSpecies = 0;
    prs.selectedId = prs.hoverId = 0;
    CreatureInst inst = makeInstance(world, c, prs);
    inst.angle = -PI * 0.5f + 0.25f * std::sin(time * 0.8f);
    inst.radius = c.ph.adultRadius;
    inst.phase = time * 6.f;
    blobStyleForIcons_ = rs.blobStyle;
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    drawCreatureInto(inst, 0, 0, 192, time);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prevFbo));
    return t.tex[0];
}

bool Renderer::speciesIcon(const World& world, uint32_t speciesId, GLuint& tex, float uv[4]) {
    constexpr int CELL = 64, GRID = 16; // 256 cached icons
    if (!iconAtlas_.fbo) {
        iconAtlas_.create(CELL * GRID, CELL * GRID, 1, GL_RGBA8);
        iconSlots_.assign(GRID * GRID, IconSlot{});
        GLint prev = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_FRAMEBUFFER, iconAtlas_.fbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
    }
    if (!world.species.valid(speciesId)) return false;
    ++iconClock_;
    int slot = -1;
    for (int k = 0; k < int(iconSlots_.size()); ++k)
        if (iconSlots_[size_t(k)].species == speciesId) slot = k;
    if (slot < 0) {
        if (iconBudget_ <= 0) return false; // spread rendering over frames
        --iconBudget_;
        slot = 0;
        for (int k = 1; k < int(iconSlots_.size()); ++k)
            if (iconSlots_[size_t(k)].lastUse < iconSlots_[size_t(slot)].lastUse) slot = k;
        iconSlots_[size_t(slot)].species = speciesId;
        Creature c = world.preview(world.species.get(speciesId).rep);
        RenderSettings rs;
        CreatureInst inst = makeInstance(world, c, rs);
        inst.angle = -PI * 0.5f;
        inst.radius = c.ph.adultRadius;
        GLint prev = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_FRAMEBUFFER, iconAtlas_.fbo);
        drawCreatureInto(inst, (slot % GRID) * CELL, (slot / GRID) * CELL, CELL, 1.f);
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
    }
    iconSlots_[size_t(slot)].lastUse = iconClock_;
    tex = iconAtlas_.tex[0];
    const float s = 1.f / GRID;
    uv[0] = float(slot % GRID) * s;
    uv[1] = float(slot / GRID + 1) * s; // flipped vertically for ImGui
    uv[2] = uv[0] + s;
    uv[3] = float(slot / GRID) * s;
    return true;
}

void Renderer::drawCreatures(const World& world, const Camera& cam, const RenderSettings& rs) {
    creatureInst_.clear();
    const Vec2 tl = cam.screenToWorld({0, 0}), br = cam.screenToWorld(cam.fbSize);
    const float margin = 40.f;
    for (const Creature& c : world.creatures) {
        if (c.pos.x < tl.x - margin || c.pos.x > br.x + margin || c.pos.y < tl.y - margin || c.pos.y > br.y + margin)
            continue;
        creatureInst_.push_back(makeInstance(world, c, rs));
        // Keep distant herds visible as dots when zoomed far out.
        creatureInst_.back().radius = std::max(creatureInst_.back().radius, rs.minCreaturePx / cam.zoom);
    }
    // Draw the selected creature last so its ring sits on top.
    auto sel = std::find_if(creatureInst_.begin(), creatureInst_.end(),
                            [](const CreatureInst& ci) { return (uint32_t(ci.flags) & RF_SELECTED) != 0; });
    if (sel != creatureInst_.end()) std::iter_swap(sel, creatureInst_.end() - 1);
    if (creatureInst_.empty()) return;
    creatureProg_.use();
    setCameraUniforms(creatureProg_, cam);
    creatureProg_.set("uTime", time_);
    creatureProg_.set("uSignals", rs.showSignals ? 1 : 0);
    creatureProg_.set("uNight", night_);
    creatureProg_.set("uBlob", rs.blobStyle ? 1 : 0);
    glBindVertexArray(creatureVao_);
    glBindBuffer(GL_ARRAY_BUFFER, creatureVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(creatureInst_.size() * sizeof(CreatureInst)), creatureInst_.data(),
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(creatureInst_.size()));
}

void Renderer::consumeFx(World& world, const RenderSettings& rs) {
    if (!rs.particles) {
        world.fx.clear();
        return;
    }
    auto spawn = [&](Vec2 p, Vec2 v, float life, float size, float kind, Color c) {
        if (particles_.size() < 8000) particles_.push_back({p, v, life, life, size, kind, c});
    };
    for (const FxEvent& e : world.fx) {
        switch (e.kind) {
            case FxEvent::Birth:
                for (int k = 0; k < 4; ++k) {
                    float a = rng_.range(0, TAU), s = rng_.range(8.f, 25.f);
                    spawn(e.pos, Vec2(std::cos(a), std::sin(a)) * s, rng_.range(0.5f, 0.9f), e.size * 0.45f, 2.f,
                          e.color);
                }
                break;
            case FxEvent::Death:
                for (int k = 0; k < 6; ++k) {
                    float a = rng_.range(0, TAU), s = rng_.range(4.f, 14.f);
                    spawn(e.pos, Vec2(std::cos(a), std::sin(a)) * s, rng_.range(0.8f, 1.4f), e.size * 0.5f, 0.f,
                          {0.6f, 0.58f, 0.55f, 0.5f});
                }
                break;
            case FxEvent::Hit:
                for (int k = 0; k < 3; ++k) {
                    float a = rng_.range(0, TAU), s = rng_.range(15.f, 40.f);
                    spawn(e.pos, Vec2(std::cos(a), std::sin(a)) * s, rng_.range(0.25f, 0.45f), e.size * 0.3f, 0.f,
                          e.color);
                }
                break;
            case FxEvent::Poison:
                for (int k = 0; k < 3; ++k)
                    spawn(e.pos + Vec2(rng_.range(-5.f, 5.f), 0), Vec2(rng_.range(-4.f, 4.f), -rng_.range(10.f, 20.f)),
                          rng_.range(0.6f, 1.f), e.size * 0.3f, 0.f, e.color);
                break;
            case FxEvent::Sick:
                spawn(e.pos + Vec2(rng_.range(-4.f, 4.f), 0), Vec2(rng_.range(-3.f, 3.f), -rng_.range(8.f, 16.f)),
                      rng_.range(0.8f, 1.3f), e.size * 0.25f, 0.f, e.color);
                break;
            case FxEvent::Love:
                for (int k = 0; k < 2; ++k)
                    spawn(e.pos + Vec2(rng_.range(-6.f, 6.f), 0), Vec2(rng_.range(-5.f, 5.f), -rng_.range(14.f, 24.f)),
                          rng_.range(1.f, 1.5f), std::max(4.f, e.size * 0.55f), 3.f, e.color);
                break;
            case FxEvent::Gift:
                spawn(e.pos, Vec2(0, -14.f), 0.9f, e.size * 0.5f, 2.f, e.color);
                break;
            case FxEvent::Speciation:
                spawn(e.pos, {}, 1.6f, e.size, 1.f, e.color);
                for (int k = 0; k < 14; ++k) {
                    float a = rng_.range(0, TAU), s = rng_.range(10.f, 45.f);
                    spawn(e.pos, Vec2(std::cos(a), std::sin(a)) * s, rng_.range(0.8f, 1.5f), 6.f, 2.f, e.color);
                }
                break;
            case FxEvent::Meteor:
                spawn(e.pos, {}, 2.5f, e.size * 1.6f, 1.f, {1.f, 0.65f, 0.25f, 1.f});
                spawn(e.pos, {}, 1.8f, e.size * 1.1f, 1.f, {1.f, 0.9f, 0.6f, 1.f});
                for (int k = 0; k < 90; ++k) {
                    float a = rng_.range(0, TAU), s = rng_.range(30.f, 260.f);
                    spawn(e.pos, Vec2(std::cos(a), std::sin(a)) * s, rng_.range(0.8f, 2.4f), rng_.range(6.f, 22.f),
                          0.f, mix({1.f, 0.85f, 0.4f, 1}, {0.9f, 0.25f, 0.1f, 1}, rng_.uniform()));
                }
                break;
        }
    }
    world.fx.clear();
}

void Renderer::drawParticles(const Camera& cam, float dt) {
    particleInst_.clear();
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.life -= dt;
        if (p.life <= 0.f) {
            particles_[i] = particles_.back();
            particles_.pop_back();
            continue;
        }
        p.pos += p.vel * dt;
        p.vel *= std::exp(-dt * 2.5f);
        float t = p.life / p.maxLife;
        float size = p.kind == 1.f ? p.size * (1.f - t * t * 0.9f) : p.size * (0.4f + 0.6f * t);
        particleInst_.push_back({p.pos.x, p.pos.y, size, p.kind, p.color.r, p.color.g, p.color.b, p.color.a * t});
        ++i;
    }
    if (particleInst_.empty()) return;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    particleProg_.use();
    setCameraUniforms(particleProg_, cam);
    glBindVertexArray(particleVao_);
    glBindBuffer(GL_ARRAY_BUFFER, particleVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(particleInst_.size() * sizeof(ParticleInst)), particleInst_.data(),
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(particleInst_.size()));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::line(Vec2 a, Vec2 b, float width, Color c) {
    Vec2 d = b - a;
    float len = d.length();
    if (len < 1e-5f) return;
    Vec2 n = Vec2(-d.y, d.x) * (width * 0.5f / len);
    uint32_t col = packRGBA8(c);
    Vtx v0{a.x + n.x, a.y + n.y, col}, v1{a.x - n.x, a.y - n.y, col}, v2{b.x + n.x, b.y + n.y, col},
        v3{b.x - n.x, b.y - n.y, col};
    overlayVerts_.insert(overlayVerts_.end(), {v0, v1, v2, v2, v1, v3});
}

void Renderer::circle(Vec2 c, float r, float width, Color col, int segments) {
    for (int i = 0; i < segments; ++i) {
        float a0 = TAU * float(i) / float(segments), a1 = TAU * float(i + 1) / float(segments);
        line(c + Vec2(std::cos(a0), std::sin(a0)) * r, c + Vec2(std::cos(a1), std::sin(a1)) * r, width, col);
    }
}

void Renderer::disc(Vec2 c, float r, Color col, int segments) { sector(c, r, 0.f, TAU, col), (void)segments; }

void Renderer::sector(Vec2 c, float r, float a0, float a1, Color col) {
    uint32_t pc = packRGBA8(col);
    int segs = std::max(3, int(std::fabs(a1 - a0) / TAU * 48.f));
    for (int i = 0; i < segs; ++i) {
        float t0 = a0 + (a1 - a0) * float(i) / float(segs), t1 = a0 + (a1 - a0) * float(i + 1) / float(segs);
        overlayVerts_.push_back({c.x, c.y, pc});
        overlayVerts_.push_back({c.x + std::cos(t0) * r, c.y + std::sin(t0) * r, pc});
        overlayVerts_.push_back({c.x + std::cos(t1) * r, c.y + std::sin(t1) * r, pc});
    }
}

void Renderer::drawOverlays(const World& world, const Camera& cam, const RenderSettings& rs) {
    overlayVerts_.clear();
    const float px = 1.f / cam.zoom;
    // World border.
    const float W = world.terrain.width, H = world.terrain.height;
    Color border{0.55f, 0.75f, 0.8f, 0.35f};
    line({0, 0}, {W, 0}, 2 * px, border);
    line({W, 0}, {W, H}, 2 * px, border);
    line({W, H}, {0, H}, 2 * px, border);
    line({0, H}, {0, 0}, 2 * px, border);

    int sel = world.indexOfId(rs.selectedId);
    if (rs.selectedId != trailId_) {
        trail_.clear();
        trailId_ = rs.selectedId;
    }
    if (sel >= 0) {
        const Creature& c = world.creatures[sel];
        if (trail_.empty() || (trail_.back() - c.pos).lengthSq() > 9.f) trail_.push_back(c.pos);
        while (trail_.size() > 600) trail_.pop_front();
        if (rs.showTrails && trail_.size() > 1) {
            for (size_t i = 1; i < trail_.size(); ++i) {
                float t = float(i) / float(trail_.size());
                if ((trail_[i] - trail_[i - 1]).lengthSq() > 400.f * 400.f) continue;
                line(trail_[i - 1], trail_[i], (1.f + 2.f * t) * px, {1.f, 0.85f, 0.4f, 0.55f * t});
            }
        }
        if (rs.showVision) {
            const float R = c.ph.visionRange, half = c.ph.fov * 0.5f, w = c.ph.fov / NUM_EYES;
            for (int e = 0; e < NUM_EYES; ++e) {
                const float* eye = c.in + e * EC_COUNT;
                float a0 = c.angle - half + w * float(e), a1 = a0 + w;
                Color col{0.8f, 0.9f, 1.f, 0.05f};
                float best = 0.f;
                auto consider = [&](float v, Color cc) {
                    if (v > best) { best = v; col = cc; }
                };
                consider(eye[EC_PLANT] * 0.8f, {0.4f, 1.f, 0.4f, 1});
                consider(eye[EC_MEAT], {1.f, 0.4f, 0.35f, 1});
                consider(eye[EC_CREATURE], {1.f, 0.85f, 0.3f, 1});
                consider(eye[EC_OBSTACLE], {0.35f, 0.6f, 1.f, 1});
                col.a = 0.06f + 0.22f * best;
                sector(c.pos, R, a0, a1, col);
                Vec2 d0 = fromAngle(a0);
                line(c.pos, c.pos + d0 * R, 1.f * px, {1, 1, 1, 0.18f});
            }
            line(c.pos, c.pos + fromAngle(c.angle - half + c.ph.fov) * R, 1.f * px, {1, 1, 1, 0.18f});
            // Hearing radius.
            circle(c.pos, std::min(R, 160.f) + 40.f, 1.f * px, {0.6f, 0.9f, 1.f, 0.15f}, 64);
        }
    }
    if (rs.highlightSpecies && world.species.valid(rs.highlightSpecies)) {
        const Species& s = world.species.get(rs.highlightSpecies);
        // Migration path: the species' range centre over its last ~120 samples, fading into the past.
        const size_t n = s.track.size();
        const size_t from = n > 120 ? n - 120 : 0;
        for (size_t k = from + 1; k < n; ++k) {
            float t = float(k - from) / float(std::max<size_t>(1, n - from));
            Color lc = s.color;
            lc.a = 0.15f + 0.65f * t;
            line(s.track[k - 1], s.track[k], (1.5f + 2.5f * t) * px, lc);
        }
        if (n > 1) circle(s.track[from], 6.f * px, 1.5f * px, {1, 1, 1, 0.35f}, 16);
        if (s.alive()) {
            Color sc = s.color;
            sc.a = 0.8f;
            circle(s.centroid, 14.f * px, 2.5f * px, sc, 24);
        }
    }
    if (rs.brushVisible) {
        circle(rs.brushPos, rs.brushRadius, 2.f * px, rs.brushColor, 72);
        Color fill = rs.brushColor;
        fill.a *= 0.12f;
        sector(rs.brushPos, rs.brushRadius, 0, TAU, fill);
    }
    if (overlayVerts_.empty()) return;
    overlayProg_.use();
    setCameraUniforms(overlayProg_, cam);
    glBindVertexArray(overlayVao_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(overlayVerts_.size() * sizeof(Vtx)), overlayVerts_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(overlayVerts_.size()));
}

void Renderer::bloomAndComposite(const RenderSettings& rs, GLuint targetFbo, int w, int h) {
    glDisable(GL_BLEND);
    glBindVertexArray(quadVao_);
    if (rs.bloom) {
        blurProg_.use();
        blurProg_.set("uTex", 0);
        blurProg_.set("uFb", float(blurA_.w), float(blurA_.h));
        glViewport(0, 0, blurA_.w, blurA_.h);
        GLuint src = scene_.tex[1];
        for (int pass = 0; pass < 3; ++pass) {
            float spread = 1.f + float(pass) * 1.5f;
            glBindFramebuffer(GL_FRAMEBUFFER, blurA_.fbo);
            glBindTexture(GL_TEXTURE_2D, src);
            blurProg_.set("uDir", spread / float(blurA_.w), 0.f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindFramebuffer(GL_FRAMEBUFFER, blurB_.fbo);
            glBindTexture(GL_TEXTURE_2D, blurA_.tex[0]);
            blurProg_.set("uDir", 0.f, spread / float(blurA_.h));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            src = blurB_.tex[0];
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, w, h);
    compositeProg_.use();
    compositeProg_.set("uFb", float(w), float(h));
    compositeProg_.set("uScene", 0);
    compositeProg_.set("uBloom", 1);
    compositeProg_.set("uBloomStrength", rs.bloom ? rs.bloomStrength : 0.f);
    compositeProg_.set("uVignette", rs.vignette ? 0.55f : 0.f);
    compositeProg_.set("uAmbient", ambient_.r, ambient_.g, ambient_.b);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_.tex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurB_.tex[0]);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::render(const World& world, const Camera& cam, const RenderSettings& rs, float dt, GLuint targetFbo) {
    time_ += dt;
    ++frame_;
    {
        // Ambient light: white noon, warm dusk, deep blue night. Glows are added after, so they pop at night.
        float L = world.daylight();
        // Data overlays stay readable at night.
        float dark = (1.f - L) * rs.nightDarkness * (rs.overlay != Overlay::None ? 0.3f : 1.f);
        Color night{0.17f, 0.21f, 0.38f, 1}, dusk{1.f, 0.78f, 0.6f, 1}, day{1, 1, 1, 1};
        float duskAmt = (1.f - std::fabs(L - 0.5f) * 2.f) * rs.nightDarkness * 0.6f;
        Color a = mix(day, night, dark / std::max(0.001f, rs.nightDarkness) * (rs.nightDarkness > 0 ? 1.f : 0.f));
        a = mix(day, a, rs.nightDarkness);
        a = mix(a, Color{a.r * dusk.r, a.g * dusk.g, a.b * dusk.b, 1}, duskAmt);
        ambient_ = a;
        night_ = dark;
    }
    const int w = int(cam.fbSize.x), h = int(cam.fbSize.y);
    if (w <= 0 || h <= 0) return;
    ensureTargets(w, h);
    uploadTerrain(world);
    if (rs.overlay == Overlay::Territories && (frame_ % 15 == 1 || territoryPixels_.empty())) updateTerritories(world);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    if (minimapDirty_ || frame_ % 30 == 0) {
        minimapDirty_ = false;
        glBindFramebuffer(GL_FRAMEBUFFER, minimap_.fbo);
        glViewport(0, 0, minimap_.w, minimap_.h);
        Camera mc;
        float s = float(minimap_.w) / std::max(world.terrain.width, world.terrain.height);
        mc.center = {world.terrain.width * 0.5f, world.terrain.height * 0.5f};
        mc.zoom = s;
        mc.viewCenter = {minimap_.w * 0.5f, minimap_.h * 0.5f};
        mc.fbSize = {float(minimap_.w), float(minimap_.h)};
        glDisable(GL_BLEND);
        drawTerrain(world, mc, rs, true);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    drawTerrain(world, cam, rs, false);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (rs.showCorpses) drawCorpses(world, cam);
    drawOverlays(world, cam, rs);
    drawCreatures(world, cam, rs);
    drawParticles(cam, dt);
    bloomAndComposite(rs, targetFbo, w, h);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_BLEND);
}

} // namespace pm
