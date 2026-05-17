// ============================================================================
// PBRGenerator.cpp — Реализация PBR-генераторов материалов
//
// Каждый генератор заполняет PBRMaps::diffuse, height, roughness за один проход,
// затем вызывает computeNormalMap() для построения карты нормалей.
//
// Оригинальная логика цвета полностью сохранена; добавлены height и roughness.
// ============================================================================

#include "PBRGenerator.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <format>

// ============================================================================
// computeNormalMap — оператор Собеля на карте высот
// ============================================================================
//
//  Фильтры:
//      Sobel X:          Sobel Y:
//      [-1  0  +1]       [-1 -2 -1]
//      [-2  0  +2]       [ 0  0  0]
//      [-1  0  +1]       [+1 +2 +1]
//
//  dX = sum(SobelX * neighborhood) / (8 * strength)
//  dY = sum(SobelY * neighborhood) / (8 * strength)
//  N  = normalize(vec3(-dX, -dY, 1.0))
//  Упаковка: R=(Nx+1)/2*255, G=(Ny+1)/2*255, B=(Nz+1)/2*255
// ============================================================================
void computeNormalMap(const std::vector<uint8_t>& heightmap,
                      std::vector<uint8_t>&        normalmap,
                      int width, int height, float strength)
{
    // Лямбда для безопасного сэмплирования с тайлингом (wrap around)
    auto sampleH = [&](int x, int y) -> float {
        x = (x + width)  % width;
        y = (y + height) % height;
        return heightmap[y * width + x] / 255.0f;
    };

    const float invStrength = 1.0f / (strength * 8.0f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Собираем 3×3 окрестность
            float h00 = sampleH(x - 1, y - 1), h10 = sampleH(x, y - 1), h20 = sampleH(x + 1, y - 1);
            float h01 = sampleH(x - 1, y    ),                           h21 = sampleH(x + 1, y    );
            float h02 = sampleH(x - 1, y + 1), h12 = sampleH(x, y + 1), h22 = sampleH(x + 1, y + 1);

            // Применяем фильтр Собеля
            float dX = (-h00 - 2.0f * h01 - h02 + h20 + 2.0f * h21 + h22) * invStrength;
            float dY = (-h00 - 2.0f * h10 - h20 + h02 + 2.0f * h12 + h22) * invStrength;

            // Строим вектор нормали и нормализуем
            float nx = -dX;
            float ny = -dY;
            float nz = 1.0f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }

            // Упаковка в [0, 255]: (n + 1) / 2 * 255
            size_t idx = static_cast<size_t>(y * width + x) * 3;
            normalmap[idx    ] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
            normalmap[idx + 1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
            normalmap[idx + 2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
        }
    }
}

// ============================================================================
// 1. STONE — камень с трещинами
// ============================================================================
void generateStonePBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);
    ValueNoise noise(p.seed);
    const double sc = p.baseScale;

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double nx = static_cast<double>(x) / sc;
            double ny = static_cast<double>(y) / sc;

            // --- Базовый шум (как в оригинале) ---
            double base_val = noise.fbm(nx, ny, p.octaves, p.persistence, p.lacunarity);

            // --- Трещины ---
            double cracks = 1.0 - std::abs(
                noise.fbm(nx * 2.0, ny * 2.0, 4, 0.5, p.lacunarity) - 0.5) * 3.0;
            bool inCrack = (cracks > 0.85);
            if (inCrack) base_val *= 0.5;

            // --- Diffuse (оригинальная логика) ---
            uint8_t col = static_cast<uint8_t>(clamp(base_val * 255.0, 0.0, 255.0));
            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di] = col;
            maps.diffuse[di + 1] = col;
            maps.diffuse[di + 2] = col;

            // --- Height — прямо из base_val ---
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(base_val * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Камень в целом шероховатый (0.65–1.0)
            // Трещины немного грубее, выступы чуть глаже
            double rough;
            if (inCrack) {
                rough = 0.90 + 0.10 * (1.0 - base_val); // трещины ≈ 0.90–1.00
            } else {
                rough = 0.55 + 0.35 * (1.0 - base_val); // выступы глаже, впадины грубее
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
        }
    }

    // Нормали вычисляются через Sobel из heightmap
    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 2. GRASS — трава с переходом почва → листья
// ============================================================================
void generateGrassPBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);
    ValueNoise noise(p.seed);
    const double sc = p.baseScale;

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double nx = static_cast<double>(x) / (sc * 0.5);
            double ny = static_cast<double>(y) / (sc * 0.5);

            double base_noise = noise.fbm(nx, ny, p.octaves, p.persistence, p.lacunarity);
            double gradient   = static_cast<double>(y) / TEX_HEIGHT;
            double spikes     = noise.fbm(nx * 10.0, ny * 10.0, 2, 0.3);

            // --- Diffuse (оригинальная логика) ---
            double r = lerp(90.0, 30.0,  gradient) + (spikes * 40.0 - 20.0);
            double g = lerp(60.0, 150.0, gradient) + (spikes * 50.0 - 20.0);
            double b = lerp(30.0, 40.0,  gradient) + (spikes * 20.0 - 10.0);
            r *= (0.8 + 0.4 * base_noise);
            g *= (0.8 + 0.4 * base_noise);
            b *= (0.8 + 0.4 * base_noise);

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di    ] = static_cast<uint8_t>(clamp(r, 0.0, 255.0));
            maps.diffuse[di + 1] = static_cast<uint8_t>(clamp(g, 0.0, 255.0));
            maps.diffuse[di + 2] = static_cast<uint8_t>(clamp(b, 0.0, 255.0));

            // --- Height ---
            // Высота = базовый шум + лезвия трав (spikes) для деталей
            double h = clamp(base_noise * 0.7 + spikes * 0.3, 0.0, 1.0);
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(h * 255.0);

            // --- Roughness ---
            // Почва (низ, gradient≈0) грубее, трава (верх, gradient≈1) немного глаже
            // Лезвия травы (высокий spikes) чуть глаже (восковой налёт)
            double rough = lerp(0.85, 0.60, gradient) - spikes * 0.15;
            rough = clamp(rough + (1.0 - base_noise) * 0.15, 0.0, 1.0);
            maps.roughness[hi] = static_cast<uint8_t>(rough * 255.0);
        }
    }

    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 3. LAVA — лава с доменным искажением
// ============================================================================
void generateLavaPBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);
    ValueNoise noise(p.seed);
    const double sc = p.baseScale * 2.0; // лава использует более крупный масштаб

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double nx = static_cast<double>(x) / sc;
            double ny = static_cast<double>(y) / sc;

            // Domain Warping (оригинальная логика)
            double distort_x = nx + std::sin(ny * 5.0) * 0.1
                              + noise.fbm(nx, ny, 3, 0.5) * 0.2;
            double distort_y = ny + std::cos(nx * 5.0) * 0.1
                              + noise.fbm(nx + 10.0, ny + 10.0, 3, 0.5) * 0.2;

            double val = noise.fbm(distort_x * 4.0, distort_y * 4.0,
                                   p.octaves, p.persistence, p.lacunarity);

            // --- Diffuse (оригинальная палитра) ---
            double r = 0, g = 0, b_col = 0;
            if (val < 0.4) {
                r     = lerp(20.0,  150.0, val / 0.4);
                g     = lerp(0.0,   20.0,  val / 0.4);
                b_col = 0;
            } else if (val < 0.7) {
                double t = (val - 0.4) / 0.3;
                r     = lerp(150.0, 255.0, t);
                g     = lerp(20.0,  120.0, t);
                b_col = 0;
            } else {
                double t = (val - 0.7) / 0.3;
                r     = 255.0;
                g     = lerp(120.0, 255.0, t);
                b_col = lerp(0.0,   255.0, t);
            }

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di    ] = static_cast<uint8_t>(clamp(r,     0.0, 255.0));
            maps.diffuse[di + 1] = static_cast<uint8_t>(clamp(g,     0.0, 255.0));
            maps.diffuse[di + 2] = static_cast<uint8_t>(clamp(b_col, 0.0, 255.0));

            // --- Height ---
            // val напрямую представляет «плотность» лавы/камня
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(val * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Горящая лава (val > 0.7): почти зеркальная (rough ≈ 0.05–0.2)
            // Остывшая лава (val < 0.4): крайне шероховатая (rough ≈ 0.85–1.0)
            double rough;
            if (val >= 0.7) {
                double t = (val - 0.7) / 0.3;
                rough = lerp(0.20, 0.05, t); // чем ярче — тем глаже
            } else if (val >= 0.4) {
                double t = (val - 0.4) / 0.3;
                rough = lerp(0.85, 0.20, t);
            } else {
                rough = lerp(1.00, 0.85, val / 0.4);
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
        }
    }

    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 4. WOOD — кора дерева со структурой волокон
// ============================================================================
void generateWoodPBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);
    ValueNoise noise(p.seed);

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double nx = static_cast<double>(x) / TEX_WIDTH;
            double ny = static_cast<double>(y) / TEX_HEIGHT;

            // Цилиндрическое затенение (оригинал)
            double radial = std::pow(1.0 - std::abs(nx - 0.5) * 2.0, 0.6);

            // Вертикальная структура (пластины/чешуйки)
            double v_struct = noise.fbm(nx * 6.0, ny * 1.2, p.octaves, p.persistence, p.lacunarity);
            double plates   = 1.0 - std::abs(v_struct - 0.5) * 2.0;
            plates          = std::pow(plates, 1.8);

            // Сеть трещин
            double fissures = noise.fbm(nx * 3.0, ny * 1.0, 3, 0.45);
            fissures        = std::abs(fissures - 0.5) * 2.0;
            fissures        = std::pow(fissures, 2.5);

            // Зерно и поры
            double grain = noise.fbm(nx * 60.0, ny * 60.0, 2, 0.25);
            grain        = (grain - 0.5) * 0.4;

            double micro = (noise.get(nx * 15.0, ny * 15.0) - 0.5) * 0.25;

            // Комбинация (оригинал)
            double bark_raw = clamp(plates * 0.55 + fissures * 0.35 + grain + micro, 0.0, 1.0);
            double bark     = std::pow(bark_raw, 0.85);

            // Палитра (оригинал)
            double r_deep = 55.0,  g_deep = 32.0,  b_deep = 22.0;
            double r_mid  = 115.0, g_mid  = 68.0,  b_mid  = 48.0;
            double r_high = 165.0, g_high = 115.0, b_high = 95.0;

            double t_deep = std::pow(1.0 - bark, 1.8);
            double r = lerp(r_mid, r_deep, t_deep);
            double g = lerp(g_mid, g_deep, t_deep);
            double b = lerp(b_mid, b_deep, t_deep);

            double t_high = std::pow(bark, 1.5) * 0.5;
            r = lerp(r, r_high, t_high);
            g = lerp(g, g_high, t_high);
            b = lerp(b, b_high, t_high);

            double saturation_boost = 1.35;
            double gray = (r + g + b) / 3.0;
            r = clamp(gray + (r - gray) * saturation_boost, 0.0, 255.0);
            g = clamp(gray + (g - gray) * saturation_boost, 0.0, 255.0);
            b = clamp(gray + (b - gray) * saturation_boost, 0.0, 255.0);

            r = lerp(r * 0.80, r, radial * 0.75);
            g = lerp(g * 0.80, g, radial * 0.75);
            b = lerp(b * 0.80, b, radial * 0.75);

            double final_grain = (noise.get(nx * 80.0, ny * 80.0) - 0.5) * 9.0;
            r = clamp(r + final_grain, 35.0, 175.0);
            g = clamp(g + final_grain, 20.0, 150.0);
            b = clamp(b + final_grain, 10.0, 120.0);

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di    ] = static_cast<uint8_t>(r);
            maps.diffuse[di + 1] = static_cast<uint8_t>(g);
            maps.diffuse[di + 2] = static_cast<uint8_t>(b);

            // --- Height ---
            // bark_raw — прямая высота: гребни пластин высокие, трещины низкие
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(bark_raw * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Трещины (fissures высокий, bark_raw низкий) — очень шероховатые
            // Гребни пластин — умеренно шероховатые (смоляной налёт)
            double rough = 0.35 + 0.55 * (1.0 - bark_raw);
            // Дополнительно: зерно добавляет микрошероховатость
            rough = clamp(rough + std::abs(grain) * 0.2, 0.0, 1.0);
            maps.roughness[hi] = static_cast<uint8_t>(rough * 255.0);
        }
    }

    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// Диспетчер генерации
// ============================================================================
void generatePBR(PBRMaps& maps, const GeneratorParams& params)
{
    switch (params.materialType) {
        case 0: generateStonePBR(maps, params); break;
        case 1: generateGrassPBR(maps, params); break;
        case 2: generateLavaPBR (maps, params); break;
        case 3: generateWoodPBR (maps, params); break;
        default: generateStonePBR(maps, params); break;
    }
}

// ============================================================================
// Экспорт всех 4 карт в PPM-файлы (P6 binary)
// ============================================================================
static void writeP6(const std::string& path,
                    const std::vector<uint8_t>& data,
                    int w, int h, bool grayscale)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error(std::format("Cannot open: {}", path));

    if (grayscale) {
        // Конвертируем grayscale → RGB для совместимости с PPM
        out << "P6\n" << w << " " << h << "\n255\n";
        for (int i = 0; i < w * h; ++i) {
            uint8_t v = data[i];
            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
        }
    } else {
        out << "P6\n" << w << " " << h << "\n255\n";
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

void savePBRtoPPM(const std::string& prefix, const PBRMaps& maps)
{
    writeP6(prefix + "_diffuse.ppm",   maps.diffuse,   maps.width, maps.height_px, false);
    writeP6(prefix + "_height.ppm",    maps.height,    maps.width, maps.height_px, true);
    writeP6(prefix + "_normal.ppm",    maps.normal,    maps.width, maps.height_px, false);
    writeP6(prefix + "_roughness.ppm", maps.roughness, maps.width, maps.height_px, true);
}
