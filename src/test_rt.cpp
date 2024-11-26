
#include "gc.h"
#include "rc.h"
#include <span>
#include <cmath>
struct Vec3 {
    double x, y, z;
    Vec3(double x, double y, double z) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3 &v) const {
        return Vec3(x + v.x, y + v.y, z + v.z);
    }
    Vec3 operator-(const Vec3 &v) const {
        return Vec3(x - v.x, y - v.y, z - v.z);
    }

    Vec3 operator*(double scalar) const {
        return Vec3(x * scalar, y * scalar, z * scalar);
    }

    Vec3 operator/(double scalar) const {
        return Vec3(x / scalar, y / scalar, z / scalar);
    }

    bool operator==(const Vec3 &v) const {
        return x == v.x && y == v.y && z == v.z;
    }

    bool operator!=(const Vec3 &v) const {
        return !(*this == v);
    }

    double dot(const Vec3 &v) const {
        return x * v.x + y * v.y + z * v.z;
    }
    Vec3 cross(const Vec3 &v) const {
        return Vec3(
            y * v.z - z * v.y,
            z * v.x - x * v.z,
            x * v.y - y * v.x);
    }
    double max_element() const {
        return std::max<double>({x, y, z});
    }
};

struct Ray {
    Vec3 origin, direction;
    Ray(const Vec3 &origin, const Vec3 &direction) : origin(origin), direction(direction) {}
};

struct AABB {
    Vec3 min, max;
    AABB(const Vec3 &min, const Vec3 &max) : min(min), max(max) {}
    Vec3 extent() const {
        return max - min;
    }
    Vec3 center() const {
        return (min + max) / 2;
    }
};

struct Material {
    Vec3 color;
};

struct Sphere {
    Vec3 center;
    double radius;
    Sphere(const Vec3 &center, double radius) : center(center), radius(radius) {}
    AABB aabb() const {
        return AABB(center - Vec3(radius, radius, radius), center + Vec3(radius, radius, radius));
    }
    std::optional<double> intersect(const Ray &ray) const {
        Vec3 oc = ray.origin - center;
        double a = ray.direction.dot(ray.direction);
        double b = 2.0 * oc.dot(ray.direction);
        double c = oc.dot(oc) - radius * radius;
        double discriminant = b * b - 4 * a * c;
        if (discriminant < 0) {
            return std::nullopt;
        }
        double sqrt_discriminant = std::sqrt(discriminant);
        double t0 = (-b - sqrt_discriminant) / (2.0 * a);
        double t1 = (-b + sqrt_discriminant) / (2.0 * a);
        double t = t0;
        if (t < 0) t = t1;
        if (t < 0) return std::nullopt;
        return t;
    }
};

template<class C>
struct BVH {
    struct Node {
        AABB aabb;
        typename C::template Member<Node> left, right;
        Node(const AABB &aabb) : aabb(aabb), left(this), right(this) {}
    };
    typename C::template Member<Node> root;

    BVH(std::span<const Sphere> objects) {}
};
void write_ppm(const char *filename, const std::vector<Vec3> &pixels, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        auto &pixel = pixels[i];
        fputc((int)(pixel.x * 255), fp);
        fputc((int)(pixel.y * 255), fp);
        fputc((int)(pixel.z * 255), fp);
    }
    fclose(fp);
}

int main() {
}