#pragma once

#include "vr/core/frame_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace OutRunVR::Core
{
    struct Matrix4
    {
        float m[4][4]{};

        float* operator[](std::size_t row) noexcept { return m[row]; }
        const float* operator[](std::size_t row) const noexcept { return m[row]; }
    };

    inline Matrix4 IdentityMatrix() noexcept
    {
        Matrix4 out{};
        out[0][0] = out[1][1] = out[2][2] = out[3][3] = 1.0f;
        return out;
    }

    inline bool MatrixFinite(const Matrix4& matrix) noexcept
    {
        for (const auto& row : matrix.m)
            for (const float value : row)
                if (!std::isfinite(value))
                    return false;
        return true;
    }

    inline Matrix4 Multiply(const Matrix4& a, const Matrix4& b) noexcept
    {
        Matrix4 out{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k)
                    out[r][c] += a[r][k] * b[k][c];
        return out;
    }

    inline Matrix4 Transpose(const Matrix4& in) noexcept
    {
        Matrix4 out{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r][c] = in[c][r];
        return out;
    }

    inline bool Invert(const Matrix4& in, Matrix4& out) noexcept
    {
        float a[4][8]{};
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
                a[r][c] = in[r][c];
            a[r][4 + r] = 1.0f;
        }

        for (int col = 0; col < 4; ++col)
        {
            int pivot = col;
            float pivotAbs = std::fabs(a[pivot][col]);
            for (int r = col + 1; r < 4; ++r)
            {
                const float candidate = std::fabs(a[r][col]);
                if (candidate > pivotAbs)
                {
                    pivot = r;
                    pivotAbs = candidate;
                }
            }
            if (!std::isfinite(pivotAbs) || pivotAbs < 1.0e-8f)
                return false;
            if (pivot != col)
                for (int c = 0; c < 8; ++c)
                    std::swap(a[pivot][c], a[col][c]);

            const float divisor = a[col][col];
            for (int c = 0; c < 8; ++c)
                a[col][c] /= divisor;

            for (int r = 0; r < 4; ++r)
            {
                if (r == col)
                    continue;
                const float factor = a[r][col];
                for (int c = 0; c < 8; ++c)
                    a[r][c] -= factor * a[col][c];
            }
        }

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r][c] = a[r][c + 4];
        return MatrixFinite(out);
    }

    inline Matrix4 RigidTransform(const Quaternion& qIn, const Vector3& position,
        float positionScale) noexcept
    {
        float x = qIn.x;
        float y = qIn.y;
        float z = qIn.z;
        float w = qIn.w;
        const float lenSq = x*x + y*y + z*z + w*w;
        if (!std::isfinite(lenSq) || lenSq <= 1.0e-12f)
            return IdentityMatrix();

        const float inv = 1.0f / std::sqrt(lenSq);
        x *= inv; y *= inv; z *= inv; w *= inv;

        const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z;
        const float xw=x*w, yw=y*w, zw=z*w;
        Matrix4 out = IdentityMatrix();
        out[0][0]=1-2*(yy+zz); out[0][1]=2*(xy+zw); out[0][2]=2*(xz-yw);
        out[1][0]=2*(xy-zw); out[1][1]=1-2*(xx+zz); out[1][2]=2*(yz+xw);
        out[2][0]=2*(xz+yw); out[2][1]=2*(yz-xw); out[2][2]=1-2*(xx+yy);
        out[3][0]=position.x*positionScale;
        out[3][1]=position.y*positionScale;
        out[3][2]=position.z*positionScale;
        return out;
    }

    inline Matrix4 InverseRigid(const Matrix4& m) noexcept
    {
        Matrix4 out = IdentityMatrix();
        out[0][0]=m[0][0]; out[0][1]=m[1][0]; out[0][2]=m[2][0];
        out[1][0]=m[0][1]; out[1][1]=m[1][1]; out[1][2]=m[2][1];
        out[2][0]=m[0][2]; out[2][1]=m[1][2]; out[2][2]=m[2][2];
        out[3][0]=-(m[3][0]*out[0][0] + m[3][1]*out[1][0] + m[3][2]*out[2][0]);
        out[3][1]=-(m[3][0]*out[0][1] + m[3][1]*out[1][1] + m[3][2]*out[2][1]);
        out[3][2]=-(m[3][0]*out[0][2] + m[3][1]*out[1][2] + m[3][2]*out[2][2]);
        return out;
    }

    inline bool ProjectionFromOpenXrFov(const Matrix4& base, const Fov& fov,
        Matrix4& out) noexcept
    {
        const float tanLeft = std::tan(fov.angleLeft);
        const float tanRight = std::tan(fov.angleRight);
        const float tanUp = std::tan(fov.angleUp);
        const float tanDown = std::tan(fov.angleDown);
        const float width = tanRight - tanLeft;
        const float height = tanUp - tanDown;
        if (!std::isfinite(width) || !std::isfinite(height) ||
            std::fabs(width) < 1.0e-6f || std::fabs(height) < 1.0e-6f)
            return false;

        out = {};
        out[0][0] = 2.0f / width;
        out[1][1] = 2.0f / height;
        out[2][0] = (tanRight + tanLeft) / width;
        out[2][1] = (tanUp + tanDown) / height;
        out[2][2] = base[2][2];
        out[2][3] = base[2][3];
        out[3][2] = base[3][2];
        out[3][3] = base[3][3];
        return MatrixFinite(out);
    }
}
