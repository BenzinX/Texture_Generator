// ============================================================================
// Renderer.cpp — Реализация рендерера OpenGL 3.3 Core
// GLFW + GLAD, VAO/VBO, встроенные GLSL-шейдеры, 4 PBR-текстуры.
//
// Матрицы (View, Projection, Model, MVP) вычисляются вручную без glm.
// 3D-плоскость — тесселированный меш 32×32, освещение Blinn-Phong + NormalMap.
// ============================================================================

#include "Renderer.h"

#include <stdexcept>
#include <format>
#include <iostream>
#include <array>
#include <vector>
#include <cmath>
#include <numbers>

// ============================================================================
// Вспомогательные матричные операции (4×4, column-major как требует OpenGL)
// Все матрицы хранятся в массиве float[16], column-major порядок.
// ============================================================================

// Умножение двух 4×4 матриц (column-major): C = A * B
static void mat4_mul(const float A[16], const float B[16], float C[16]) noexcept
{
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += A[k * 4 + row] * B[col * 4 + k];
            }
            C[col * 4 + row] = sum;
        }
    }
}

// Единичная матрица
static void mat4_identity(float M[16]) noexcept
{
    for (int i = 0; i < 16; ++i) M[i] = 0.0f;
    M[0] = M[5] = M[10] = M[15] = 1.0f;
}

// Матрица вращения вокруг оси X
static void mat4_rotX(float M[16], float angle) noexcept
{
    mat4_identity(M);
    float c = std::cos(angle), s = std::sin(angle);
    M[5] = c;  M[9] = -s;
    M[6] = s;  M[10] = c;
}

// Матрица вращения вокруг оси Y
static void mat4_rotY(float M[16], float angle) noexcept
{
    mat4_identity(M);
    float c = std::cos(angle), s = std::sin(angle);
    M[0] = c;  M[8] = s;
    M[2] = -s;  M[10] = c;
}

// Матрица переноса
static void mat4_translate(float M[16], float tx, float ty, float tz) noexcept
{
    mat4_identity(M);
    M[12] = tx;  M[13] = ty;  M[14] = tz;
}

// Матрица масштабирования
static void mat4_scale(float M[16], float sx, float sy, float sz) noexcept
{
    mat4_identity(M);
    M[0] = sx;  M[5] = sy;  M[10] = sz;
}

// Матрица перспективной проекции (аналог glm::perspective)
// fovY — вертикальный угол обзора в радианах
static void mat4_perspective(float M[16],
    float fovY, float aspect,
    float zNear, float zFar) noexcept
{
    for (int i = 0; i < 16; ++i) M[i] = 0.0f;
    float f = 1.0f / std::tan(fovY * 0.5f);
    M[0] = f / aspect;
    M[5] = f;
    M[10] = -(zFar + zNear) / (zFar - zNear);
    M[11] = -1.0f;
    M[14] = -(2.0f * zFar * zNear) / (zFar - zNear);
}

// Матрица вида: простая камера смотрит на origin снизу-сбоку
// eye=(0, 1.4, 2.2), target=(0,0,0), up=(0,1,0)
static void mat4_lookAt(float M[16],
    float ex, float ey, float ez,
    float cx, float cy, float cz,
    float ux, float uy, float uz) noexcept
{
    // Вектор «вперёд» (нормализованный)
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= flen; fy /= flen; fz /= flen;

    // Вектор «вправо» = forward × up
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rlen; ry /= rlen; rz /= rlen;

    // Скорректированный «вверх»
    float uux = ry * fz - rz * fy;
    float uuy = rz * fx - rx * fz;
    float uuz = rx * fy - ry * fx;

    // column-major заполнение
    M[0] = rx;  M[1] = uux;  M[2] = -fx;  M[3] = 0.0f;
    M[4] = ry;  M[5] = uuy;  M[6] = -fy;  M[7] = 0.0f;
    M[8] = rz;  M[9] = uuz;  M[10] = -fz;  M[11] = 0.0f;
    M[12] = -(rx * ex + ry * ey + rz * ez);
    M[13] = -(uux * ex + uuy * ey + uuz * ez);
    M[14] = (fx * ex + fy * ey + fz * ez);
    M[15] = 1.0f;
}

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

uniform sampler2D uDiffuse;
uniform sampler2D uHeight;
uniform sampler2D uNormal;
uniform sampler2D uRoughness;

// 0 = Diffuse, 1 = Height, 2 = Normal, 3 = Roughness
uniform int   uMapMode;
uniform float uBrightness;

void main() {
    vec2 uv = vUV;
    vec4 color;

    if (uMapMode == 0) {
        color = vec4(texture(uDiffuse, uv).rgb, 1.0);
    }
    else if (uMapMode == 1) {
        float h = texture(uHeight, uv).r;
        color = vec4(h, h, h, 1.0);
    }
    else if (uMapMode == 2) {
        color = vec4(texture(uNormal, uv).rgb, 1.0);
    }
    else {
        float r = texture(uRoughness, uv).r;
        color = vec4(r, r, r, 1.0);
    }

    FragColor = color * uBrightness;
}
)GLSL";

// ============================================================================
// Вершинный шейдер для 3D-плоскости
// Принимает: позиция (vec3), нормаль (vec3), UV (vec2)
// Передаёт во фрагментный: UV, позицию в world-space, нормаль в world-space
// ============================================================================
static const char* VERTEX_3D_SRC = R"GLSL(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// MVP-матрица (Model * View * Projection) и отдельно Model для нормалей
uniform mat4 uMVP;
uniform mat4 uModel;

out vec2  vUV;
out vec3  vWorldPos;
out vec3  vWorldNormal;

void main() {
    vUV          = aUV;
    // Позиция в world-space (без View и Projection)
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos     = worldPos.xyz;
    // Нормаль в world-space: умножаем на матрицу 3×3 модели
    // (для ортогональных матриц без неравномерного масштаба это корректно)
    vWorldNormal  = mat3(uModel) * aNormal;
    gl_Position   = uMVP * vec4(aPos, 1.0);
}
)GLSL";

// ============================================================================
// Фрагментный шейдер для 3D-плоскости: Blinn-Phong + Normal Map
//
// Алгоритм:
//   1. Сэмплируем карту нормалей → распаковываем из [0,1] в [-1,1]
//   2. Строим TBN матрицу: T=dP/dU, B=dP/dV, N=геометрическая нормаль
//   3. Трансформируем нормаль текстуры в world-space через TBN
//   4. Blinn-Phong: диффузный + спекулярный компонент
//   5. Шероховатость из roughness-карты модулирует specPower
// ============================================================================
static const char* FRAGMENT_3D_SRC = R"GLSL(
#version 330 core

in vec2  vUV;
in vec3  vWorldPos;
in vec3  vWorldNormal;

out vec4 FragColor;

uniform sampler2D uDiffuse;
uniform sampler2D uNormal;
uniform sampler2D uRoughness;

uniform vec3  uLightDir;   // нормализованный вектор на источник света
uniform float uLightInt;   // интенсивность источника
uniform float uAmbient;    // фоновое освещение
uniform float uSpecPow;    // показатель блеска (32–256)
uniform float uSpecInt;    // интенсивность спекуляра

void main() {
    // --- Получаем карты ---
    vec3 albedo   = texture(uDiffuse,   vUV).rgb;
    vec3 normSamp = texture(uNormal,    vUV).rgb;
    float rough   = texture(uRoughness, vUV).r;

    // --- Распаковка нормали: [0,1] → [-1,1] ---
    vec3 tangentNormal = normSamp * 2.0 - 1.0;

    // --- TBN матрица из производных экранных координат ---
    // Использование dFdx/dFdy (стандарт GLSL 3.30) позволяет вычислить TBN
    // без хранения тангентов в меше.
    vec3 dPdx  = dFdx(vWorldPos);
    vec3 dPdy  = dFdy(vWorldPos);
    vec2 dUVdx = dFdx(vUV);
    vec2 dUVdy = dFdy(vUV);

    // Детерминант Якобиана UV-преобразования (для масштабирования касательных)
    float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
    // Защита от вырожденного случая (det≈0 на краях/стыках)
    float invDet = (abs(det) > 1e-6) ? (1.0 / det) : 0.0;

    vec3 T = normalize(( dUVdy.y * dPdx - dUVdx.y * dPdy) * invDet);
    vec3 B = normalize((-dUVdy.x * dPdx + dUVdx.x * dPdy) * invDet);
    vec3 N = normalize(vWorldNormal);

    // Перестройка TBN с ортонормализацией (Gram-Schmidt по нормали меша)
    T = normalize(T - dot(T, N) * N);
    B = cross(N, T);

    // Трансформируем нормаль текстуры в world-space через TBN
    vec3 worldNormal = normalize(T * tangentNormal.x
                                + B * tangentNormal.y
                                + N * tangentNormal.z);

    // --- Blinn-Phong освещение ---
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(vec3(0.0, 2.0, 3.0) - vWorldPos); // направление к камере

    // Диффузный компонент (Ламберт)
    float NdotL = max(dot(worldNormal, L), 0.0);
    vec3  diffuse = albedo * NdotL * uLightInt;

    // Спекулярный компонент (Blinn-Phong): half-vector
    vec3  H       = normalize(L + V);
    float NdotH   = max(dot(worldNormal, H), 0.0);
    // Шероховатость инвертирует и масштабирует показатель блеска:
    // rough≈0 (гладко) → высокий specPow; rough≈1 (грубо) → specPow≈4
    float adaptPow  = max(uSpecPow * (1.0 - rough * 0.92) + 2.0, 2.0);
    float specValue = pow(NdotH, adaptPow);
    vec3  specular  = vec3(1.0) * specValue * uSpecInt * uLightInt * (1.0 - rough * 0.85);

    // Итог
    vec3 ambient = albedo * uAmbient;
    vec3 color   = ambient + diffuse + specular;

    // Тонмаппинг Reinhard (предотвращает пересветку при ярких источниках)
    color = color / (color + vec3(1.0));

    // Гамма-коррекция: линейное → sRGB
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
)GLSL";

// ============================================================================
// Конструктор — инициализация GLFW, GLAD, шейдеров, quad, plane
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

    // --- 3. Компиляция 2D-шейдерной программы ---
    {
        GLuint vert = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
        GLuint frag = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
        m_shader = linkProgram(vert, frag);
        glDeleteShader(vert);
        glDeleteShader(frag);

        // Кэшируем uniform locations
        m_uMapMode = glGetUniformLocation(m_shader, "uMapMode");
        m_uDiffuse = glGetUniformLocation(m_shader, "uDiffuse");
        m_uHeight = glGetUniformLocation(m_shader, "uHeight");
        m_uNormal = glGetUniformLocation(m_shader, "uNormal");
        m_uRoughness = glGetUniformLocation(m_shader, "uRoughness");
        m_uBrightness = glGetUniformLocation(m_shader, "uBrightness");

        // Привязываем сэмплеры к texture units (один раз при инициализации)
        glUseProgram(m_shader);
        glUniform1i(m_uDiffuse, 0);
        glUniform1i(m_uHeight, 1);
        glUniform1i(m_uNormal, 2);
        glUniform1i(m_uRoughness, 3);
        glUniform1f(m_uBrightness, 1.0f);
        glUseProgram(0);
    }

    // --- 4. Компиляция 3D-шейдерной программы ---
    {
        GLuint vert = compileShader(GL_VERTEX_SHADER, VERTEX_3D_SRC);
        GLuint frag = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_3D_SRC);
        m_shader3D = linkProgram(vert, frag);
        glDeleteShader(vert);
        glDeleteShader(frag);

        // Кэшируем uniform locations для 3D-шейдера
        m_3d_uMVP = glGetUniformLocation(m_shader3D, "uMVP");
        m_3d_uModel = glGetUniformLocation(m_shader3D, "uModel");
        m_3d_uLightDir = glGetUniformLocation(m_shader3D, "uLightDir");
        m_3d_uLightInt = glGetUniformLocation(m_shader3D, "uLightInt");
        m_3d_uAmbient = glGetUniformLocation(m_shader3D, "uAmbient");
        m_3d_uSpecPow = glGetUniformLocation(m_shader3D, "uSpecPow");
        m_3d_uSpecInt = glGetUniformLocation(m_shader3D, "uSpecInt");
        m_3d_uDiffuse = glGetUniformLocation(m_shader3D, "uDiffuse");
        m_3d_uNormal = glGetUniformLocation(m_shader3D, "uNormal");
        m_3d_uRoughness = glGetUniformLocation(m_shader3D, "uRoughness");

        // Привязываем сэмплеры для 3D-шейдера (units 0, 2, 3)
        glUseProgram(m_shader3D);
        glUniform1i(m_3d_uDiffuse, 0);
        glUniform1i(m_3d_uNormal, 2);
        glUniform1i(m_3d_uRoughness, 3);
        glUseProgram(0);
    }

    // --- 5. Создание геометрии ---
    createQuad();
    createPlane3D();

    // Включаем тест глубины для 3D-режима
    glEnable(GL_DEPTH_TEST);
}

// ============================================================================
// Деструктор — освобождение всех GPU-ресурсов
// ============================================================================
Renderer::~Renderer()
{
    if (m_texDiffuse)   glDeleteTextures(1, &m_texDiffuse);
    if (m_texHeight)    glDeleteTextures(1, &m_texHeight);
    if (m_texNormal)    glDeleteTextures(1, &m_texNormal);
    if (m_texRoughness) glDeleteTextures(1, &m_texRoughness);

    if (m_vao)      glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)      glDeleteBuffers(1, &m_vbo);
    if (m_planeVao) glDeleteVertexArrays(1, &m_planeVao);
    if (m_planeVbo) glDeleteBuffers(1, &m_planeVbo);
    if (m_planeEbo) glDeleteBuffers(1, &m_planeEbo);
    if (m_shader)   glDeleteProgram(m_shader);
    if (m_shader3D) glDeleteProgram(m_shader3D);

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
    //  UV:  (0,0)   → (1,1)
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
// createPlane3D — тесселированный меш 32×32 квадрата для 3D-режима
//
// Формат вершины: [posX, posY, posZ, normX, normY, normZ, uvX, uvY]
// Плоскость располагается горизонтально: XZ-плоскость, Y=0
// Размер: от (-1,0,-1) до (1,0,1)
// ============================================================================
void Renderer::createPlane3D()
{
    constexpr int GRID = 32; // разбиение на GRID×GRID квадратов

    std::vector<float>    verts;
    std::vector<uint32_t> indices;

    verts.reserve(static_cast<size_t>((GRID + 1) * (GRID + 1) * 8));
    indices.reserve(static_cast<size_t>(GRID * GRID * 6));

    // Генерируем вершины
    for (int iy = 0; iy <= GRID; ++iy) {
        for (int ix = 0; ix <= GRID; ++ix) {
            float u = static_cast<float>(ix) / GRID;
            float v = static_cast<float>(iy) / GRID;

            float px = u * 2.0f - 1.0f; // [-1, 1]
            float py = 0.0f;
            float pz = v * 2.0f - 1.0f; // [-1, 1]

            // Нормаль плоскости: Y-вверх
            float nx = 0.0f, ny = 1.0f, nz = 0.0f;

            // UV: [0,1]
            float tu = u;
            float tv = v;

            verts.insert(verts.end(), { px, py, pz, nx, ny, nz, tu, tv });
        }
    }

    // Генерируем индексы (CCW winding при взгляде сверху)
    for (int iy = 0; iy < GRID; ++iy) {
        for (int ix = 0; ix < GRID; ++ix) {
            uint32_t i00 = static_cast<uint32_t>(iy * (GRID + 1) + ix);
            uint32_t i10 = static_cast<uint32_t>(iy * (GRID + 1) + ix + 1);
            uint32_t i01 = static_cast<uint32_t>((iy + 1) * (GRID + 1) + ix);
            uint32_t i11 = static_cast<uint32_t>((iy + 1) * (GRID + 1) + ix + 1);

            // Два треугольника на квадрат
            indices.insert(indices.end(), { i00, i10, i11 });
            indices.insert(indices.end(), { i00, i11, i01 });
        }
    }

    m_planeIndexCount = static_cast<int>(indices.size());

    glGenVertexArrays(1, &m_planeVao);
    glGenBuffers(1, &m_planeVbo);
    glGenBuffers(1, &m_planeEbo);

    glBindVertexArray(m_planeVao);

    glBindBuffer(GL_ARRAY_BUFFER, m_planeVbo);
    glBufferData(GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
        verts.data(),
        GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_planeEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
        indices.data(),
        GL_STATIC_DRAW);

    constexpr GLsizei stride = 8 * sizeof(float);

    // layout(location=0): vec3 aPos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // layout(location=1): vec3 aNormal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // layout(location=2): vec2 aUV
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
        reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

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
    createTexture(m_texDiffuse, maps.diffuse, w, h, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
    // Высота: R8 (grayscale)
    createTexture(m_texHeight, maps.height, w, h, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
    // Нормали: RGB8
    createTexture(m_texNormal, maps.normal, w, h, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
    // Шероховатость: R8 (grayscale)
    createTexture(m_texRoughness, maps.roughness, w, h, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
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
// mapMode: 0–3 = fullscreen quad; 4 = 3D-плоскость с освещением
// ============================================================================
void Renderer::drawQuad(int mapMode, const LightParams& light)
{
    // Обновить viewport под текущий размер framebuffer
    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);

    if (mapMode != 4) {
        // --- Режимы 0–3: обычный fullscreen quad ---
        glDisable(GL_DEPTH_TEST);

        glUseProgram(m_shader);
        glUniform1i(m_uMapMode, mapMode);

        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texDiffuse);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_texHeight);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_texNormal);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_texRoughness);

        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);

        glEnable(GL_DEPTH_TEST);
    }
    else {
        // --- Режим 4: 3D-плоскость с освещением ---
        glEnable(GL_DEPTH_TEST);
        glClear(GL_DEPTH_BUFFER_BIT); // очищаем буфер глубины

        // --- Матрица модели: вращение + небольшой наклон ---
        float rotX[16], rotY[16], rotTmp[16], modelMat[16];
        mat4_rotX(rotX, light.rotAngleX);
        mat4_rotY(rotY, light.rotAngleY);
        mat4_mul(rotY, rotX, rotTmp);

        // Поднимаем плоскость чуть ниже центра для лучшего вида
        float transMat[16];
        mat4_translate(transMat, 0.0f, -0.1f, 0.0f);
        mat4_mul(transMat, rotTmp, modelMat);

        // --- Матрица вида: камера смотрит на плоскость сверху-сбоку ---
        float viewMat[16];
        mat4_lookAt(viewMat,
            0.0f, 2.0f, 3.0f,   // позиция камеры
            0.0f, 0.0f, 0.0f,   // цель
            0.0f, 1.0f, 0.0f);  // вверх

        // --- Матрица проекции ---
        float aspect = (fbH > 0) ? static_cast<float>(fbW) / static_cast<float>(fbH) : 1.0f;
        float projMat[16];
        mat4_perspective(projMat,
            static_cast<float>(std::numbers::pi) / 4.0f, // 45 градусов
            aspect,
            0.1f, 100.0f);

        // --- MVP = Proj * View * Model ---
        float pvMat[16], mvpMat[16];
        mat4_mul(projMat, viewMat, pvMat);
        mat4_mul(pvMat, modelMat, mvpMat);

        // --- Вычисляем вектор на источник света из углов азимута и элевации ---
        float azRad = light.azimuth * static_cast<float>(std::numbers::pi) / 180.0f;
        float elRad = light.elevation * static_cast<float>(std::numbers::pi) / 180.0f;
        float lx = std::cos(elRad) * std::sin(azRad);
        float ly = std::sin(elRad);
        float lz = std::cos(elRad) * std::cos(azRad);
        // Нормализуем (уже единичный вектор из тригонометрии, но на случай погрешностей)
        float llen = std::sqrt(lx * lx + ly * ly + lz * lz);
        if (llen > 0.001f) { lx /= llen; ly /= llen; lz /= llen; }

        // --- Привязываем шейдер и передаём uniforms ---
        glUseProgram(m_shader3D);
        glUniformMatrix4fv(m_3d_uMVP, 1, GL_FALSE, mvpMat);
        glUniformMatrix4fv(m_3d_uModel, 1, GL_FALSE, modelMat);
        glUniform3f(m_3d_uLightDir, lx, ly, lz);
        glUniform1f(m_3d_uLightInt, light.intensity);
        glUniform1f(m_3d_uAmbient, light.ambient);
        glUniform1f(m_3d_uSpecPow, light.specPower);
        glUniform1f(m_3d_uSpecInt, light.specIntens);

        // Привязываем текстуры (diffuse=0, normal=2, roughness=3)
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texDiffuse);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_texNormal);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_texRoughness);

        glBindVertexArray(m_planeVao);
        glDrawElements(GL_TRIANGLES, m_planeIndexCount, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        glUseProgram(0);
    }
}

// ============================================================================
// clear — очистка буфера кадра (+ depth если 3D)
// ============================================================================
void Renderer::clear(float r, float g, float b) const
{
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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