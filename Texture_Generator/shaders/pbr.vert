#version 330 core
// ============================================================================
// pbr.vert — Вершинный шейдер для PBR-превью сферы
// Передаёт: vUV, vNormal (мировое пространство), vWorldPos
// Матрицы: column-major float[16], вычислены на CPU без glm
// ============================================================================

layout(location = 0) in vec3 aPos;     // позиция вершины (единичная сфера)
layout(location = 1) in vec2 aUV;      // UV-координаты
layout(location = 2) in vec3 aNormal;  // нормаль в объектном пространстве

out vec2 vUV;
out vec3 vNormal;
out vec3 vWorldPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    // Мировая позиция вершины
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;

    // Нормаль в мировом пространстве.
    // Для единичной сферы uModel = identity → mat3(uModel) = mat3(1).
    // При равномерном масштабировании mat3(uModel) корректна без transpose(inverse).
    vNormal = mat3(uModel) * aNormal;

    vUV = aUV;

    gl_Position = uProjection * uView * worldPos;
}