// ============================================================================
// ImGuiSetup.cpp — Реализация панели редактора на Dear ImGui
// ============================================================================

#include "ImGuiSetup.h"
#include <format>
#include <array>

// ============================================================================
// Конструктор — инициализация Dear ImGui с бэкендами GLFW и OpenGL3
// ============================================================================
TextureEditorPanel::TextureEditorPanel(GLFWwindow* window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // навигация с клавиатуры
    io.IniFilename  = "imgui_pbr.ini"; // сохранение позиций окон

    applyCustomStyle();

    // Бэкенд GLFW — true = ImGui перехватывает callbacks (мышь, клавиатура)
    ImGui_ImplGlfw_InitForOpenGL(window, true);

    // Бэкенд OpenGL3 с GLSL 3.30
    ImGui_ImplOpenGL3_Init("#version 330");
}

// ============================================================================
// Деструктор — корректная очистка ImGui
// ============================================================================
TextureEditorPanel::~TextureEditorPanel()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ============================================================================
// applyCustomStyle — тёмная тема с небольшими настройками
// ============================================================================
void TextureEditorPanel::applyCustomStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding  = 6.0f;
    s.FrameRounding   = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding    = 4.0f;
    s.WindowPadding   = ImVec2(12.0f, 12.0f);
    s.FramePadding    = ImVec2(8.0f, 4.0f);
    s.ItemSpacing     = ImVec2(8.0f, 6.0f);

    // Акцентный цвет — оранжевый (напоминает лаву)
    ImVec4* c = s.Colors;
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.65f, 0.30f, 0.05f, 1.0f);
    c[ImGuiCol_CheckMark]      = ImVec4(1.00f, 0.65f, 0.10f, 1.0f);
    c[ImGuiCol_SliderGrab]     = ImVec4(0.90f, 0.50f, 0.05f, 1.0f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(1.00f, 0.65f, 0.10f, 1.0f);
    c[ImGuiCol_Button]         = ImVec4(0.50f, 0.25f, 0.05f, 1.0f);
    c[ImGuiCol_ButtonHovered]  = ImVec4(0.75f, 0.38f, 0.07f, 1.0f);
    c[ImGuiCol_ButtonActive]   = ImVec4(1.00f, 0.55f, 0.10f, 1.0f);
    c[ImGuiCol_Header]         = ImVec4(0.60f, 0.28f, 0.05f, 0.80f);
    c[ImGuiCol_HeaderHovered]  = ImVec4(0.80f, 0.40f, 0.07f, 1.0f);
    c[ImGuiCol_HeaderActive]   = ImVec4(1.00f, 0.55f, 0.10f, 1.0f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_Tab]            = ImVec4(0.35f, 0.18f, 0.04f, 1.0f);
    c[ImGuiCol_TabActive]      = ImVec4(0.65f, 0.30f, 0.05f, 1.0f);
    c[ImGuiCol_TabHovered]     = ImVec4(0.80f, 0.40f, 0.07f, 1.0f);
}

// ============================================================================
// beginFrame — запускает новый ImGui-фрейм (вызывать до рисования UI)
// ============================================================================
void TextureEditorPanel::beginFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

// ============================================================================
// drawPanel — главная панель управления генератором
// Возвращает true если нужна перегенерация текстур
// ============================================================================
bool TextureEditorPanel::drawPanel(GeneratorParams& params, int& viewMode)
{
    bool changed = false;

    // Закрепить панель в левом верхнем углу (только первый раз)
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("PBR Texture Generator", nullptr, flags)) {
        ImGui::End();
        return false;
    }

    // ---- Выбор материала ----
    ImGui::SeparatorText("Material");
    {
        static constexpr std::array<const char*, 4> types = {
            "Stone", "Grass",
            "Lava",   "Wood"
        };
        int prevType = params.materialType;
        if (ImGui::BeginCombo("##material", types[params.materialType])) {
            for (int i = 0; i < 4; ++i) {
                bool selected = (params.materialType == i);
                if (ImGui::Selectable(types[i], selected)) {
                    params.materialType = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (params.materialType != prevType) changed = true;
    }

    ImGui::Spacing();

    // ---- Параметры шума ----
    ImGui::SeparatorText("Noise Parameters");

    // Seed — используем int для слайдера, конвертируем в uint
    {
        int seed = static_cast<int>(params.seed);
        if (ImGui::SliderInt("Seed", &seed, 0, 99999)) {
            params.seed = static_cast<unsigned int>(seed);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Random Number Generator Seed.\nChanges the unique noise pattern.");
    }

    {
        if (ImGui::SliderInt("Octaves", &params.octaves, 1, 10)) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Number of FBM octaves.\nMore = more detail, but slower.");
    }

    {
        auto pers = static_cast<float>(params.persistence);
        if (ImGui::SliderFloat("Persistence", &pers, 0.10f, 0.95f, "%.2f")) {
            params.persistence = static_cast<double>(pers);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Amplitude attenuation between octaves.\n"
                              "0.5 = standard FBM.");
    }

    {
        auto lac = static_cast<float>(params.lacunarity);
        if (ImGui::SliderFloat("Lacunarity", &lac, 1.0f, 4.0f, "%.2f")) {
            params.lacunarity = static_cast<double>(lac);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Octave frequency multiplier.\n"
                              "2.0 = standard FBM.");
    }

    {
        auto sc = static_cast<float>(params.baseScale);
        if (ImGui::SliderFloat("Base Scale", &sc, 8.0f, 256.0f, "%.1f")) {
            params.baseScale = static_cast<double>(sc);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("UV coordinate divider.\n"
                              "More = larger pattern.");
    }

    ImGui::Spacing();

    // ---- Параметры нормалей ----
    ImGui::SeparatorText("Normal Map");
    {
        if (ImGui::SliderFloat("Strength", &params.normalStrength, 0.5f, 20.0f, "%.1f")) {
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sobel gradient enhancement.\n"
                              "More = more pronounced relief in the normal map.");
    }

    ImGui::Spacing();

    // ---- Выбор режима просмотра ----
    ImGui::SeparatorText("View Mode");
    {
        bool v0 = (viewMode == 0), v1 = (viewMode == 1),
             v2 = (viewMode == 2), v3 = (viewMode == 3);

        if (ImGui::RadioButton("Diffuse",   v0)) viewMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Height",    v1)) viewMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Normal",    v2)) viewMode = 2;
        ImGui::SameLine();
        if (ImGui::RadioButton("Roughness", v3)) viewMode = 3;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Быстрые действия ----
    if (ImGui::Button("Regenerate Now", ImVec2(160.0f, 0.0f))) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Export PPM", ImVec2(140.0f, 0.0f))) {
        if (onExportPPM) onExportPPM();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save 4 PPM-files:\n"
                          "prefix_diffuse.ppm\n"
                          "prefix_height.ppm\n"
                          "prefix_normal.ppm\n"
                          "prefix_roughness.ppm");

    ImGui::Spacing();

    // ---- Статистика ----
    ImGui::SeparatorText("Info");
    {
        static constexpr std::array<const char*, 4> matNames = {
            "Stone", "Grass", "Lava", "Wood"
        };
        ImGui::TextDisabled("Material : %s", matNames[params.materialType]);
        ImGui::TextDisabled("Texture  : %d x %d px", TEX_WIDTH, TEX_HEIGHT);
        if (m_lastGenMs > 0.0) {
            ImGui::TextDisabled("Gen time : %.1f ms", m_lastGenMs);
        }
        ImGui::TextDisabled("FPS      : %.0f", ImGui::GetIO().Framerate);
    }

    ImGui::Spacing();

    // ---- Справка по горячим клавишам ----
    if (ImGui::CollapsingHeader("Hotkeys")) {
        ImGui::BulletText("ESC - close window");
        ImGui::BulletText("1/2/3/4 - select material");
        ImGui::BulletText("D/H/N/R - View mode");
        ImGui::BulletText("E - export to .ppm");
        ImGui::BulletText("Space - regenerate");
    }

    ImGui::End();

    return changed;
}

// ============================================================================
// endFrame — финализация ImGui-фрейма, отправка draw calls
// ============================================================================
void TextureEditorPanel::endFrame()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
