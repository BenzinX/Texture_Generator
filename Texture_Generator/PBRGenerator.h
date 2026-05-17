#pragma once
// ============================================================================
// PBRGenerator.h — Структуры и объявления PBR-генератора
// Генерирует 4 карты на материал: Diffuse, Height, Normal, Roughness
// ============================================================================

#include "NoiseCore.h"
#include <vector>
#include <cstdint>
#include <span>
#include <string>

// ============================================================================
// Параметры генератора — передаются из ImGui-панели
// ============================================================================
struct GeneratorParams {
    int          materialType   = 0;      // 0=Stone 1=Grass 2=Lava 3=Wood
    unsigned int seed           = 1337;
    int          octaves        = 6;
    double       persistence    = 0.5;
    double       lacunarity     = 2.0;
    double       baseScale      = 64.0;  // делитель UV (больше = крупнее паттерн)
    float        normalStrength = 6.0f;  // усиление карты нормалей (Sobel)
};

// ============================================================================
// PBRMaps — 4 карты текстур одного материала (CPU-буферы)
// ============================================================================
struct PBRMaps {
    // Диффузная карта: RGB, 3 байта на пиксель
    std::vector<uint8_t> diffuse;

    // Карта высот: grayscale, 1 байт на пиксель
    std::vector<uint8_t> height;

    // Карта нормалей: RGB, 3 байта на пиксель (вычисляется через Sobel из height)
    std::vector<uint8_t> normal;

    // Карта шероховатости: grayscale, 1 байт на пиксель
    std::vector<uint8_t> roughness;

    int width  = TEX_WIDTH;
    int height_px = TEX_HEIGHT;

    // Инициализация буферов нужного размера
    void allocate(int w, int h) {
        width     = w;
        height_px = h;
        diffuse  .assign(w * h * 3, 0);
        height   .assign(w * h,     0);
        normal   .assign(w * h * 3, 0);
        roughness.assign(w * h,     0);
    }
};

// ============================================================================
// Публичный API генераторов материалов
// ============================================================================

// Вычислить карту нормалей через оператор Собеля на heightmap
// strength — коэффициент усиления градиента (рекомендуется 4.0–12.0)
void computeNormalMap(const std::vector<uint8_t>& heightmap,
                      std::vector<uint8_t>&        normalmap,
                      int width, int height, float strength);

// Генераторы материалов (заполняют все 4 буфера в PBRMaps)
void generateStonePBR(PBRMaps& maps, const GeneratorParams& p);
void generateGrassPBR(PBRMaps& maps, const GeneratorParams& p);
void generateLavaPBR (PBRMaps& maps, const GeneratorParams& p);
void generateWoodPBR (PBRMaps& maps, const GeneratorParams& p);

// Диспетчер: вызывает нужный генератор по params.materialType
void generatePBR(PBRMaps& maps, const GeneratorParams& params);

// Экспорт карты в PPM (для отладки и тестирования)
void savePBRtoPPM(const std::string& prefix, const PBRMaps& maps);
