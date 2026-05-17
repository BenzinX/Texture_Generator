#pragma once
// ============================================================================
// Renderer.h — Современный рендерер OpenGL 3.3 Core
// Использует GLFW + GLAD, VAO/VBO, шейдерные программы, 4 текстуры PBR
// ============================================================================

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <cstdint>
#include "PBRGenerator.h"

// ============================================================================
// Renderer — управляет окном GLFW, шейдерами и GPU-ресурсами
// ============================================================================
class Renderer {
public:
    // Создаёт окно и инициализирует OpenGL 3.3 Core
    Renderer(int width, int height, const std::string& title);
    ~Renderer();

    // Запрет копирования (RAII с GPU-ресурсами)
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // --- Управление текстурами ---

    // Загрузить все 4 карты PBR на GPU (пересоздаёт текстуры при каждом вызове)
    void uploadPBRMaps(const PBRMaps& maps);

    // --- Рендеринг ---

    // Нарисовать fullscreen quad с активной текстурой
    // mapMode: 0=Diffuse, 1=Height, 2=Normal, 3=Roughness
    void drawQuad(int mapMode);

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

    // Создать/пересоздать текстуру OpenGL из CPU-буфера
    void createTexture(GLuint& texID,
                       const std::vector<uint8_t>& data,
                       int w, int h,
                       GLenum internalFmt,  // GL_RGB8 или GL_R8
                       GLenum format,       // GL_RGB  или GL_RED
                       GLenum type);        // GL_UNSIGNED_BYTE

    // --- Поля ---
    GLFWwindow* m_window = nullptr;

    // Fullscreen quad
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    // Шейдерная программа
    GLuint m_shader = 0;

    // 4 текстуры PBR
    GLuint m_texDiffuse   = 0;
    GLuint m_texHeight    = 0;
    GLuint m_texNormal    = 0;
    GLuint m_texRoughness = 0;

    // Uniform locations (кэшируем при создании шейдера)
    GLint m_uMapMode     = -1;
    GLint m_uDiffuse     = -1;
    GLint m_uHeight      = -1;
    GLint m_uNormal      = -1;
    GLint m_uRoughness   = -1;
    GLint m_uBrightness  = -1;
};
