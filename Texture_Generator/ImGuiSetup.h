#pragma once
// ============================================================================
// ImGuiSetup.h — Интерактивная панель редактора текстур (Dear ImGui)
// ============================================================================

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include "PBRGenerator.h"
#include <string>
#include <functional>

// ============================================================================
// TextureEditorPanel — управляет жизненным циклом ImGui и рисует UI-панель
// ============================================================================
class TextureEditorPanel {
public:
    // Инициализация ImGui (вызвать один раз после создания окна)
    explicit TextureEditorPanel(GLFWwindow* window);
    ~TextureEditorPanel();

    // Запрет копирования
    TextureEditorPanel(const TextureEditorPanel&)            = delete;
    TextureEditorPanel& operator=(const TextureEditorPanel&) = delete;

    // --- Фреймовые вызовы ---

    // Начать новый ImGui-фрейм (вызвать до любого ImGui::*)
    void beginFrame();

    // Нарисовать панель управления.
    // Возвращает true если параметры изменились и нужна перегенерация.
    bool drawPanel(GeneratorParams& params, int& viewMode);

    // Завершить ImGui-фрейм и отправить draw calls на GPU
    void endFrame();

    // --- Состояние ---

    // Экспорт текущих карт (задаётся снаружи через callback)
    std::function<void()> onExportPPM;

    // Показать окно статистики генерации (мс)
    void setGenerationTime(double ms) { m_lastGenMs = ms; }

private:
    double      m_lastGenMs = 0.0;

    // Внутренний стиль ImGui (устанавливается в конструкторе)
    void applyCustomStyle();
};
