#pragma once
// ============================================================================
// NoiseCore.h — Математическое ядро шума (оригинальный код, без изменений)
// Содержит: lerp, cos_interpolate, clamp, ValueNoise (Value Noise + FBM)
// ============================================================================

#include <vector>
#include <cmath>
#include <numbers>
#include <random>

constexpr int TEX_WIDTH       = 512;
constexpr int TEX_HEIGHT      = 512;
constexpr int NOISE_GRID_SIZE = 256;

// --- Линейная интерполяция ---
constexpr double lerp(double a, double b, double t) noexcept {
    return a + t * (b - a);
}

// --- Косинусная интерполяция (гладкая кривая для шума) ---
inline double cos_interpolate(double a, double b, double t) noexcept {
    const double ft = t * std::numbers::pi;
    const double f  = (1.0 - std::cos(ft)) * 0.5;
    return a * (1.0 - f) + b * f;
}

// --- Зажим значения ---
constexpr double clamp(double v, double lo, double hi) noexcept {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

// ============================================================================
// Value Noise — класс генератора шума на основе таблицы случайных значений
// ============================================================================
class ValueNoise {
private:
    std::vector<double> grid;

public:
    explicit ValueNoise(unsigned int seed = 42)
        : grid(NOISE_GRID_SIZE * NOISE_GRID_SIZE)
    {
        std::mt19937 gen(seed);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        for (auto& val : grid) val = dist(gen);
    }

    // Получить значение шума в точке (x, y) с косинусной интерполяцией
    [[nodiscard]] double get(double x, double y) const noexcept {
        int xi = static_cast<int>(std::floor(x));
        int yi = static_cast<int>(std::floor(y));
        double tx = x - xi;
        double ty = y - yi;

        int rx0 = xi        & (NOISE_GRID_SIZE - 1);
        int rx1 = (xi + 1)  & (NOISE_GRID_SIZE - 1);
        int ry0 = yi        & (NOISE_GRID_SIZE - 1);
        int ry1 = (yi + 1)  & (NOISE_GRID_SIZE - 1);

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
        double total    = 0.0;
        double frequency = 1.0;
        double amplitude = 1.0;
        double maxValue  = 0.0;

        for (int i = 0; i < octaves; ++i) {
            total    += get(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude  *= persistence;
            frequency  *= lacunarity;
        }
        return total / maxValue;
    }
};
