#pragma once
// ============================================================================
// NoiseCore.h — Математическое ядро шума
// Содержит: lerp, cos_interpolate, smooth_step, clamp,
//           ValueNoise (Value Noise + FBM),
//           WorleyNoise (Cellular/F1/F2 шум),
//           GradientNoise (Перлин-подобный градиентный шум),
//           вспомогательные комбинированные функции.
// ============================================================================

#include <vector>
#include <cmath>
#include <numbers>
#include <random>
#include <array>
#include <algorithm>

// Разрешение текстур увеличено до 1024x1024
constexpr int TEX_WIDTH = 1024;
constexpr int TEX_HEIGHT = 1024;
constexpr int NOISE_GRID_SIZE = 256; // размер таблицы шума (должен быть степенью 2)

// --- Линейная интерполяция ---
constexpr double lerp(double a, double b, double t) noexcept {
    return a + t * (b - a);
}

// --- Косинусная интерполяция (гладкая кривая для шума) ---
inline double cos_interpolate(double a, double b, double t) noexcept {
    const double ft = t * std::numbers::pi;
    const double f = (1.0 - std::cos(ft)) * 0.5;
    return a * (1.0 - f) + b * f;
}

// --- Smoothstep (5-я степень Кена Перлина) для минимизации артефактов ---
constexpr double smooth_step(double t) noexcept {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// --- Зажим значения ---
constexpr double clamp(double v, double lo, double hi) noexcept {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

// --- Модуль (fmod) с гарантированно положительным результатом ---
inline double fmod_pos(double x, double m) noexcept {
    double r = std::fmod(x, m);
    return r < 0.0 ? r + m : r;
}

// ============================================================================
// Value Noise — класс генератора шума на основе таблицы случайных значений
// ============================================================================
class ValueNoise {
private:
    std::vector<double> grid;

public:
    explicit ValueNoise(unsigned int seed = 42)
        : grid(NOISE_GRID_SIZE* NOISE_GRID_SIZE)
    {
        std::mt19937 gen(seed);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        for (auto& val : grid) val = dist(gen);
    }

    // Получить значение шума в точке (x, y) с косинусной интерполяцией
    [[nodiscard]] double get(double x, double y) const noexcept {
        int xi = static_cast<int>(std::floor(x));
        int yi = static_cast<int>(std::floor(y));
        double tx = x - static_cast<double>(xi);
        double ty = y - static_cast<double>(yi);

        int rx0 = xi & (NOISE_GRID_SIZE - 1);
        int rx1 = (xi + 1) & (NOISE_GRID_SIZE - 1);
        int ry0 = yi & (NOISE_GRID_SIZE - 1);
        int ry1 = (yi + 1) & (NOISE_GRID_SIZE - 1);

        double c00 = grid[ry0 * NOISE_GRID_SIZE + rx0];
        double c10 = grid[ry0 * NOISE_GRID_SIZE + rx1];
        double c01 = grid[ry1 * NOISE_GRID_SIZE + rx0];
        double c11 = grid[ry1 * NOISE_GRID_SIZE + rx1];

        double nx0 = cos_interpolate(c00, c10, tx);
        double nx1 = cos_interpolate(c01, c11, tx);
        return cos_interpolate(nx0, nx1, ty);
    }

    // Fractal Brownian Motion (FBM) — многооктавный шум
    [[nodiscard]] double fbm(double x, double y,
        int octaves, double persistence,
        double lacunarity = 2.0) const noexcept
    {
        double total = 0.0;
        double frequency = 1.0;
        double amplitude = 1.0;
        double maxValue = 0.0;

        for (int i = 0; i < octaves; ++i) {
            total += get(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }

    // Turbulence FBM — абсолютное значение октав для «турбулентного» вида
    [[nodiscard]] double turbulence(double x, double y,
        int octaves, double persistence,
        double lacunarity = 2.0) const noexcept
    {
        double total = 0.0;
        double frequency = 1.0;
        double amplitude = 1.0;
        double maxValue = 0.0;

        for (int i = 0; i < octaves; ++i) {
            // Центрируем вокруг 0 и берём абсолютное значение — создаёт «острые» хребты
            double v = std::abs(get(x * frequency, y * frequency) * 2.0 - 1.0);
            total += v * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }
};

// ============================================================================
// GradientNoise — градиентный шум (Перлин-подобный, без lookup table)
// Использует хэш-функцию для случайного выбора градиента из 8 направлений.
// Более «органичный» вид по сравнению с Value Noise.
// ============================================================================
class GradientNoise {
private:
    // Таблица перестановок (256 элементов, дублированная до 512)
    std::array<int, 512> perm;

    // Градиенты 2D — 8 единичных направлений
    static constexpr std::array<std::array<double, 2>, 8> GRAD = { {
        { 1.0,  0.0}, {-1.0,  0.0},
        { 0.0,  1.0}, { 0.0, -1.0},
        { 0.7071,  0.7071}, {-0.7071,  0.7071},
        { 0.7071, -0.7071}, {-0.7071, -0.7071}
    } };

    // Скалярное произведение градиента с вектором смещения
    [[nodiscard]] double dot_grad(int hash, double dx, double dy) const noexcept {
        const auto& g = GRAD[hash & 7];
        return g[0] * dx + g[1] * dy;
    }

public:
    explicit GradientNoise(unsigned int seed = 42) {
        // Заполняем 0..255 и перемешиваем по seed
        for (int i = 0; i < 256; ++i) perm[i] = i;
        std::mt19937 gen(seed);
        std::shuffle(perm.begin(), perm.begin() + 256, gen);
        // Дублируем для удобства индексирования без маскирования
        for (int i = 0; i < 256; ++i) perm[256 + i] = perm[i];
    }

    [[nodiscard]] double get(double x, double y) const noexcept {
        int xi = static_cast<int>(std::floor(x));
        int yi = static_cast<int>(std::floor(y));
        double tx = x - static_cast<double>(xi);
        double ty = y - static_cast<double>(yi);

        // Маскируем координаты сетки в диапазон [0,255]
        int x0 = xi & 255, x1 = (xi + 1) & 255;
        int y0 = yi & 255, y1 = (yi + 1) & 255;

        // Вычисляем хэши для 4 узлов
        int h00 = perm[perm[x0] + y0];
        int h10 = perm[perm[x1] + y0];
        int h01 = perm[perm[x0] + y1];
        int h11 = perm[perm[x1] + y1];

        // Dot products
        double n00 = dot_grad(h00, tx, ty);
        double n10 = dot_grad(h10, tx - 1.0, ty);
        double n01 = dot_grad(h01, tx, ty - 1.0);
        double n11 = dot_grad(h11, tx - 1.0, ty - 1.0);

        // Интерполяция через smooth_step
        double u = smooth_step(tx);
        double v = smooth_step(ty);
        double nx0 = lerp(n00, n10, u);
        double nx1 = lerp(n01, n11, u);
        // Приводим [-0.7..0.7] → [0..1]
        return clamp((lerp(nx0, nx1, v) + 0.7) / 1.4, 0.0, 1.0);
    }

    [[nodiscard]] double fbm(double x, double y,
        int octaves, double persistence,
        double lacunarity = 2.0) const noexcept
    {
        double total = 0.0;
        double frequency = 1.0;
        double amplitude = 1.0;
        double maxValue = 0.0;

        for (int i = 0; i < octaves; ++i) {
            total += get(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }
};

// ============================================================================
// WorleyNoise — клеточный / Voronoi шум (F1, F2-F1)
// Возвращает расстояние до ближайшей (F1) и второй ближайшей (F2) точки.
// Используется для трещин, прожилок и органических паттернов.
// ============================================================================
class WorleyNoise {
private:
    // Случайные смещения точек внутри каждой ячейки (2 числа на ячейку)
    std::vector<double> pts; // pts[cell*2], pts[cell*2+1] — координаты точки
    int gridSize;            // количество ячеек по каждой оси

public:
    // gridSize — количество ячеек (например 16 → 16x16 ячеек = 256 точек)
    explicit WorleyNoise(unsigned int seed = 42, int gridSz = 16)
        : gridSize(gridSz)
    {
        pts.resize(static_cast<size_t>(gridSz * gridSz * 2));
        std::mt19937 gen(seed + 9999); // смещаем seed чтобы не совпадало с ValueNoise
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        for (auto& v : pts) v = dist(gen);
    }

    // Возвращает {F1, F2} — расстояния до ближайшей и второй ближайшей точки,
    // нормализованные в [0,1] для данной частоты сетки.
    [[nodiscard]] std::pair<double, double> get(double x, double y) const noexcept {
        // Масштабируем в пространство ячеек
        double cx = x * gridSize;
        double cy = y * gridSize;

        int cellX = static_cast<int>(std::floor(cx));
        int cellY = static_cast<int>(std::floor(cy));

        double f1 = 1e9, f2 = 1e9;

        // Проверяем 3x3 соседних ячейки для полноты
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = ((cellX + dx) % gridSize + gridSize) % gridSize;
                int ny = ((cellY + dy) % gridSize + gridSize) % gridSize;
                int idx = (ny * gridSize + nx) * 2;

                // Координата точки в пространстве ячеек
                double px = (cellX + dx) + pts[idx];
                double py = (cellY + dy) + pts[idx + 1];

                double dist2 = (cx - px) * (cx - px) + (cy - py) * (cy - py);

                if (dist2 < f1) { f2 = f1; f1 = dist2; }
                else if (dist2 < f2) { f2 = dist2; }
            }
        }

        // Максимальное расстояние для нормализации ≈ 0.7 (диагональ полуячейки)
        const double maxDist = 1.2;
        return {
            clamp(std::sqrt(f1) / maxDist, 0.0, 1.0),
            clamp(std::sqrt(f2) / maxDist, 0.0, 1.0)
        };
    }

    // Удобный метод: только F1
    [[nodiscard]] double f1(double x, double y) const noexcept {
        return get(x, y).first;
    }

    // Удобный метод: F2-F1 — паттерн «краёв ячеек» (прожилки, границы)
    [[nodiscard]] double f2_minus_f1(double x, double y) const noexcept {
        auto [d1, d2] = get(x, y);
        return clamp(d2 - d1, 0.0, 1.0);
    }
};