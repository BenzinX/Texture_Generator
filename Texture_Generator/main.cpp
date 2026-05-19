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
#include <algorithm>
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
                           bool& wantsExport,
                           bool& show3DPreview)
{
    // Игнорируем горячие клавиши, если ImGui захватил клавиатуру
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // 1–4: выбор материала
    static bool keyWasDown[12] = {};
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

    // V (View)
    if (checkKey(GLFW_KEY_V, 10)) {
        show3DPreview = !show3DPreview;
    }
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

        // --- Переключатель режимов ---
        bool show3DPreview = false;

        // --- Параметры орбитальной камеры ---
        float camTheta = 45.0f;   // азимут в градусах [0, 360]
        float camPhi   = 60.0f;   // полярный угол в градусах [5, 175]
        float camDist  = 2.5f;    // расстояние от центра [0.5, 10]

        // --- Параметры источника света ---
        LightParams light;        // инициализируется default-значениями из Renderer.h

        // --- Состояние перетаскивания мышью (орбит-камера) ---
        bool   s_dragging    = false;
        double s_lastMouseX  = 0.0;
        double s_lastMouseY  = 0.0;

        PBRMaps maps;

        // Callback экспорта PPM
        panel.onExportPPM = [&]() { wantsExport = true; };

        // ---- Главный цикл ----
        while (!renderer.shouldClose()) {
            renderer.pollEvents();

            // Drag работает только в 3D-режиме, когда ImGui не захватил мышь
            if (show3DPreview && !ImGui::GetIO().WantCaptureMouse) {
                if (glfwGetMouseButton(renderer.window(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                    double mx, my;
                    glfwGetCursorPos(renderer.window(), &mx, &my);
                    if (!s_dragging) {
                        s_dragging   = true;
                        s_lastMouseX = mx;
                        s_lastMouseY = my;
                    } else {
                        // 0.3 deg/px — достаточно чувствительно для орбиты
                        camTheta += static_cast<float>((mx - s_lastMouseX) * 0.3);
                        camPhi   += static_cast<float>((my - s_lastMouseY) * 0.3);
                        camPhi    = std::clamp(camPhi, 5.0f, 175.0f);
                        // theta оборачиваем в [0, 360]
                        while (camTheta <   0.0f) camTheta += 360.0f;
                        while (camTheta > 360.0f) camTheta -= 360.0f;
                        s_lastMouseX = mx;
                        s_lastMouseY = my;
                    }
                } else {
                    s_dragging = false;
                }
            } else {
                s_dragging = false;  // сброс при переключении режима
            }

            // --- Горячие клавиши ---
            handleHotkeys(renderer.window(), params, viewMode, needsRegen, wantsExport, show3DPreview);

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

            if (show3DPreview) {
                renderer.drawPBRScene(camDist, camTheta, camPhi, light);
            } else {
                renderer.drawQuad(viewMode);
            }

            // --- ImGui ---
            panel.beginFrame();
            ImGui::SetNextWindowPos(ImVec2(360.0f, 10.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);

            ImGuiWindowFlags pbrFlags = ImGuiWindowFlags_NoScrollbar
                                      | ImGuiWindowFlags_AlwaysAutoResize;

            if (ImGui::Begin("3D PBR Preview", nullptr, pbrFlags)) {

                // --- Переключатель режима ---
                ImGui::SeparatorText("View Mode");
                ImGui::Checkbox("Enable 3D Preview", &show3DPreview);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Переключение между 2D картами и 3D-превью на сфере.\n"
                                      "Drag левой кнопкой мыши для вращения камеры.");

                if (show3DPreview) {

                    ImGui::Spacing();
                    ImGui::SeparatorText("Orbital Camera");

                    ImGui::SliderFloat("Distance",    &camDist,  0.5f, 10.0f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Расстояние камеры от центра сцены.");

                    ImGui::SliderFloat("Azimuth (θ)", &camTheta, 0.0f, 360.0f, "%.1f°");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Горизонтальное вращение камеры.\nТакже управляется drag мышью.");

                    ImGui::SliderFloat("Elevation (φ)", &camPhi, 5.0f, 175.0f, "%.1f°");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Вертикальный наклон камеры.\nОграничен от полюсов (5–175°).");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Light");

                    ImGui::DragFloat3("Position", light.position, 0.05f, -20.0f, 20.0f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Позиция point light в мировых координатах.");

                    ImGui::ColorEdit3("Color", light.color);
                    ImGui::SliderFloat("Intensity", &light.intensity, 0.1f, 100.0f, "%.1f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Мощность источника (в условных ваттах).\n"
                                          "Высокие значения дают видимые блики при большом расстоянии.");

                    ImGui::Spacing();
                    ImGui::SeparatorText("Ambient");
                    ImGui::ColorEdit3("Ambient Color", light.ambient);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Фоновый свет — заполняет тени.\n"
                                          "Слишком высокий убивает контраст BRDF.");

                    ImGui::Spacing();
                    ImGui::TextDisabled("LMB drag — orbit camera");
                }
            }
            ImGui::End();
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
