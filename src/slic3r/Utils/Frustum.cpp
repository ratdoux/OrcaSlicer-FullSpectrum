#include "Frustum.hpp"

namespace Slic3r
{

float Plane::PointToPlaneDistance(const Vec3f& p) const
{
    return m_normal.dot(p) + m_distance;
}

bool Plane::PointInFrontOfPlane(const Vec3f& p) const
{
    IntersectionResult res = Intersect(p);
    return res != IntersectionResult::Outside;
}

void Plane::Normalize()
{
    double magnitude = m_normal.norm();
    if (magnitude > EPSILON) 
    {
        m_normal   /= magnitude;
        m_distance /= magnitude;
    }
}

IntersectionResult Plane::Intersect(const BoundingBoxf3& box) const
{
    Vec3f boxMin = box.min.cast<float>();
    Vec3f boxMax = box.max.cast<float>();
    const Vec3f  aabbCenter   = (boxMin + boxMax) * 0.5f;
    const Vec3f  aabbHalfSize = boxMax - aabbCenter;
    const float projRadius  = aabbHalfSize.x() * std::abs(m_normal.x())
                             + aabbHalfSize.y() * std::abs(m_normal.y())
                             + aabbHalfSize.z() * std::abs(m_normal.z());
    const float aabbCenter2Plane = aabbCenter.dot(m_normal) + m_distance;

    if (std::abs(aabbCenter2Plane) <= projRadius) 
    {
        return IntersectionResult::Intersect;
    }
    if (aabbCenter2Plane > projRadius) 
    {
        return IntersectionResult::Inside;
    }
    return IntersectionResult::Outside;
}

IntersectionResult Plane::Intersect(const Plane& other) const
{
    const Vec3f&  n1 = m_normal;
    const Vec3f&  n2 = other.GetNormal();
    const float dot = n1.dot(n2);
    const float norm_prod = n1.norm() * n2.norm();
    const bool parallel = std::abs(dot - norm_prod) < EPSILON || std::abs(dot + norm_prod) < EPSILON;

    if (!parallel) 
    {
        return IntersectionResult::Intersect;
    }

    float scale = 1.0f;
    if (n1.x() != 0.0f) 
    {
        scale = n2.x() / n1.x();
    } else if (n1.y() != 0.0f) 
    {
        scale = n2.y() / n1.y();
    } else if (n1.z() != 0.0f) 
    {
        scale = n2.z() / n1.z();
    }

    if (std::abs(m_distance * scale - other.GetDistance()) < EPSILON) 
    {
        return IntersectionResult::Inside;
    }
    return IntersectionResult::Outside;
}

IntersectionResult Plane::Intersect(const Vec3f& p) const
{
    const float dist = PointToPlaneDistance(p);
    if (dist > 0) 
    {
        return IntersectionResult::Inside;
    }
    if (dist < 0) 
    {
        return IntersectionResult::Outside;
    }
    return IntersectionResult::Intersect;
}

IntersectionResult Plane::Intersect(const Vec3f& p0, const Vec3f& p1) const
{
    const bool p0Inside = PointInFrontOfPlane(p0);
    const bool p1Inside = PointInFrontOfPlane(p1);

    if (p0Inside && p1Inside) 
    {
        return IntersectionResult::Inside;
    }
    if (!p0Inside && !p1Inside) 
    {
        return IntersectionResult::Outside;
    }
    return IntersectionResult::Intersect;
}

IntersectionResult Plane::Intersect(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2) const
{
    const bool p0Inside = PointInFrontOfPlane(p0);
    const bool p1Inside = PointInFrontOfPlane(p1);
    const bool p2Inside = PointInFrontOfPlane(p2);

    if (p0Inside && p1Inside && p2Inside) 
    {
        return IntersectionResult::Inside;
    }
    if (!p0Inside && !p1Inside && !p2Inside) 
    {
        return IntersectionResult::Outside;
    }
    return IntersectionResult::Intersect;
}

bool Frustum::Intersects(const BoundingBoxf3& box) const
{
    for (const Plane& plane : planes) 
    {
        if (plane.Intersect(box) == IntersectionResult::Outside) 
        {
            return false;
        }
    }
    return true;
}

bool Frustum::Intersects(const Vec3f& p0) const
{
    for (const Plane& plane : planes) 
    {
        if (plane.Intersect(p0) == IntersectionResult::Outside) 
        {
            return false;
        }
    }    
    return true;
}

bool Frustum::Intersects(const Vec3f& p0, const Vec3f& p1) const
{
    for (const Plane& plane : planes) 
    {
        if (plane.Intersect(p0, p1) == IntersectionResult::Outside) 
        {
            return false;
        }
    }    
    return true;
}

bool Frustum::Intersects(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2) const
{
    for (const Plane& plane : planes) 
    {
        if (plane.Intersect(p0, p1, p2) == IntersectionResult::Outside) 
        {
            return false;
        }
    }    
    return true;
}

} // namespace Slic3r
