// lab_math.h — 物理の試作(tools/physics_lab、T-0016)の double の小さなベクトル・行列。
// 試作はアルゴリズムとパラメータを決めるための道具で、決定性は要らない(ツールなので浮動小数点を使ってよい。CLAUDE.md 原則 2)。
// 整数のソルバーは shaders/common/physics_*.hlsli に別に書く。
#pragma once

#include <array>
#include <cmath>

namespace bicameral::lab {

    // --- 3 次元のベクトル ---
    struct Vec3 {
        double x = 0;
        double y = 0;
        double z = 0;

        double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
        double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    };

    inline Vec3 operator+(Vec3 a, Vec3 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }

    inline Vec3 operator-(Vec3 a, Vec3 b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline Vec3 operator-(Vec3 a) {
        return {-a.x, -a.y, -a.z};
    }

    inline Vec3 operator*(Vec3 a, double s) {
        return {a.x * s, a.y * s, a.z * s};
    }

    inline Vec3 operator*(double s, Vec3 a) {
        return a * s;
    }

    inline Vec3& operator+=(Vec3& a, Vec3 b) {
        return a = a + b;
    }

    inline Vec3& operator-=(Vec3& a, Vec3 b) {
        return a = a - b;
    }

    inline double Dot(Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vec3 Cross(Vec3 a, Vec3 b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    inline double Length(Vec3 a) {
        return std::sqrt(Dot(a, a));
    }

    // --- 3×3 の行列(行優先)---
    struct Mat3 {
        std::array<Vec3, 3> row{};

        [[nodiscard]] Vec3 Column(int j) const { return {row[0][j], row[1][j], row[2][j]}; }
    };

    inline Vec3 operator*(const Mat3& m, Vec3 v) {
        return {Dot(m.row[0], v), Dot(m.row[1], v), Dot(m.row[2], v)};
    }

    inline Mat3 Transpose(const Mat3& m) {
        return {{m.Column(0), m.Column(1), m.Column(2)}};
    }

    inline Mat3 operator*(const Mat3& a, const Mat3& b) {
        Mat3 result;
        for (int i = 0; i < 3; ++i)
            result.row[i] = {Dot(a.row[i], b.Column(0)), Dot(a.row[i], b.Column(1)), Dot(a.row[i], b.Column(2))};

        return result;
    }

    // --- 四元数 (x, y, z, w) ---
    struct Quat {
        double x = 0;
        double y = 0;
        double z = 0;
        double w = 1;
    };

    inline Quat operator*(Quat a, Quat b) {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    inline Quat Normalize(Quat q) {
        const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return {q.x / length, q.y / length, q.z / length, q.w / length};
    }

    // 回転ベクトル(軸 × 角度)を q に積む: exp(θ/2) × q
    inline Quat Integrate(Quat q, Vec3 rotationVector) {
        const double angle = Length(rotationVector);
        if (angle < 1e-12)
            return q;

        const Vec3 axis = rotationVector * (1.0 / angle);
        const double s = std::sin(angle / 2);
        const Quat delta{axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2)};

        return Normalize(delta * q);
    }

    inline Mat3 RotationMatrix(Quat q) {
        const double xx = q.x * q.x;
        const double yy = q.y * q.y;
        const double zz = q.z * q.z;
        const double xy = q.x * q.y;
        const double xz = q.x * q.z;
        const double yz = q.y * q.z;
        const double wx = q.w * q.x;
        const double wy = q.w * q.y;
        const double wz = q.w * q.z;

        return {{
            Vec3{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
            Vec3{2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
            Vec3{2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)},
        }};
    }

    // --- 6×6 の対称正定値の連立方程式(Cholesky)---
    using Vec6 = std::array<double, 6>;
    using Mat6 = std::array<std::array<double, 6>, 6>;

    // a x = b を解く。a は対称正定値(壊さずに写して分解する)
    inline Vec6 SolveSymmetric6(Mat6 a, Vec6 b) {
        for (int j = 0; j < 6; ++j) {
            double diagonal = a[j][j];
            for (int k = 0; k < j; ++k)
                diagonal -= a[j][k] * a[j][k];

            a[j][j] = std::sqrt(diagonal);
            for (int i = j + 1; i < 6; ++i) {
                double value = a[i][j];
                for (int k = 0; k < j; ++k)
                    value -= a[i][k] * a[j][k];

                a[i][j] = value / a[j][j];
            }
        }

        for (int i = 0; i < 6; ++i) {
            for (int k = 0; k < i; ++k)
                b[i] -= a[i][k] * b[k];

            b[i] /= a[i][i];
        }

        for (int i = 5; i >= 0; --i) {
            for (int k = i + 1; k < 6; ++k)
                b[i] -= a[k][i] * b[k];

            b[i] /= a[i][i];
        }

        return b;
    }

}  // namespace bicameral::lab
