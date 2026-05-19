#version 330 core
// ============================================================================
// pbr.frag — Cook-Torrance PBR BRDF
//
// Формулы:
//   NDF  (GGX):      D(h)   = α² / (π · ((N·H)²·(α²-1)+1)²)
//   Геометрия (Smith-GGX):  G = G1(N·V) · G1(N·L)
//   G1 (Schlick-GGX): G1(v) = (N·v) / ((N·v)·(1-k)+k),   k=(α+1)²/8
//   Fresnel (Schlick): F(h,v) = F0 + (1-F0)·(1 - V·H)⁵
//   BRDF: Lo = (kD·albedo/π + D·G·F / (4·NdotV·NdotL)) · radiance · NdotL
//
// TBN строится через dFdx/dFdy без атрибутов касательных на CPU.
// Источник: point light с квадратичным затуханием (1/d²).
// Тональная компрессия: Reinhard + гамма-коррекция 2.2.
// ============================================================================

in vec2 vUV;
in vec3 vNormal;
in vec3 vWorldPos;

out vec4 FragColor;

// Четыре PBR-текстуры (юниты 0–3 — те же, что в 2D-режиме)
uniform sampler2D uDiffuse;    // юнит 0 — альбедо RGB
uniform sampler2D uHeight;     // юнит 1 — высота R (не участвует в освещении)
uniform sampler2D uNormal;     // юнит 2 — tangent-space нормаль RGB
uniform sampler2D uRoughness;  // юнит 3 — шероховатость R

// Камера
uniform vec3 uViewPos;

// Источник света (point light)
uniform vec3  uLightPos;
uniform vec3  uLightColor;
uniform float uLightIntensity;

// Фоновый свет
uniform vec3 uAmbientColor;

const float PI = 3.14159265359;

// ============================================================
// GGX Normal Distribution Function
// Отвечает за "размытость" зеркального блика — чем больше
// roughness, тем шире и тусклее блик.
// ============================================================
float distributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;   // α = r²  (Disney remapping)
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0001);
}

// ============================================================
// Геометрическая функция Шлика-GGX (один вектор)
// Моделирует самозатенение микрограней.
// k=(r+1)²/8 — формула для прямого освещения (не IBL).
// ============================================================
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// ============================================================
// Smith Geometry: учитывает затенение и со стороны взгляда,
// и со стороны света.
// ============================================================
float geometrySmith(float NdotV, float NdotL, float roughness)
{
    return geometrySchlickGGX(NdotV, roughness)
         * geometrySchlickGGX(NdotL, roughness);
}

// ============================================================
// Fresnel Schlick
// F0 — базовое отражение при нормальном падении.
// При скользящих углах отражение → 1 (эффект Пула-Фарли).
// ============================================================
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    // ---- Материальные параметры из текстур ----
    // Переводим альбедо из sRGB в линейное пространство для физически корректных расчётов
    vec3  albedo    = pow(texture(uDiffuse,   vUV).rgb, vec3(2.2));
    float roughness = clamp(texture(uRoughness, vUV).r, 0.04, 1.0);

    // Металличность отсутствует в наборе карт — все материалы диэлектрики.
    // F0 диэлектрика ≈ 0.04 (вода/камень/дерево).
    const float metallic = 0.0;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---- Восстановление нормали через TBN (dFdx/dFdy) ----
    // Метод работает без атрибутов касательных на CPU.
    // Артефакт: на UV-шве сферы (u=0/1) производные некорректны —
    // это приемлемо для превью-инструмента.
    vec3 N = normalize(vNormal);

    vec3 dp1  = dFdx(vWorldPos);   // производная позиции по экранному X
    vec3 dp2  = dFdy(vWorldPos);   // производная позиции по экранному Y
    vec2 duv1 = dFdx(vUV);         // производная UV по экранному X
    vec2 duv2 = dFdy(vUV);         // производная UV по экранному Y

    // Ортогонализованная TBN (алгоритм: Mikkelsen / Knarkowski)
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    mat3 TBN = mat3(T * invmax, B * invmax, N);

    // Нормаль из tangent-space карты (распаковка [0,1] → [-1,1])
    vec3 normalSample = texture(uNormal, vUV).rgb * 2.0 - 1.0;
    N = normalize(TBN * normalSample);

    // ---- Векторы освещения ----
    vec3 V = normalize(uViewPos - vWorldPos);       // к камере
    vec3 L = normalize(uLightPos - vWorldPos);      // к источнику
    vec3 H = normalize(V + L);                      // half-vector

    float NdotV = max(dot(N, V), 0.0001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // ---- Cook-Torrance BRDF ----
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3  F = fresnelSchlick(VdotH, F0);

    // Зеркальная составляющая (specular lobe)
    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    // Диффузная составляющая: (1-F)·(1-metallic) исключает Fresnel-вклад из диффуза
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Квадратичное затухание point light: 1/d² (физически корректно)
    float dist        = length(uLightPos - vWorldPos);
    float attenuation = 1.0 / (dist * dist);
    vec3  radiance    = uLightColor * uLightIntensity * attenuation;

    // Итоговый вклад освещения
    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    // Фоновый свет (аппроксимация ambient occlusion через константу)
    vec3 ambient = uAmbientColor * albedo;

    vec3 color = ambient + Lo;

    // ---- Тональная компрессия Reinhard ----
    // Переводит HDR-значения в [0,1] без обрезания ярких бликов
    color = color / (color + vec3(1.0));

    // Гамма-коррекция: linear → sRGB
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}