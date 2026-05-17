// ============================================================================
// main.cpp — Точка входа. Главный цикл рендеринга.
//
// Архитектура:
//   Renderer      — GLFW + GLAD, VAO/VBO, шейдеры, 4 GPU-текстуры
//   PBRGenerator  — CPU-генерация 4 карт (Diffuse/Height/Normal/Roughness)
//   TextureEditorPanel — ImGui-панель с live-параметрами
//
// Зависимости (vcpkg x64-windows):
//   glfw3, glad, imgui[glfw-binding,opengl3-binding]
//
// Компиляция: Visual Studio 2022+, C++20, x64
// ============================================================================

#include <glad/glad.h>          // Должен быть ДО любых других GL заголовков
#include <GLFW/glfw3.h>

#include "Renderer.h"
#include "PBRGenerator.h"
#include "ImGuiSetup.h"

#include <iostream>
#include <chrono>
#include <format>
#include <string>

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Имена материалов для имён файлов PPM
static constexpr const char* MATERIAL_NAMES[] = {
    "stone", "grass", "lava", "wood"
};

// Горячие клавиши — обработка нажатий GLFW без ImGui
static void handleHotkeys(GLFWwindow* window,
                           GeneratorParams& params,
                           int& viewMode,
                           bool& needsRegen,
                           bool& wantsExport)
{
    // Игнорируем горячие клавиши, если ImGui захватил клавиатуру
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // 1–4: выбор материала
    static bool keyWasDown[10] = {};
    auto checkKey = [&](int key, int idx) -> bool {
        bool down = glfwGetKey(window, key) == GLFW_PRESS;
        bool pressed = down && !keyWasDown[idx];
        keyWasDown[idx] = down;
        return pressed;
    };

    if (checkKey(GLFW_KEY_1, 0)) { params.materialType = 0; needsRegen = true; }
    if (checkKey(GLFW_KEY_2, 1)) { params.materialType = 1; needsRegen = true; }
    if (checkKey(GLFW_KEY_3, 2)) { params.materialType = 2; needsRegen = true; }
    if (checkKey(GLFW_KEY_4, 3)) { params.materialType = 3; needsRegen = true; }

    // D/H/N/R: режим просмотра
    if (checkKey(GLFW_KEY_D, 4)) viewMode = 0;
    if (checkKey(GLFW_KEY_H, 5)) viewMode = 1;
    if (checkKey(GLFW_KEY_N, 6)) viewMode = 2;
    if (checkKey(GLFW_KEY_R, 7)) viewMode = 3;

    // Space: принудительная перегенерация
    if (checkKey(GLFW_KEY_SPACE, 8)) needsRegen = true;

    // E: экспорт PPM
    if (checkKey(GLFW_KEY_E, 9)) wantsExport = true;
}

// ============================================================================
// main
// ============================================================================
int main()
{
    try {
        // ---- Инициализация рендерера ----
        Renderer renderer(1024, 768, "PBR Texture Generator — GLFW/GLAD/ImGui");

        // ---- Инициализация ImGui-панели ----
        TextureEditorPanel panel(renderer.window());

        // ---- Начальные параметры ----
        GeneratorParams params;
        params.materialType  = 0;     // Stone
        params.seed          = 1337;
        params.octaves       = 6;
        params.persistence   = 0.5;
        params.lacunarity    = 2.0;
        params.baseScale     = 64.0;
        params.normalStrength = 6.0f;

        int  viewMode   = 0;      // 0=Diffuse
        bool needsRegen = true;   // Генерируем сразу при старте
        bool wantsExport = false;

        PBRMaps maps;

        // Callback экспорта PPM
        panel.onExportPPM = [&]() { wantsExport = true; };

        // ---- Главный цикл ----
        while (!renderer.shouldClose()) {
            renderer.pollEvents();

            // --- Горячие клавиши ---
            handleHotkeys(renderer.window(), params, viewMode, needsRegen, wantsExport);

            // --- Перегенерация текстур (CPU) ---
            if (needsRegen) {
                auto t0 = std::chrono::high_resolution_clock::now();

                generatePBR(maps, params);  // Diffuse + Height + Normal + Roughness

                auto t1 = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                panel.setGenerationTime(ms);

                // Загрузка новых текстур на GPU
                renderer.uploadPBRMaps(maps);

                std::cout << std::format(
                    "[Gen] {} | seed={} | octaves={} | {:.1f} ms\n",
                    MATERIAL_NAMES[params.materialType],
                    params.seed, params.octaves, ms);

                needsRegen = false;
            }

            // --- Экспорт PPM ---
            if (wantsExport) {
                std::string prefix = MATERIAL_NAMES[params.materialType];
                savePBRtoPPM(prefix, maps);
                std::cout << std::format(
                    "[Export] {}_diffuse/height/normal/roughness.ppm wrote.\n", prefix);
                wantsExport = false;
            }

            // --- Рендеринг ---
            renderer.clear(0.08f, 0.08f, 0.08f);
            renderer.drawQuad(viewMode);

            // --- ImGui ---
            panel.beginFrame();
            if (panel.drawPanel(params, viewMode)) {
                needsRegen = true; // Изменились параметры → перегенерация
            }
            panel.endFrame();

            renderer.swapBuffers();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
