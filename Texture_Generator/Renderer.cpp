// ============================================================================
// Renderer.cpp — Реализация современного рендерера OpenGL 3.3 Core
// GLFW + GLAD, VAO/VBO, встроенные GLSL-шейдеры, 4 PBR-текстуры
// ============================================================================

#include "Renderer.h"

#include <stdexcept>
#include <format>
#include <iostream>
#include <array>

// ============================================================================
// Исходники встроенных шейдеров
// ============================================================================

// Вершинный шейдер — fullscreen quad, передаёт UV во фрагментный
static const char* VERTEX_SRC = R"GLSL(
#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Фрагментный шейдер — выбирает одну из 4 карт PBR для отображения
static const char* FRAGMENT_SRC = R"GLSL(
#version 330 core

in vec2 vUV;
out vec4 FragColor;

// Четыре PBR-сэмплера
uniform sampler2D uDiffuse;
uniform sampler2D uHeight;
uniform sampler2D uNormal;
uniform sampler2D uRoughness;

// 0 = Diffuse, 1 = Height, 2 = Normal, 3 = Roughness
uniform int   uMapMode;

// Общая яркость (полезно для отладки тёмных карт)
uniform float uBrightness;

void main() {
    vec2 uv = vUV;  // UV уже в [0,1] с учётом ориентации quad

    vec4 color;

    if (uMapMode == 0) {
        // Диффузная карта — прямой RGB
        color = vec4(texture(uDiffuse, uv).rgb, 1.0);
    }
    else if (uMapMode == 1) {
        // Карта высот — grayscale (R канал → серый)
        float h = texture(uHeight, uv).r;
        color = vec4(h, h, h, 1.0);
    }
    else if (uMapMode == 2) {
        // Карта нормалей — RGB уже в [0,1] (нормали упакованы)
        // Синий канал близок к 1.0 для плоских поверхностей
        color = vec4(texture(uNormal, uv).rgb, 1.0);
    }
    else {
        // Карта шероховатости — grayscale (R канал → серый)
        float r = texture(uRoughness, uv).r;
        color = vec4(r, r, r, 1.0);
    }

    FragColor = color * uBrightness;
}
)GLSL";

// ============================================================================
// Конструктор — инициализация GLFW, GLAD, шейдеров, quad
// ============================================================================
Renderer::Renderer(int width, int height, const std::string& title)
{
    // --- 1. Инициализация GLFW ---
    if (!glfwInit()) {
        throw std::runtime_error("Ошибка: glfwInit() завершился неудачно.");
    }

    // Запрашиваем OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0); // MSAA отключён (текстура и так чёткая)
    glfwWindowHint(GLFW_DEPTH_BITS, 24); // явно запрашиваем depth buffer для 3D-режима

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("Ошибка: не удалось создать окно GLFW.");
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // VSync — 60 FPS

    // Клавиша ESC закрывает окно
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, [](GLFWwindow* w, int key, int, int action, int) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            glfwSetWindowShouldClose(w, GLFW_TRUE);
    });

    // --- 2. Инициализация GLAD (загрузчик расширений) ---
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
        throw std::runtime_error("Ошибка: gladLoadGLLoader() завершился неудачно.");
    }

    // Вывод версии для верификации
    std::cout << std::format("[Renderer] OpenGL: {} | Renderer: {}\n",
        reinterpret_cast<const char*>(glGetString(GL_VERSION)),
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // --- 3. Компиляция шейдерной программы ---
    GLuint vert = compileShader(GL_VERTEX_SHADER,   VERTEX_SRC);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    m_shader    = linkProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    // Кэшируем uniform locations
    m_uMapMode   = glGetUniformLocation(m_shader, "uMapMode");
    m_uDiffuse   = glGetUniformLocation(m_shader, "uDiffuse");
    m_uHeight    = glGetUniformLocation(m_shader, "uHeight");
    m_uNormal    = glGetUniformLocation(m_shader, "uNormal");
    m_uRoughness = glGetUniformLocation(m_shader, "uRoughness");
    m_uBrightness= glGetUniformLocation(m_shader, "uBrightness");

    // Привязываем сэмплеры к texture units (один раз при инициализации)
    glUseProgram(m_shader);
    glUniform1i(m_uDiffuse,   0);
    glUniform1i(m_uHeight,    1);
    glUniform1i(m_uNormal,    2);
    glUniform1i(m_uRoughness, 3);
    glUniform1f(m_uBrightness, 1.0f);
    glUseProgram(0);

    // --- 4. Создание fullscreen quad ---
    createQuad();
    loadPBRShader();     // компиляция PBR-шейдера
    createSphere(32, 24); // параметрическая сфера 32×24 сегмента
}

// ============================================================================
// Деструктор — освобождение всех GPU-ресурсов
// ============================================================================
Renderer::~Renderer()
{
    if (m_sphereVao)    glDeleteVertexArrays(1, &m_sphereVao);
    if (m_sphereVbo)    glDeleteBuffers(1, &m_sphereVbo);
    if (m_sphereEbo)    glDeleteBuffers(1, &m_sphereEbo);
    if (m_pbrShader)    glDeleteProgram(m_pbrShader);

    if (m_texDiffuse)   glDeleteTextures(1, &m_texDiffuse);
    if (m_texHeight)    glDeleteTextures(1, &m_texHeight);
    if (m_texNormal)    glDeleteTextures(1, &m_texNormal);
    if (m_texRoughness) glDeleteTextures(1, &m_texRoughness);

    if (m_vao)    glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)    glDeleteBuffers(1, &m_vbo);
    if (m_shader) glDeleteProgram(m_shader);

    glfwDestroyWindow(m_window);
    glfwTerminate();
}

// ============================================================================
// createQuad — VAO + VBO для fullscreen quad (2 треугольника)
// ============================================================================
void Renderer::createQuad()
{
    //  Формат каждой вершины: [posX, posY, uvX, uvY]
    //  NDC: (-1,-1) → (1,1) покрывает весь экран
    //  UV:  (0,0)   → (1,1) (левый нижний → правый верхний)
    //
    //  Два треугольника (CCW winding):
    //      (-1,+1) --- (+1,+1)
    //          |    ╲    |
    //      (-1,-1) --- (+1,-1)
    static constexpr std::array<float, 24> quadVerts = {
        // posX   posY   uvX   uvY
        -1.0f,  1.0f,  0.0f, 1.0f,  // верхний левый
        -1.0f, -1.0f,  0.0f, 0.0f,  // нижний левый
         1.0f, -1.0f,  1.0f, 0.0f,  // нижний правый

        -1.0f,  1.0f,  0.0f, 1.0f,  // верхний левый
         1.0f, -1.0f,  1.0f, 0.0f,  // нижний правый
         1.0f,  1.0f,  1.0f, 1.0f,  // верхний правый
    };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(quadVerts.size() * sizeof(float)),
                 quadVerts.data(),
                 GL_STATIC_DRAW);

    // layout(location=0): vec2 aPos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // layout(location=1): vec2 aUV
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

// ============================================================================
// uploadPBRMaps — загружает 4 текстуры из CPU-буферов на GPU
// ============================================================================
void Renderer::uploadPBRMaps(const PBRMaps& maps)
{
    // Удаляем старые текстуры если они существуют
    GLuint toDelete[] = { m_texDiffuse, m_texHeight, m_texNormal, m_texRoughness };
    glDeleteTextures(4, toDelete);
    m_texDiffuse = m_texHeight = m_texNormal = m_texRoughness = 0;

    const int w = maps.width;
    const int h = maps.height_px;

    // Диффуз: RGB8
    createTexture(m_texDiffuse,   maps.diffuse,   w, h, GL_RGB8, GL_RGB,  GL_UNSIGNED_BYTE);
    // Высота: R8 (grayscale)
    createTexture(m_texHeight,    maps.height,    w, h, GL_R8,   GL_RED,  GL_UNSIGNED_BYTE);
    // Нормали: RGB8
    createTexture(m_texNormal,    maps.normal,    w, h, GL_RGB8, GL_RGB,  GL_UNSIGNED_BYTE);
    // Шероховатость: R8 (grayscale)
    createTexture(m_texRoughness, maps.roughness, w, h, GL_R8,   GL_RED,  GL_UNSIGNED_BYTE);
}

// ============================================================================
// createTexture — вспомогательная функция создания текстуры
// ============================================================================
void Renderer::createTexture(GLuint& texID,
                              const std::vector<uint8_t>& data,
                              int w, int h,
                              GLenum internalFmt,
                              GLenum format,
                              GLenum type)
{
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);

    // Параметры фильтрации и тайлинга
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Для R8 текстур: указываем mapping каналов (иначе G,B = 0, A = 1)
    if (format == GL_RED) {
        GLint swizzle[] = { GL_RED, GL_RED, GL_RED, GL_ONE };
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFmt),
                 w, h, 0, format, type, data.data());
    glGenerateMipmap(GL_TEXTURE_2D); // MIP-карты для качественного масштабирования

    glBindTexture(GL_TEXTURE_2D, 0);
}

// ============================================================================
// drawQuad — основной вызов рендеринга
// ============================================================================
void Renderer::drawQuad(int mapMode)
{
    // Обновить viewport под текущий размер framebuffer
    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);

    glUseProgram(m_shader);
    glUniform1i(m_uMapMode, mapMode);

    // Привязываем 4 текстуры к texture units 0–3
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texDiffuse);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_texHeight);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_texNormal);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_texRoughness);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glUseProgram(0);
}

// ============================================================================
// clear — очистка буфера кадра
// ============================================================================
void Renderer::clear(float r, float g, float b) const
{
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// ============================================================================
// compileShader — компиляция одного шейдера с детальным выводом ошибок
// ============================================================================
GLuint Renderer::compileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        const char* typeName = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        throw std::runtime_error(
            std::format("Ошибка компиляции {} шейдера:\n{}", typeName, log));
    }

    return shader;
}

// ============================================================================
// linkProgram — линковка программы с проверкой ошибок
// ============================================================================
GLuint Renderer::linkProgram(GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint success = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        glDeleteProgram(prog);
        throw std::runtime_error(
            std::format("Ошибка линковки шейдерной программы:\n{}", log));
    }

    return prog;
}

// ---- Исходники PBR шейдеров (встроены как raw string literals) ----

static const char* PBR_VERT_SRC = R"GLSL(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNormal;

out vec2 vUV;
out vec3 vNormal;
out vec3 vWorldPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal   = mat3(uModel) * aNormal;
    vUV       = aUV;
    gl_Position = uProjection * uView * worldPos;
}
)GLSL";

static const char* PBR_FRAG_SRC = R"GLSL(
#version 330 core

in vec2 vUV;
in vec3 vNormal;
in vec3 vWorldPos;

out vec4 FragColor;

uniform sampler2D uDiffuse;
uniform sampler2D uHeight;
uniform sampler2D uNormal;
uniform sampler2D uRoughness;

uniform vec3  uViewPos;
uniform vec3  uLightPos;
uniform vec3  uLightColor;
uniform float uLightIntensity;
uniform vec3  uAmbientColor;

const float PI = 3.14159265359;

float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness)
         * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    vec3  albedo    = pow(texture(uDiffuse,   vUV).rgb, vec3(2.2));
    float roughness = clamp(texture(uRoughness, vUV).r, 0.04, 1.0);
    const float metallic = 0.0;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // TBN через экранные производные (без атрибутов касательных на CPU)
    vec3 N = normalize(vNormal);
    vec3 dp1  = dFdx(vWorldPos);
    vec3 dp2  = dFdy(vWorldPos);
    vec2 duv1 = dFdx(vUV);
    vec2 duv2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    vec3 normalSample = texture(uNormal, vUV).rgb * 2.0 - 1.0;
    N = normalize(TBN * normalSample);

    vec3  V    = normalize(uViewPos - vWorldPos);
    vec3  L    = normalize(uLightPos - vWorldPos);
    vec3  H    = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3  F = fresnelSchlick(VdotH, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    float dist        = length(uLightPos - vWorldPos);
    float attenuation = 1.0 / (dist * dist);
    vec3  radiance    = uLightColor * uLightIntensity * attenuation;

    vec3 Lo      = (kD * albedo / PI + specular) * radiance * NdotL;
    vec3 ambient = uAmbientColor * albedo;
    vec3 color   = ambient + Lo;

    color = color / (color + vec3(1.0));        // Reinhard tonemapping
    color = pow(color, vec3(1.0 / 2.2));         // gamma correction

    FragColor = vec4(color, 1.0);
}
)GLSL";

// ============================================================================
// Вспомогательные функции матричной математики (column-major, без glm)
// OpenGL convention: m[col*4 + row]
// ============================================================================

namespace {  // анонимное пространство имён — только для этого .cpp файла

// --- Скалярные операции над vec3 ---

static float v3Dot(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void v3Cross(const float* a, const float* b, float* out) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

static void v3Norm(float* v) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

// --- Единичная матрица 4×4 ---
static void mat4Identity(float* m) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// --- Перспективная матрица (column-major) ---
// fovY_rad — вертикальный угол обзора в радианах
static void mat4Perspective(float fovY_rad, float aspect, float zNear, float zFar, float* m)
{
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    float f = 1.0f / std::tan(fovY_rad * 0.5f);
    m[0]  = f / aspect;                                     // col 0, row 0
    m[5]  = f;                                              // col 1, row 1
    m[10] = (zFar + zNear) / (zNear - zFar);               // col 2, row 2
    m[11] = -1.0f;                                          // col 2, row 3 → w = -z
    m[14] = (2.0f * zFar * zNear) / (zNear - zFar);        // col 3, row 2
    // m[15] = 0 (homogeneous w)
}

// --- Матрица lookAt (column-major) ---
// Стандартный алгоритм: строим правый и верхний векторы камеры,
// переводим в базис камеры через транспонирование + offset.
static void mat4LookAt(const float* eye, const float* center,
                       const float* worldUp, float* m)
{
    // f = normalize(center - eye)
    float f[3] = { center[0]-eye[0], center[1]-eye[1], center[2]-eye[2] };
    v3Norm(f);

    // r = normalize(cross(f, worldUp))
    float r[3];
    v3Cross(f, worldUp, r);
    v3Norm(r);

    // u = cross(r, f)  (уже нормализован)
    float u[3];
    v3Cross(r, f, u);

    // Заполняем column-major: col 0 = r, col 1 = u, col 2 = -f, col 3 = translation
    m[0] = r[0];  m[1] = u[0];  m[2] = -f[0];  m[3] = 0.0f;
    m[4] = r[1];  m[5] = u[1];  m[6] = -f[1];  m[7] = 0.0f;
    m[8] = r[2];  m[9] = u[2]; m[10] = -f[2]; m[11] = 0.0f;
    m[12] = -v3Dot(r, eye);
    m[13] = -v3Dot(u, eye);
    m[14] =  v3Dot(f, eye);
    m[15] = 1.0f;
}

} // anonymous namespace

// ============================================================================
// loadPBRShader — компилирует PBR-шейдер и кэширует uniform locations
// ============================================================================
void Renderer::loadPBRShader()
{
    GLuint vert = compileShader(GL_VERTEX_SHADER,   PBR_VERT_SRC);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, PBR_FRAG_SRC);
    m_pbrShader = linkProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    // Кэшируем uniform locations (вызов glGetUniformLocation дёшев при старте,
    // но накладен в цикле рендеринга)
    m_pbr.model    = glGetUniformLocation(m_pbrShader, "uModel");
    m_pbr.view     = glGetUniformLocation(m_pbrShader, "uView");
    m_pbr.proj     = glGetUniformLocation(m_pbrShader, "uProjection");
    m_pbr.viewPos  = glGetUniformLocation(m_pbrShader, "uViewPos");
    m_pbr.lightPos = glGetUniformLocation(m_pbrShader, "uLightPos");
    m_pbr.lightCol = glGetUniformLocation(m_pbrShader, "uLightColor");
    m_pbr.lightInt = glGetUniformLocation(m_pbrShader, "uLightIntensity");
    m_pbr.ambient  = glGetUniformLocation(m_pbrShader, "uAmbientColor");

    // Привязываем сэмплеры к texture units (одноразово — sampler uniforms статичны)
    glUseProgram(m_pbrShader);
    glUniform1i(glGetUniformLocation(m_pbrShader, "uDiffuse"),   0);
    glUniform1i(glGetUniformLocation(m_pbrShader, "uHeight"),    1);
    glUniform1i(glGetUniformLocation(m_pbrShader, "uNormal"),    2);
    glUniform1i(glGetUniformLocation(m_pbrShader, "uRoughness"), 3);
    glUseProgram(0);

    std::cout << "[Renderer] PBR шейдер скомпилирован успешно.\n";
}

// ============================================================================
// createSphere — параметрическая сфера (32 секции × 24 стопки)
//
// Обоснование выбора: параметрическая сфера проще икосферы в реализации,
// даёт корректные UV-координаты для тайлинга текстур (u,v ∈ [0,1]),
// а плотность 32×24 = ~1500 треугольников достаточна для плавных бликов.
//
// Формат вершины: pos(3) + uv(2) + normal(3) = 8 float = 32 байта
// ============================================================================
void Renderer::createSphere(int sectors, int stacks)
{
    const float pi = std::numbers::pi_v<float>;

    std::vector<float>        verts;
    std::vector<unsigned int> indices;

    const float dSector = 2.0f * pi / static_cast<float>(sectors);
    const float dStack  =         pi / static_cast<float>(stacks);

    // Генерация вершин: от верхнего полюса (phi=π/2) к нижнему (phi=-π/2)
    for (int i = 0; i <= stacks; ++i) {
        float stackAngle = pi * 0.5f - static_cast<float>(i) * dStack;
        float xz = std::cos(stackAngle);   // радиус горизонтального кольца
        float y  = std::sin(stackAngle);   // высота

        for (int j = 0; j <= sectors; ++j) {
            float sectorAngle = static_cast<float>(j) * dSector;
            float x = xz * std::cos(sectorAngle);
            float z = xz * std::sin(sectorAngle);

            // pos
            verts.push_back(x);
            verts.push_back(y);
            verts.push_back(z);
            // uv  (для единичной сферы нормали == позиции)
            verts.push_back(static_cast<float>(j) / static_cast<float>(sectors));
            verts.push_back(static_cast<float>(i) / static_cast<float>(stacks));
            // normal (совпадает с pos для единичной сферы)
            verts.push_back(x);
            verts.push_back(y);
            verts.push_back(z);
        }
    }

    // Генерация индексов (два треугольника на квад, исключаем вырожденные у полюсов)
    for (int i = 0; i < stacks; ++i) {
        unsigned int k1 = static_cast<unsigned int>(i * (sectors + 1));
        unsigned int k2 = k1 + static_cast<unsigned int>(sectors + 1);

        for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
            if (i != 0) {
                // верхний треугольник квада
                indices.push_back(k1);
                indices.push_back(k2);
                indices.push_back(k1 + 1);
            }
            if (i != stacks - 1) {
                // нижний треугольник квада
                indices.push_back(k1 + 1);
                indices.push_back(k2);
                indices.push_back(k2 + 1);
            }
        }
    }

    m_sphereIdxCnt = static_cast<GLsizei>(indices.size());

    // Загрузка на GPU
    glGenVertexArrays(1, &m_sphereVao);
    glGenBuffers(1,     &m_sphereVbo);
    glGenBuffers(1,     &m_sphereEbo);

    glBindVertexArray(m_sphereVao);

    glBindBuffer(GL_ARRAY_BUFFER, m_sphereVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_sphereEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
                 indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = 8 * static_cast<GLsizei>(sizeof(float));

    // layout(location=0): vec3 aPos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // layout(location=1): vec2 aUV
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // layout(location=2): vec3 aNormal
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    std::cout << std::format("[Renderer] Сфера создана: {} вершин, {} индексов.\n",
        verts.size() / 8, m_sphereIdxCnt);
}

// ============================================================================
// drawPBRScene — рендеринг сферы с Cook-Torrance BRDF
// camTheta — азимут [0..360] градусы
// camPhi   — полярный угол [5..175] градусы (5/175 отступ от полюса)
// camDist  — радиус орбиты камеры
// ============================================================================
void Renderer::drawPBRScene(float camDist, float camTheta, float camPhi,
                             const LightParams& light)
{
    if (!m_pbrShader || !m_sphereVao) return;  // защита от неинициализированного состояния

    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);

    // Depth test нужен только в 3D-режиме; очищаем depth здесь
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    glUseProgram(m_pbrShader);

    // ---- Матрица модели: единичная (сфера в начале координат) ----
    float model[16]; mat4Identity(model);

    // ---- Позиция камеры (сферические координаты → декартовы) ----
    // theta — азимут (вращение вокруг Y), phi — полярный угол (наклон)
    const float pi = std::numbers::pi_v<float>;
    float thetaR = camTheta * pi / 180.0f;
    float phiR   = camPhi   * pi / 180.0f;

    float eye[3] = {
        camDist * std::sin(phiR) * std::cos(thetaR),
        camDist * std::cos(phiR),
        camDist * std::sin(phiR) * std::sin(thetaR)
    };
    const float center[3] = { 0.0f, 0.0f, 0.0f };
    const float worldUp[3]= { 0.0f, 1.0f, 0.0f };

    float view[16]; mat4LookAt(eye, center, worldUp, view);

    float aspect = (fbH > 0) ? static_cast<float>(fbW) / static_cast<float>(fbH) : 1.0f;
    float proj[16]; mat4Perspective(45.0f * pi / 180.0f, aspect, 0.1f, 100.0f, proj);

    // ---- Передача матриц ----
    // GL_FALSE: матрицы уже в column-major (как ожидает OpenGL)
    glUniformMatrix4fv(m_pbr.model, 1, GL_FALSE, model);
    glUniformMatrix4fv(m_pbr.view,  1, GL_FALSE, view);
    glUniformMatrix4fv(m_pbr.proj,  1, GL_FALSE, proj);

    // ---- Камера и свет ----
    glUniform3fv(m_pbr.viewPos,  1, eye);
    glUniform3fv(m_pbr.lightPos, 1, light.position);
    glUniform3fv(m_pbr.lightCol, 1, light.color);
    glUniform1f (m_pbr.lightInt,    light.intensity);
    glUniform3fv(m_pbr.ambient,  1, light.ambient);

    // ---- Текстуры (те же юниты 0–3, что и в drawQuad) ----
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texDiffuse);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_texHeight);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_texNormal);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_texRoughness);

    // ---- Draw call ----
    glBindVertexArray(m_sphereVao);
    glDrawElements(GL_TRIANGLES, m_sphereIdxCnt, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    // Восстанавливаем состояние (для корректной работы 2D drawQuad)
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
}
