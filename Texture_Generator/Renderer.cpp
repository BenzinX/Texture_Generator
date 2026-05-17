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
