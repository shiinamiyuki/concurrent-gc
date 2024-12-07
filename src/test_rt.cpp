
#include "gc.h"
#include "rc.h"
#include "test_common.h"
#include <span>
#include <cmath>
template<class C>
struct Vec3 : C::template Enable<Vec3<C>> {
    double x, y, z;
    Vec3(double x, double y, double z) : x(x), y(y), z(z) {}
    Vec3(const Vec3 &v) : Vec3(v.x, v.y, v.z) {}
    C::template Owned<Vec3> operator+(const Vec3 &v) const {
        return C::template make<Vec3>(x + v.x, y + v.y, z + v.z);
    }
    C::template Owned<Vec3> operator-(const Vec3 &v) const {
        return C::template make<Vec3>(x - v.x, y - v.y, z - v.z);
    }

    C::template Owned<Vec3> operator*(double scalar) const {
        return C::template make<Vec3>(x * scalar, y * scalar, z * scalar);
    }
    C::template Owned<Vec3> operator*(const Vec3 &v) const {
        return C::template make<Vec3>(x * v.x, y * v.y, z * v.z);
    }
    C::template Owned<Vec3> operator/(double scalar) const {
        return C::template make<Vec3>(x / scalar, y / scalar, z / scalar);
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
    C::template Owned<Vec3> normalized() const {
        double len = std::sqrt(x * x + y * y + z * z);
        return C::template make<Vec3>(x / len, y / len, z / len);
    }
    C::template Owned<Vec3> cross(const Vec3 &v) const {
        return C::template make<Vec3>(
            y * v.z - z * v.y,
            z * v.x - x * v.z,
            x * v.y - y * v.x);
    }
    double max_element() const {
        return std::max<double>({x, y, z});
    }
    GC_CLASS()
};

template<class C>
struct Ray : C::template Enable<Ray<C>> {
    using Vec3 = ::Vec3<C>;
    typename C::template Member<Vec3> origin, direction;
    Ray() : origin(this), direction(this) {}
    Ray(typename C::template Ptr<Vec3> origin, typename C::template Ptr<Vec3> direction) : origin(this), direction(this) {
        this->origin = origin;
        this->direction = direction;
    }
    typename C::template Owned<Vec3> at(double t) const {
        return *origin + *(*direction * t);
    }
    GC_CLASS(origin, direction)
};

template<class C>
struct Sphere : C::template Enable<Sphere<C>> {
    using Vec3 = ::Vec3<C>;
    using Ray = ::Ray<C>;
    typename C::template Member<Vec3> center;
    double radius;
    typename C::template Member<Vec3> color;
    GC_CLASS(center, color)
    Sphere(typename C::template Ptr<Vec3> center, double radius, typename C::template Ptr<Vec3> color) : center(this), radius(radius), color(this) {
        this->center = center;
        this->color = color;
    }
    // AABB aabb() const {
    //     return AABB(center - Vec3(radius, radius, radius), center + Vec3(radius, radius, radius));
    // }
    std::optional<double> intersect(const Ray &ray) const {
        auto oc = *ray.origin - *center;
        double a = ray.direction->dot(*ray.direction);
        double b = 2.0 * oc->dot(*ray.direction);
        double c = oc->dot(*oc) - radius * radius;
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
    typename C::template Owned<Vec3> normal(const Vec3 &point) const {
        return (point - *center)->normalized();
    }
};

void write_ppm(const char *filename, const std::vector<std::tuple<double, double, double>> &pixels, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        auto &pixel = pixels[i];
        auto &&[r, g, b] = pixel;
        fputc(static_cast<int>(r * 255), fp);
        fputc(static_cast<int>(g * 255), fp);
        fputc(static_cast<int>(b * 255), fp);
    }
    fclose(fp);
}
template<class C>
struct Onb : C::template Enable<Onb<C>> {
    using Vec3 = ::Vec3<C>;
    typename C::template Member<Vec3> tangent, bitangent, normal;
    GC_CLASS(tangent, bitangent, normal)
    explicit Onb(typename C::template Ptr<Vec3> n) : tangent(this), bitangent(this), normal(this) {
        normal = n;
        if (std::abs(normal->x) > std::abs(normal->y)) {
            tangent = C::template make<Vec3>(-normal->z, 0, normal->x)->normalized();
        } else {
            tangent = C::template make<Vec3>(0, normal->z, -normal->y)->normalized();
        }
        bitangent = normal->cross(*tangent);
    }
    typename C::template Owned<Vec3> to_local(const Vec3 &v) const {
        auto x = v.dot(*tangent);
        auto y = v.dot(*bitangent);
        auto z = v.dot(*normal);
        return C::template make<Vec3>(x, y, z);
    }
    typename C::template Owned<Vec3> to_world(const Vec3 &v) const {
        return *tangent * v.x + *bitangent * v.y + *normal * v.z;
    }
};

template<class C>
void render(C policy, int width, int height, bool parallel = false) {
    auto name = policy.name();
    printf("rendering %s", name.c_str());
    if (parallel) {
        printf(" parallel\n");
    } else {
        printf("\n");
    }
    int n_threads = 4;
    gc::ThreadPool pool{static_cast<size_t>(n_threads)};
    policy.init();
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::tuple<double, double, double>> pixels(width * height);
    {
        using Vec3 = ::Vec3<C>;
        using Ray = ::Ray<C>;
        using Sphere = ::Sphere<C>;

        auto generate_ray = [&](int x, int y) -> typename C::template Owned<Ray> {
            double u = (x + 0.5) / width;
            double v = (y + 0.5) / height;
            u = 2 * u - 1;
            v = 1 - 2 * v;
            auto d = C::template make<Vec3>(u, v, 0.6)->normalized();
            auto o = C::template make<Vec3>(0, 0, 0);
            return C::template make<Ray>(o.get(), d.get());
        };
        // init spheres
        auto spheres = C::template make<typename C::template Array<Sphere>>();
        {
            auto make_sphere = [&](double x, double y, double z, double rad, double r, double g, double b) {
                auto center = C::template make<Vec3>(x, y, z);
                auto color = C::template make<Vec3>(r, g, b);
                auto sphere = C::template make<Sphere>(center.get(), rad, color.get());
                spheres->push_back(sphere);
            };
            make_sphere(0.0, 0.0, 1.0, 0.5, 0.8, 0.3, 0.3);
            make_sphere(0.0, -100.5, 1.0, 100, 0.8, 0.8, 0.0);
            make_sphere(1.0, 0.0, 1.0, 0.5, 0.8, 0.6, 0.2);
            make_sphere(-1.0, 0.0, 1.0, 0.5, 0.8, 0.8, 0.8);
        }

        auto render_pixel = [&](int x, int y) {
            auto ray = generate_ray(x, y);
            auto color = C::template make<Vec3>(0, 0, 0);
            auto throughput = C::template make<Vec3>(1, 1, 1);
            auto sky_color = C::template make<Vec3>(0.5, 0.7, 1.0);
            for (int d = 0; d < 5; d++) {
                std::optional<double> hit = std::nullopt;
                typename C::template Ptr<Sphere> hit_sphere{};
                for (auto &sphere : *spheres) {
                    auto t = sphere->intersect(*ray);
                    if (t.has_value() && t > 0.0001) {
                        if (!hit.has_value() || t.value() < hit.value()) {
                            hit = t;
                            hit_sphere = sphere.get();
                        }
                    }
                }
                if (hit.has_value()) {
                    auto &color = hit_sphere->color;
                    throughput = *throughput * *color;
                    auto normal = hit_sphere->normal(*ray->at(hit.value()));
                    // reflection
                    auto reflected = *ray->direction - *(*normal * (2.0 * ray->direction->dot(*normal)));
                    ray->origin = ray->at(hit.value());
                    ray->direction = reflected;
                } else {
                    // add sky color
                    color->x += throughput->x * sky_color->x;
                    color->y += throughput->y * sky_color->y;
                    color->z += throughput->z * sky_color->z;
                    break;
                }
            }
            pixels[y * width + x] = {color->x, color->y, color->z};
        };
        if (!parallel) {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    render_pixel(x, y);
                }
            }
        } else {
            pool.dispatch([&](size_t tid) {
                // divide the work
                auto y_chunk = (height + n_threads - 1) / n_threads;
                auto y_start = y_chunk * tid;
                auto y_end = std::min<int>(y_start + y_chunk, height);
                for (int y = y_start; y < y_end; y++) {
                    for (int x = 0; x < width; x++) {
                        render_pixel(x, y);
                    }
                }
            });
        }
    }
    auto elapsed = (std::chrono::high_resolution_clock::now() - t0).count() * 1e-9;
    policy.finalize();
    printf("rendering %s took %f s\n", name.c_str(), elapsed);
    auto output_filename = std::string("output_") + name;
    if (parallel) {
        output_filename += "_parallel";
    }
    output_filename += ".ppm";
    write_ppm(output_filename.c_str(), pixels, width, height);
}

int main() {
    // the parallel version could run out of memory due to mutators allocates too fast
    // the error during an OOM might not be straightforward due to std::abort() in multithreaded context
    int w = 800*3, h = 800*3;
    render(RcPolicy<rc::RefCounter>{}, w, h);
    render(RcPolicy<rc::AtomicRefCounter>{}, w, h);
    render(RcPolicy<rc::AtomicRefCounter>{}, w, h, true);
    gc::GcOption option{};
    option.max_heap_size = 1024 * 1024 * 64;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    render(GcPolicy{option}, w, h);
    option.mode = gc::GcMode::INCREMENTAL;
    render(GcPolicy{option}, w, h);
    option.mode = gc::GcMode::CONCURRENT;
    render(GcPolicy{option}, w, h);
    render(GcPolicy{option}, w, h, true);
    option.mode = gc::GcMode::STOP_THE_WORLD;
    option.n_collector_threads = 4;
    render(GcPolicy{option}, w, h);
    option.mode = gc::GcMode::CONCURRENT;
    render(GcPolicy{option}, w, h);
    render(GcPolicy{option}, w, h, true);
}