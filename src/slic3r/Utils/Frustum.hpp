#pragma once

#include "BoundingBox.hpp"

namespace Slic3r
{
    enum class IntersectionResult
    {
        Outside,
        Intersect,
        Inside
    };

    // Plane defined by ax + by + cz + d = 0, where (a,b,c) is normal and d is distance
    class Plane
    {
    public:
        Plane() = default;
        ~Plane() = default;

        Plane(const Vec4f& planeEquation)
            : m_normal(planeEquation.head<3>())
            , m_distance(planeEquation.w())
        {}

        void SetEquation(const Vec4f& planeEquation)
        {
            SetEquation(planeEquation.head<3>(), planeEquation.w());
        }

        void SetEquation(float a, float b, float c, float d)
        {
            SetEquation(Vec3f(a, b, c), d); 
        }

        void SetEquation(const Vec3f& normal, float distance)
        {
            m_normal   = normal;
            m_distance = distance;
        }

        void Normalize();

        const Vec3f& GetNormal() const { return m_normal; }
        float GetDistance() const { return m_distance; }

        float PointToPlaneDistance(const Vec3f& p) const;
        bool  PointInFrontOfPlane(const Vec3f& p) const;

        IntersectionResult Intersect(const BoundingBoxf3& box) const;
        IntersectionResult Intersect(const Plane& other) const;
        IntersectionResult Intersect(const Vec3f& p) const;
        IntersectionResult Intersect(const Vec3f& p0, const Vec3f& p1) const;
        IntersectionResult Intersect(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2) const;

    private:
        Vec3f m_normal   = Vec3f::Zero();
        float m_distance = 0.0;
    };

    class Frustum
    {
    public:
        Frustum()  = default;
        ~Frustum() = default;

        bool Intersects(const BoundingBoxf3& box) const;
        bool Intersects(const Vec3f& p0) const;
        bool Intersects(const Vec3f& p0, const Vec3f& p1) const;
        bool Intersects(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2) const;

    public:
        Plane planes[6];
    };
} // namespace Slic3r
