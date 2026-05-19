#pragma once
// ============================================================================
// Renderer.h — Современный рендерер OpenGL 3.3 Core
// Использует GLFW + GLAD, VAO/VBO, шейдерные программы, 4 текстуры PBR.
//
// Поддерживает 5 режимов:
//   0 = Diffuse       — диффузная карта на fullscreen quad
//   1 = Height        — карта высот (grayscale) на fullscreen quad
//   2 = Normal        — карта нормалей на fullscreen quad
//   3 = Roughness     — карта шероховатости на fullscreen quad
//   4 = 3D View       — вращаемая 3D-плоскость с освещением Blinn-Phong
// ============================================================================

#pragma once
#include <glad/glad.h>          // СТРОГО ПЕРВЫМ
#define GLFW_INCLUDE_NONE       // Запрет системных GL-заголовков в GLFW
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <cstdint>
#include "PBRGenerator.h"

// ============================================================================
// Параметры 3D-режима освещения — передаются из ImGui-панели
// ============================================================================
struct LightParams {
    float azimuth = 45.0f;   // угол по горизонтали (градусы)
    float elevation = 35.0f;   // угол по вертикали (градусы, 0=горизонт, 90=зенит)
    float intensity = 1.2f;    // яркость источника
    float ambient = 0.15f;   // фоновое освещение
    float specPower = 32.0f;   // показатель блеска Blinn-Phong
    float specIntens = 0.6f;    // интенсивность спекулярного блика
    float rotAngleX = 0.0f;    // вращение плоскости по X (радианы)
    float rotAngleY = 0.0f;    // вращение плоскости по Y (радианы)
    bool  autoRotate = false;   // автоматическое вращение плоскости
};

// ============================================================================
// Renderer — управляет окном GLFW, шейдерами и GPU-ресурсами
// ============================================================================
class Renderer {
public:
    // Создаёт окно и инициализирует OpenGL 3.3 Core
    Renderer(int width, int height, const std::string& title);
    ~Renderer();

    // Запрет копирования (RAII с GPU-ресурсами)
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // --- Управление текстурами ---

    // Загрузить все 4 карты PBR на GPU (пересоздаёт текстуры при каждом вызове)
    void uploadPBRMaps(const PBRMaps& maps);

    // --- Рендеринг ---

    // Нарисовать сцену с указанным режимом
    // mapMode: 0=Diffuse, 1=Height, 2=Normal, 3=Roughness, 4=3DView
    void drawQuad(int mapMode, const LightParams& light = {});

    // --- Доступ к окну ---
    [[nodiscard]] GLFWwindow* window() const noexcept { return m_window; }
    [[nodiscard]] bool shouldClose()   const noexcept {
        return glfwWindowShouldClose(m_window) != 0;
    }
    void pollEvents()  const { glfwPollEvents(); }
    void swapBuffers() const { glfwSwapBuffers(m_window); }

    // Очистить буфер кадра
    void clear(float r = 0.1f, float g = 0.1f, float b = 0.1f) const;

    // Вернуть размер framebuffer (для glViewport)
    void getFramebufferSize(int& w, int& h) const {
        glfwGetFramebufferSize(m_window, &w, &h);
    }

private:
    // --- Вспомогательные функции ---
    GLuint compileShader(GLenum type, const char* src);
    GLuint linkProgram(GLuint vert, GLuint frag);
    void   createQuad();
    void   createPlane3D();

    // Создать/пересоздать текстуру OpenGL из CPU-буфера
    void createTexture(GLuint& texID,
        const std::vector<uint8_t>& data,
        int w, int h,
        GLenum internalFmt,  // GL_RGB8 или GL_R8
        GLenum format,       // GL_RGB  или GL_RED
        GLenum type);        // GL_UNSIGNED_BYTE

    // --- Поля ---
    GLFWwindow* m_window = nullptr;

    // Fullscreen quad (режимы 0–3)
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    // 3D-плоскость (режим 4): тесселированный меш 32×32 квадрата
    GLuint m_planeVao = 0;
    GLuint m_planeVbo = 0;
    GLuint m_planeEbo = 0;
    int    m_planeIndexCount = 0;

    // Шейдерная программа для 2D-квада
    GLuint m_shader = 0;
    // Шейдерная программа для 3D-плоскости (освещение + нормальная карта)
    GLuint m_shader3D = 0;

    // 4 текстуры PBR
    GLuint m_texDiffuse = 0;
    GLuint m_texHeight = 0;
    GLuint m_texNormal = 0;
    GLuint m_texRoughness = 0;

    // Uniform locations для 2D-шейдера
    GLint m_uMapMode = -1;
    GLint m_uDiffuse = -1;
    GLint m_uHeight = -1;
    GLint m_uNormal = -1;
    GLint m_uRoughness = -1;
    GLint m_uBrightness = -1;

    // Uniform locations для 3D-шейдера
    GLint m_3d_uMVP = -1;
    GLint m_3d_uModel = -1;
    GLint m_3d_uLightDir = -1;
    GLint m_3d_uLightInt = -1;
    GLint m_3d_uAmbient = -1;
    GLint m_3d_uSpecPow = -1;
    GLint m_3d_uSpecInt = -1;
    GLint m_3d_uDiffuse = -1;
    GLint m_3d_uNormal = -1;
    GLint m_3d_uRoughness = -1;
};