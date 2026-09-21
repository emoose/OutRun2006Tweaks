#include "vr/core/matrix.hpp"

#include <cmath>

namespace
{
    bool Near(float a, float b, float epsilon = 1.0e-5f)
    {
        return std::fabs(a - b) <= epsilon;
    }
}

int main()
{
    using namespace OutRunVR::Core;

    Matrix4 base{};
    base[0][0] = 1.3f;
    base[1][1] = 1.7f;
    base[2][2] = -1.001f;
    base[2][3] = -1.0f;
    base[3][2] = -0.1f;

    Fov symmetric{-0.7f, 0.7f, 0.6f, -0.6f};
    Matrix4 projection{};
    if (!ProjectionFromOpenXrFov(base, symmetric, projection))
        return 1;
    if (!Near(projection[2][0], 0.0f) || !Near(projection[2][1], 0.0f))
        return 2;
    if (!Near(projection[2][2], base[2][2]) ||
        !Near(projection[2][3], base[2][3]) ||
        !Near(projection[3][2], base[3][2]))
        return 3;

    Quaternion q{};
    Vector3 p{1.0f, 2.0f, -3.0f};
    const Matrix4 rigid = RigidTransform(q, p, 2.0f);
    if (!Near(rigid[3][0], 2.0f) || !Near(rigid[3][1], 4.0f) || !Near(rigid[3][2], -6.0f))
        return 4;

    const Matrix4 inverseRigid = InverseRigid(rigid);
    const Matrix4 identity = Multiply(rigid, inverseRigid);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!Near(identity[r][c], r == c ? 1.0f : 0.0f))
                return 5;

    Matrix4 inverse{};
    if (!Invert(rigid, inverse))
        return 6;
    const Matrix4 identity2 = Multiply(rigid, inverse);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!Near(identity2[r][c], r == c ? 1.0f : 0.0f))
                return 7;

    Fov invalid{};
    if (ProjectionFromOpenXrFov(base, invalid, projection))
        return 8;

    return 0;
}
