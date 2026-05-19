// ============================================================================
// PBRGenerator.cpp — Реализация PBR-генераторов материалов
//
// Каждый генератор заполняет PBRMaps::diffuse, height, roughness за один
// проход, затем вызывает computeNormalMap() для построения карты нормалей.
//
// Математическая база:
//   Stone  — GradientNoise FBM + WorleyNoise жилы/трещины + слоистость
//   Grass  — GradientNoise FBM + ValueNoise лезвия + domain warping для корней
//   Wood   — GradientNoise волокна + WorleyNoise сучки + ValueNoise микрорельеф
//   Lava   — исходная логика адаптированная под 1024 и общий пайплайн
// ============================================================================

#include "PBRGenerator.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <format>
#include <numbers>

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
    std::vector<uint8_t>& normalmap,
    int width, int height, float strength)
{
    // Лямбда для безопасного сэмплирования с тайлингом (wrap around)
    auto sampleH = [&](int x, int y) -> float {
        x = (x + width) % width;
        y = (y + height) % height;
        return heightmap[static_cast<size_t>(y * width + x)] / 255.0f;
        };

    const float invStrength = 1.0f / (strength * 8.0f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Собираем 3×3 окрестность
            float h00 = sampleH(x - 1, y - 1), h10 = sampleH(x, y - 1), h20 = sampleH(x + 1, y - 1);
            float h01 = sampleH(x - 1, y), h21 = sampleH(x + 1, y);
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
            normalmap[idx] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
            normalmap[idx + 1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
            normalmap[idx + 2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
        }
    }
}

// ============================================================================
// 1. STONE — слоистый камень с прожилками, трещинами, вариациями шероховатости
//
// Алгоритм:
//   a) Базовая форма: GradientNoise FBM — органичная, без резких переходов
//   b) Слоистость: периодическая функция по «глубине» + domain warp для изломов
//   c) Прожилки (минеральные жилы): WorleyNoise F2-F1 — создаёт тонкие
//      «паутинные» линии, характерные для кристаллических жил в породе
//   d) Микротрещины: турбулентный FBM с высокой частотой — острые линии
//   e) Вариации шероховатости: гладкие выступы vs шероховатые трещины
// ============================================================================
void generateStonePBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);

    // Основной шум для структуры камня
    GradientNoise gNoise(p.seed);
    // Второй шум для жил (разный seed → независимые узоры)
    GradientNoise veinNoise(p.seed + 7);
    // Шум для микротрещин
    ValueNoise    crackNoise(p.seed + 13);
    // Клеточный шум для прожилок (16 ячеек в пространстве [0,1])
    WorleyNoise   worley(p.seed + 31, 12);

    const double sc = p.baseScale;

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double nx = static_cast<double>(x) / sc;
            double ny = static_cast<double>(y) / sc;

            // --- a) Базовая форма: градиентный FBM ---
            double base = gNoise.fbm(nx, ny, p.octaves, p.persistence, p.lacunarity);

            // --- b) Слоистость каменной породы ---
            // Domain warping: смещаем координаты шумом для изломов слоёв
            double warpX = gNoise.fbm(nx + 1.7, ny + 9.2, 3, 0.5) * 0.8;
            double warpY = gNoise.fbm(nx + 8.3, ny + 2.8, 3, 0.5) * 0.8;

            // Периодическая слоистость — косая (угол ~15°) с небольшим смещением
            double layerCoord = (nx + warpX) * 0.85 + (ny + warpY) * 0.15;
            // sin с узкими пиками: pow(|sin|, 0.3) → широкие «плиты» + тонкие грани
            double layers = std::pow(std::abs(std::sin(layerCoord * std::numbers::pi * 3.5)), 0.28);

            // Добавляем мелкие дополнительные слои (тонкие прослойки)
            double microLayers = gNoise.fbm(nx * 1.8 + 0.5, ny * 1.2 + 3.1, 3, 0.45) * 0.25;

            double stoneBase = clamp(base * 0.45 + layers * 0.40 + microLayers, 0.0, 1.0);

            // --- c) Прожилки (минеральные жилы) через Worley F2-F1 ---
            // F2-F1 максимален на границах ячеек Вороного — это и есть прожилки
            double uvX = static_cast<double>(x) / TEX_WIDTH;
            double uvY = static_cast<double>(y) / TEX_HEIGHT;

            // Добавляем небольшой warp для извивистости прожилок
            double vwX = veinNoise.fbm(uvX * 4.0, uvY * 4.0, 3, 0.5) * 0.15;
            double vwY = veinNoise.fbm(uvX * 4.0 + 5.0, uvY * 4.0 + 5.0, 3, 0.5) * 0.15;
            double veins = worley.f2_minus_f1(uvX + vwX, uvY + vwY);

            // Узкие жилы: экспоненциальное заострение
            double veinMask = std::pow(clamp(1.0 - veins * 2.5, 0.0, 1.0), 3.0);

            // --- d) Микротрещины: турбулентный высокочастотный шум ---
            double crackFreq = sc * 0.6;
            double cracks = crackNoise.turbulence(
                static_cast<double>(x) / crackFreq,
                static_cast<double>(y) / crackFreq,
                4, 0.5);
            // Трещины — там где turbulence близко к 0 (впадины турбулентного поля)
            double crackMask = std::pow(clamp(cracks * 1.8, 0.0, 1.0), 2.0);
            bool inCrack = (cracks < 0.28);

            // --- Итоговая высота ---
            double heightVal = stoneBase;
            if (inCrack) {
                // Трещины углубляют поверхность
                heightVal = clamp(heightVal * 0.45 - 0.05, 0.0, 1.0);
            }
            // Прожилки слегка выступают над поверхностью
            heightVal = clamp(heightVal + veinMask * 0.08, 0.0, 1.0);

            // --- Diffuse (цветовая карта) ---
            // Базовый цвет: холодный серый с голубоватым оттенком
            double baseGray = 0.38 + stoneBase * 0.32;
            double r_stone = baseGray - 0.02;
            double g_stone = baseGray;
            double b_stone = baseGray + 0.04;

            // Тёплые вариации: некоторые слои имеют охристый оттенок (железо, глина)
            double warmTint = std::max(0.0, layers - 0.6) * 0.8;
            r_stone += warmTint * 0.12;
            g_stone += warmTint * 0.07;
            b_stone -= warmTint * 0.02;

            // Прожилки: белесые/кварцевые
            r_stone = lerp(r_stone, 0.88, veinMask * 0.6);
            g_stone = lerp(g_stone, 0.85, veinMask * 0.6);
            b_stone = lerp(b_stone, 0.82, veinMask * 0.6);

            // Трещины: тёмно-серые с лёгким коричневым (окисление)
            if (inCrack) {
                double crackDepth = clamp(1.0 - cracks / 0.28, 0.0, 1.0);
                r_stone = lerp(r_stone, 0.14 + crackDepth * 0.04, crackDepth * 0.85);
                g_stone = lerp(g_stone, 0.11 + crackDepth * 0.02, crackDepth * 0.85);
                b_stone = lerp(b_stone, 0.10, crackDepth * 0.85);
            }

            // Финальный вариационный шум (микрорельеф пятна окраски)
            double colorVar = (crackNoise.get(
                static_cast<double>(x) / (sc * 0.15),
                static_cast<double>(y) / (sc * 0.15)) - 0.5) * 0.04;
            r_stone = clamp(r_stone + colorVar, 0.0, 1.0);
            g_stone = clamp(g_stone + colorVar * 0.8, 0.0, 1.0);
            b_stone = clamp(b_stone + colorVar * 0.6, 0.0, 1.0);

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di] = static_cast<uint8_t>(r_stone * 255.0);
            maps.diffuse[di + 1] = static_cast<uint8_t>(g_stone * 255.0);
            maps.diffuse[di + 2] = static_cast<uint8_t>(b_stone * 255.0);

            // --- Height ---
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(heightVal * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Гладкие выступы слоёв: rough ≈ 0.55–0.70
            // Трещины и впадины: rough ≈ 0.85–0.98
            // Прожилки (кварц): чуть глаже ≈ 0.50–0.62
            double rough;
            if (inCrack) {
                rough = 0.82 + (1.0 - cracks / 0.28) * 0.16; // 0.82–0.98
            }
            else if (veinMask > 0.1) {
                rough = 0.50 + (1.0 - veinMask) * 0.18;       // 0.50–0.68
            }
            else {
                // Слои: чем выше гребень, тем глаже (полированность выступов)
                rough = 0.62 + (1.0 - stoneBase) * 0.25;
                // Небольшая микрошероховатость от зерна
                rough += crackMask * 0.06;
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
        }
    }

    // Нормали вычисляются через Sobel из heightmap
    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 2. GRASS — фотореалистичная трава с отдельными стеблями, почвой, объёмом
//
// Алгоритм:
//   a) Основа стеблей: GradientNoise с высокой анизотропией (Y-вытянутость)
//      создаёт вертикальные полосы — базу каждого стебля
//   b) Плотность стеблей: WorleyNoise F1 — кластеры/пучки травы
//   c) Почвенный слой: низкий FBM с тёплой палитрой, прячется между стеблями
//   d) Маска перехода: смесь почвы↔трава по высоте + шум кромки
//   e) Микродетали: тонкий шум поперёк стеблей для неровности листьев
// ============================================================================
void generateGrassPBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);

    // Основной шум структуры травы
    GradientNoise gNoise(p.seed);
    // Шум для почвенного слоя
    ValueNoise    soilNoise(p.seed + 17);
    // Клеточный шум для пучков (больше ячеек = мельче пучки)
    WorleyNoise   clumpWorley(p.seed + 41, 20);
    // Высокочастотный шум для индивидуальных стеблей
    ValueNoise    bladeNoise(p.seed + 53);

    const double sc = p.baseScale;

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double uvX = static_cast<double>(x) / TEX_WIDTH;
            double uvY = static_cast<double>(y) / TEX_HEIGHT;

            // --- a) Структура стеблей: анизотропный шум ---
            // Масштаб по X меньше чем по Y → вытянутые вертикальные паттерны
            double bx = static_cast<double>(x) / (sc * 0.25);  // узкие
            double by = static_cast<double>(y) / (sc * 1.5);   // длинные

            // Базовый шум направления стеблей
            double stemBase = gNoise.fbm(bx, by, p.octaves, p.persistence, p.lacunarity);

            // Высокочастотные стебли — отдельные «травинки»
            double bladeFine = bladeNoise.fbm(
                static_cast<double>(x) / (sc * 0.05),
                static_cast<double>(y) / (sc * 0.8),
                4, 0.4);

            // Рельеф лезвия: острые вершины, плавные основания
            // sin с небольшим периодом по X создаёт ряды стеблей
            double bladeRidge = std::pow(
                std::abs(std::sin((uvX * sc * 0.7 + stemBase * 0.5) * std::numbers::pi)),
                0.4);

            // --- b) Пучки травы через Worley ---
            // F1 = расстояние до центра пучка: ближе к 0 = середина пучка
            double clumpF1 = clumpWorley.f1(uvX, uvY);
            // Маска пучка: густая трава в центре, прогалины между пучками
            double clumpMask = clamp(1.0 - clumpF1 * 1.8, 0.0, 1.0);
            clumpMask = std::pow(clumpMask, 0.5); // смягчаем переход

            // --- c) Почва под травой ---
            double soilX = static_cast<double>(x) / (sc * 0.8);
            double soilY = static_cast<double>(y) / (sc * 0.8);
            double soil = soilNoise.fbm(soilX, soilY, 4, 0.55);

            // --- d) Комбинация: высота стебля ---
            // stemBase и bladeFine определяют высоту в данной точке
            double stemHeight = clamp(
                stemBase * 0.5 + bladeFine * 0.3 + bladeRidge * 0.2,
                0.0, 1.0);

            // Применяем маску пучка: в прогалинах трава пониже
            stemHeight = clamp(stemHeight * (0.35 + clumpMask * 0.65), 0.0, 1.0);

            // Карта высот: стебли высокие, почва между ними низкая
            double heightVal = stemHeight * 0.72 + soil * 0.08;
            heightVal = clamp(heightVal, 0.0, 1.0);

            // --- e) Маска трава/почва для цвета ---
            // Высокие точки → зелёная трава; низкие между пучками → коричневая почва
            double grassMask = clamp(stemHeight * 1.6 - 0.1, 0.0, 1.0);
            // Финальный шум на кромке для естественности
            double edgeVar = (bladeNoise.get(
                static_cast<double>(x) / (sc * 0.12),
                static_cast<double>(y) / (sc * 0.12)) - 0.5) * 0.3;
            grassMask = clamp(grassMask + edgeVar, 0.0, 1.0);

            // --- Diffuse: цветовая карта ---
            // Цвет почвы: тёплый коричневый с вариациями (камушки, песок)
            double soilR = 0.34 + soil * 0.12;
            double soilG = 0.23 + soil * 0.08;
            double soilB = 0.12 + soil * 0.04;

            // Цвет травы: живой зелёный, темнее у основания, светлее на вершинах
            // Вертикальный градиент: stеbleName по Y-UV — темнее снизу
            double grassShade = clamp(stemHeight * 0.7 + bladeFine * 0.3, 0.0, 1.0);

            // Нижняя часть стебля — тёмно-зелёная
            double grassR = lerp(0.09, 0.22, grassShade);
            double grassG = lerp(0.20, 0.48, grassShade);
            double grassB = lerp(0.04, 0.10, grassShade);

            // Добавляем вариации оттенка: часть стеблей желтоватая (сухие)
            double yellowVar = std::max(0.0, bladeFine - 0.65) * 1.5;
            grassR += yellowVar * 0.15;
            grassG += yellowVar * 0.08;

            // Тонкий цветовой шум
            double colorJitter = (soilNoise.get(
                static_cast<double>(x) / (sc * 0.3),
                static_cast<double>(y) / (sc * 0.3)) - 0.5) * 0.06;
            grassG += colorJitter;
            grassR += colorJitter * 0.5;

            // Финальное смешение почва ↔ трава
            double finalR = lerp(soilR, clamp(grassR, 0.0, 1.0), grassMask);
            double finalG = lerp(soilG, clamp(grassG, 0.0, 1.0), grassMask);
            double finalB = lerp(soilB, clamp(grassB, 0.0, 1.0), grassMask);

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di] = static_cast<uint8_t>(clamp(finalR * 255.0, 0.0, 255.0));
            maps.diffuse[di + 1] = static_cast<uint8_t>(clamp(finalG * 255.0, 0.0, 255.0));
            maps.diffuse[di + 2] = static_cast<uint8_t>(clamp(finalB * 255.0, 0.0, 255.0));

            // --- Height ---
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(heightVal * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Почва очень шероховатая (0.85–0.95)
            // Стебли немного глаже (восковой кутикулярный слой: 0.60–0.75)
            // Кончики листьев — наиболее гладкие (0.55)
            double rough;
            if (grassMask < 0.3) {
                // Зона почвы
                rough = 0.85 + soil * 0.10;
            }
            else {
                // Зона травы: глаже на высоких точках (tip), шершавее у основания
                rough = lerp(0.72, 0.58, grassShade);
                rough += (1.0 - clumpMask) * 0.12; // редкая трава грубее
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
        }
    }

    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 3. LAVA — лава с доменным искажением (оригинальная логика, адаптированная)
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
                r = lerp(20.0, 150.0, val / 0.4);
                g = lerp(0.0, 20.0, val / 0.4);
                b_col = 0;
            }
            else if (val < 0.7) {
                double t = (val - 0.4) / 0.3;
                r = lerp(150.0, 255.0, t);
                g = lerp(20.0, 120.0, t);
                b_col = 0;
            }
            else {
                double t = (val - 0.7) / 0.3;
                r = 255.0;
                g = lerp(120.0, 255.0, t);
                b_col = lerp(0.0, 255.0, t);
            }

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di] = static_cast<uint8_t>(clamp(r, 0.0, 255.0));
            maps.diffuse[di + 1] = static_cast<uint8_t>(clamp(g, 0.0, 255.0));
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
            }
            else if (val >= 0.4) {
                double t = (val - 0.4) / 0.3;
                rough = lerp(0.85, 0.20, t);
            }
            else {
                rough = lerp(1.00, 0.85, val / 0.4);
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
        }
    }

    computeNormalMap(maps.height, maps.normal, TEX_WIDTH, TEX_HEIGHT, p.normalStrength);
}

// ============================================================================
// 4. WOOD — берёзовый брус/строительный пиломатериал
//
// Алгоритм:
//   a) Волокна древесины: GradientNoise с сильной Y-анизотропией → продольные
//      полосы вдоль оси бруса; небольшой domain warp для естественного изгиба
//   b) Сучки: WorleyNoise F1 — точечные «центры» сучков, радиальные кольца
//      вокруг каждого через sin(1/F1) с нарастающей частотой
//   c) Годовые кольца: sin(radius) где radius = расстояние от центра бруса
//      (берёза — кольца широкие, берёзовое дерево светлое)
//   d) Смоляные потёки: ValueNoise turbulence — тёмные вертикальные потёки
//   e) Микрорельеф: высокочастотный ValueNoise поперёк волокон
// ============================================================================
void generateWoodPBR(PBRMaps& maps, const GeneratorParams& p)
{
    maps.allocate(TEX_WIDTH, TEX_HEIGHT);

    // Основной шум волокон
    GradientNoise gNoise(p.seed);
    // Шум для сучков
    WorleyNoise   knots(p.seed + 23, 6); // мало ячеек → крупные, редкие сучки
    // Шум смоляных потёков
    ValueNoise    resinNoise(p.seed + 61);
    // Микрорельефный шум
    ValueNoise    microNoise(p.seed + 79);

    const double sc = p.baseScale;

    for (int y = 0; y < TEX_HEIGHT; ++y) {
        for (int x = 0; x < TEX_WIDTH; ++x) {
            double uvX = static_cast<double>(x) / TEX_WIDTH;
            double uvY = static_cast<double>(y) / TEX_HEIGHT;

            // --- a) Волокна: анизотропный градиентный шум ---
            // По Y (вдоль бруса) — крупный масштаб; по X — мелкий (тонкие волокна)
            double fx = static_cast<double>(x) / (sc * 0.18);
            double fy = static_cast<double>(y) / (sc * 2.5);

            // Domain warp: небольшое «дыхание» волокон (натуральное искривление)
            double dwx = gNoise.fbm(fx * 0.3 + 0.5, fy * 0.3, 3, 0.5) * 0.4;
            double dwy = gNoise.fbm(fx * 0.3 + 8.0, fy * 0.3 + 8.0, 3, 0.5) * 0.15;

            double fiber = gNoise.fbm(fx + dwx, fy + dwy,
                p.octaves, p.persistence, p.lacunarity);

            // Периодические полосы волокон (разной ширины для реализма)
            double fiberRidge = std::pow(
                std::abs(std::sin((uvX * sc * 0.6 + fiber * 0.8) * std::numbers::pi)),
                0.35); // широкие светлые полосы с тонкими тёмными границами

            // --- b) Сучки: Worley + радиальные кольца ---
            auto [knotF1, knotF2] = knots.get(uvX, uvY);

            // Центр сучка: маска близости
            double knotMask = clamp(1.0 - knotF1 * 4.0, 0.0, 1.0);
            knotMask = std::pow(knotMask, 1.5);

            // Концентрические кольца вокруг сучка: sin(1/dist) с затуханием
            double knotRings = 0.0;
            if (knotF1 > 0.001) {
                // Частота колец нарастает ближе к центру (как реальные сучки)
                double ringFreq = 1.0 / (knotF1 * 6.0 + 0.05);
                knotRings = std::sin(ringFreq) * knotMask * 0.6;
                knotRings = clamp(knotRings, -0.3, 0.3);
            }

            // --- c) Годовые кольца (через X-координату — поперечный срез) ---
            // Берёза — светлая древесина: кольца слабо выражены, шире
            double ringOffset = gNoise.fbm(uvX * 0.5, uvY * 2.0, 3, 0.5) * 0.2;
            double rings = std::sin((uvX + ringOffset + fiber * 0.25) *
                std::numbers::pi * sc * 0.08);
            rings = rings * 0.5 + 0.5; // нормализуем в [0,1]
            rings = std::pow(rings, 1.3); // слегка заостряем

            // --- d) Смоляные потёки: вертикальный турбулентный шум ---
            double resinX = static_cast<double>(x) / (sc * 0.2);
            double resinY = static_cast<double>(y) / (sc * 0.6);
            double resin = resinNoise.turbulence(resinX, resinY, 3, 0.5);
            // Смоляные потёки тонкие и тёмные — только самые высокие значения
            double resinMask = std::pow(clamp(resin - 0.55, 0.0, 1.0) / 0.45, 2.5);

            // --- e) Микрорельеф поперёк волокон ---
            double microX = static_cast<double>(x) / (sc * 0.04);
            double microY = static_cast<double>(y) / (sc * 0.6);
            double micro = (microNoise.get(microX, microY) - 0.5) * 0.25;

            // --- Итоговая высота ---
            // Волокна образуют основной рельеф, сучки поднимают локально,
            // смола слегка углубляет (потёки), микро добавляет фактуру
            double heightVal = clamp(
                fiber * 0.35 + fiberRidge * 0.28 + rings * 0.18
                + knotRings * 0.10 + knotMask * 0.06
                - resinMask * 0.05 + micro,
                0.0, 1.0);

            // --- Diffuse: берёзовый цвет ---
            // Берёза: светло-кремовая/молочно-белая, желтоватая, годовые кольца чуть темнее
            // Основной цвет (кремово-белый берёзовый)
            double woodR = 0.90 - fiber * 0.08 + fiberRidge * 0.05;
            double woodG = 0.82 - fiber * 0.07 + fiberRidge * 0.04;
            double woodB = 0.68 - fiber * 0.06 + fiberRidge * 0.02;

            // Тёмные годовые кольца (летняя древесина чуть темнее)
            double ringDark = (1.0 - rings) * 0.10;
            woodR -= ringDark;
            woodG -= ringDark * 0.9;
            woodB -= ringDark * 0.7;

            // Сучок — тёмно-коричневый, насыщенный
            double knotBlend = knotMask * 0.8;
            woodR = lerp(woodR, 0.28, knotBlend);
            woodG = lerp(woodG, 0.16, knotBlend);
            woodB = lerp(woodB, 0.08, knotBlend);

            // Кольца сучка — тёмно-жёлто-коричневые
            double knotRingBlend = clamp(knotRings + 0.3, 0.0, 1.0) * knotMask * 0.5;
            woodR = lerp(woodR, 0.60, knotRingBlend);
            woodG = lerp(woodG, 0.35, knotRingBlend);
            woodB = lerp(woodB, 0.12, knotRingBlend);

            // Смоляные потёки: тёмно-янтарные
            woodR = lerp(woodR, 0.25, resinMask * 0.7);
            woodG = lerp(woodG, 0.14, resinMask * 0.7);
            woodB = lerp(woodB, 0.05, resinMask * 0.7);

            // Тонкий цветовой шум для натуральности
            double cvar = (microNoise.get(
                static_cast<double>(x) / (sc * 0.5),
                static_cast<double>(y) / (sc * 0.5)) - 0.5) * 0.03;
            woodR = clamp(woodR + cvar, 0.0, 1.0);
            woodG = clamp(woodG + cvar * 0.8, 0.0, 1.0);
            woodB = clamp(woodB + cvar * 0.5, 0.0, 1.0);

            size_t di = static_cast<size_t>(y * TEX_WIDTH + x) * 3;
            maps.diffuse[di] = static_cast<uint8_t>(woodR * 255.0);
            maps.diffuse[di + 1] = static_cast<uint8_t>(woodG * 255.0);
            maps.diffuse[di + 2] = static_cast<uint8_t>(woodB * 255.0);

            // --- Height ---
            size_t hi = static_cast<size_t>(y * TEX_WIDTH + x);
            maps.height[hi] = static_cast<uint8_t>(clamp(heightVal * 255.0, 0.0, 255.0));

            // --- Roughness ---
            // Гладкие волокна: 0.30–0.55 (берёза поддаётся полировке)
            // Сучки: 0.70–0.85 (шершавые, пористые)
            // Смоляные потёки: 0.15–0.30 (смола = гладкая, глянцевая)
            // Поперечный срез (grain): чуть грубее волокон
            double rough;
            if (knotMask > 0.25) {
                rough = lerp(0.55, 0.82, knotMask);
            }
            else if (resinMask > 0.1) {
                rough = lerp(0.45, 0.18, resinMask);
            }
            else {
                // Волокна: гребни волокон глаже, границы грубее
                rough = 0.32 + (1.0 - fiberRidge) * 0.22;
                rough += std::abs(micro) * 0.15;
            }
            maps.roughness[hi] = static_cast<uint8_t>(clamp(rough * 255.0, 0.0, 255.0));
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
    case 2: generateLavaPBR(maps, params); break;
    case 3: generateWoodPBR(maps, params); break;
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
            uint8_t v = data[static_cast<size_t>(i)];
            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
            out.put(static_cast<char>(v));
        }
    }
    else {
        out << "P6\n" << w << " " << h << "\n255\n";
        out.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }
}

void savePBRtoPPM(const std::string& prefix, const PBRMaps& maps)
{
    writeP6(prefix + "_diffuse.ppm", maps.diffuse, maps.width, maps.height_px, false);
    writeP6(prefix + "_height.ppm", maps.height, maps.width, maps.height_px, true);
    writeP6(prefix + "_normal.ppm", maps.normal, maps.width, maps.height_px, false);
    writeP6(prefix + "_roughness.ppm", maps.roughness, maps.width, maps.height_px, true);
}