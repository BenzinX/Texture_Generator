// ============================================================================
// verify_test.cpp — Минимальная проверка GLFW + GLAD + ImGui
// Компилируйте и запустите ЭТО ОТДЕЛЬНО перед интеграцией с основным кодом.
//
// Ожидаемый результат:
//   - Окно 640×480 с серым фоном
//   - Поверх окна — окно Dear ImGui Demo Window
//   - В консоли — версия OpenGL 3.3+
//
// Сборка (vcpkg integrate install уже должен быть выполнен):
//   Создайте новый проект в VS, добавьте этот файл, x64 Debug/Release.
// ============================================================================

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <format>

int main()
{
    // --- GLFW ---
    if (!glfwInit()) {
        std::cerr << "FAIL: glfwInit\n";
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* win = glfwCreateWindow(640, 480, "Verify: GLFW+GLAD+ImGui", nullptr, nullptr);
    if (!win) { std::cerr << "FAIL: glfwCreateWindow\n"; glfwTerminate(); return 1; }

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // --- GLAD ---
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::cerr << "FAIL: gladLoadGLLoader\n";
        glfwTerminate();
        return 1;
    }

    // Вывести версию — должна быть 3.3 или выше
    std::cout << std::format("OK: OpenGL  = {}\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::cout << std::format("OK: GLSL    = {}\n", reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
    std::cout << std::format("OK: GPU     = {}\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // --- ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    std::cout << "OK: ImGui   = " << IMGUI_VERSION << "\n";

    // --- Цикл ---
    bool showDemo = true;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);
        ImGui::Render();

        glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(win);
    }

    // --- Cleanup ---
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();

    std::cout << "Verify PASSED — все зависимости работают корректно.\n";
    return 0;
}
