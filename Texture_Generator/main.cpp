// ============================================================================
// main.cpp — Точка входа. Главный цикл рендеринга.
//
// Архитектура:
//   Renderer            — GLFW + GLAD, VAO/VBO, шейдеры, 4 GPU-текстуры
//   PBRGenerator        — CPU-генерация 4 карт (Diffuse/Height/Normal/Roughness)
//   TextureEditorPanel  — ImGui-панель с live-параметрами
//
// Зависимости (vcpkg x64-windows):
//   glfw3, glad, imgui[glfw-binding,opengl3-binding]
//
// Компиляция: Visual Studio 2022/2026, C++20, x64
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
#include <numbers>

// ============================================================================
// Имена материалов для имён файлов PPM
// ============================================================================
static constexpr const char* MATERIAL_NAMES[] = {
    "stone", "grass", "lava", "wood"
};

// ============================================================================
// handleHotkeys — обработка нажатий GLFW без ImGui
// ============================================================================
static void handleHotkeys(GLFWwindow* window,
    GeneratorParams& params,
    int& viewMode,
    bool& needsRegen,
    bool& wantsExport)
{
    // Игнорируем горячие клавиши, если ImGui захватил клавиатуру
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // Вспомогательная лямбда: true только в момент нажатия (edge-триггер)
    static bool keyWasDown[11] = {};
    auto checkKey = [&](int key, int idx) -> bool {
        bool down = glfwGetKey(window, key) == GLFW_PRESS;
        bool pressed = down && !keyWasDown[idx];
        keyWasDown[idx] = down;
        return pressed;
        };

    // 1–4: выбор материала
    if (checkKey(GLFW_KEY_1, 0)) { params.materialType = 0; needsRegen = true; }
    if (checkKey(GLFW_KEY_2, 1)) { params.materialType = 1; needsRegen = true; }
    if (checkKey(GLFW_KEY_3, 2)) { params.materialType = 2; needsRegen = true; }
    if (checkKey(GLFW_KEY_4, 3)) { params.materialType = 3; needsRegen = true; }

    // D/H/N/R: режимы 2D-просмотра
    if (checkKey(GLFW_KEY_D, 4)) viewMode = 0;
    if (checkKey(GLFW_KEY_H, 5)) viewMode = 1;
    if (checkKey(GLFW_KEY_N, 6)) viewMode = 2;
    if (checkKey(GLFW_KEY_R, 7)) viewMode = 3;

    // V: 3D-режим просмотра
    if (checkKey(GLFW_KEY_V, 10)) viewMode = 4;

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
        Renderer renderer(1280, 800, "PBR Texture Generator — GLFW/GLAD/ImGui");

        // ---- Инициализация ImGui-панели ----
        TextureEditorPanel panel(renderer.window());

        // ---- Начальные параметры ----
        GeneratorParams params;
        params.materialType = 0;     // Stone
        params.seed = 1337;
        params.octaves = 6;
        params.persistence = 0.5;
        params.lacunarity = 2.0;
        params.baseScale = 64.0;
        params.normalStrength = 6.0f;

        // Параметры 3D-освещения
        LightParams light;
        light.azimuth = 45.0f;
        light.elevation = 35.0f;
        light.intensity = 1.2f;
        light.ambient = 0.15f;
        light.specPower = 32.0f;
        light.specIntens = 0.6f;
        light.autoRotate = false;
        light.rotAngleX = -0.35f; // небольшой наклон для лучшего вида
        light.rotAngleY = 0.0f;

        int  viewMode = 0;     // 0=Diffuse
        bool needsRegen = true;  // Генерируем сразу при старте
        bool wantsExport = false;

        PBRMaps maps;

        // Callback экспорта PPM
        panel.onExportPPM = [&]() { wantsExport = true; };

        // Счётчик времени для авто-вращения плоскости в 3D-режиме
        double autoRotTime = 0.0;

        // ---- Главный цикл ----
        while (!renderer.shouldClose()) {
            renderer.pollEvents();

            // --- Горячие клавиши ---
            handleHotkeys(renderer.window(), params, viewMode, needsRegen, wantsExport);

            // --- Авто-вращение плоскости в 3D-режиме ---
            if (viewMode == 4 && light.autoRotate) {
                autoRotTime += 0.016; // приблизительно 60 FPS
                light.rotAngleY = static_cast<float>(autoRotTime * 0.4);
                light.rotAngleX = -0.35f + std::sin(static_cast<float>(autoRotTime * 0.25)) * 0.15f;
            }

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
            renderer.drawQuad(viewMode, light);

            // --- ImGui ---
            panel.beginFrame();
            if (panel.drawPanel(params, viewMode, light)) {
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