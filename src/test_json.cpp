
#include "gc.h"
#include "rc.h"
#include "test_common.h"
#include <array>
#include <variant>
#include <sstream>
#include <format>
#include <fstream>
#include <cmath>
template<class C>
struct JsonValue;
template<class C>
using JsonDict = typename C::template HashMap<gc::Adaptor<std::string>,
                                              JsonValue<C>>;
template<class C>
using JsonArray = typename C::template Array<JsonValue<C>>;
template<class C>
using JsonValueBase = std::variant<
    std::monostate, bool, double, std::string, typename C::template Member<JsonDict<C>>, typename C::template Member<JsonArray<C>>>;
template<class C>
struct JsonValue : gc::GarbageCollected<JsonValueBase<C>>, JsonValueBase<C> {
    using Base = JsonValueBase<C>;
    // using Base::Base;
    void trace(const gc::Tracer &tracer) const {
        std::visit([&](const auto &v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, typename C::template Member<JsonDict<C>>>) {
                tracer(v);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, typename C::template Member<JsonArray<C>>>) {
                tracer(v);
            }
        },
                   *this);
    }

    explicit JsonValue(const typename C::template Ptr<JsonDict<C>> &v) : Base(std::monostate{}) {
        auto &dict = Base::template emplace<typename C::template Member<JsonDict<C>>>(this);
        // printf("index = %d\n", Base::index());
        dict = v;
    }
    explicit JsonValue(const typename C::template Ptr<JsonArray<C>> &v) : Base(std::monostate{}) {
        auto &array = Base::template emplace<typename C::template Member<JsonArray<C>>>(this);
        // printf("index = %d\n", Base::index());
        array = v;
    }
    explicit JsonValue(std::monostate = {}) : Base(std::monostate{}) {}
    explicit JsonValue(bool v) : Base(v) {}
    explicit JsonValue(int64_t v) : Base(v) {}
    explicit JsonValue(double v) : Base(v) {}
    explicit JsonValue(const std::string &v) : Base(v) {}
    static JsonValue array() {
        return JsonValue(C::template make<JsonArray<C>>());
    }

    static JsonValue dict() {
        return JsonValue(C::template make<JsonDict<C>>());
    }

    bool is_null() const {
        return std::holds_alternative<std::monostate>(*this);
    }
    bool is_string() const {
        return std::holds_alternative<std::string>(*this);
    }
    bool is_bool() const {
        return std::holds_alternative<bool>(*this);
    }
    bool is_number() const {
        return std::holds_alternative<double>(*this);
    }
    bool is_array() const {
        return std::holds_alternative<C::template Member<JsonArray<C>>>(*this);
    }
    bool is_dict() const {
        return std::holds_alternative<C::template Member<JsonDict<C>>>(*this);
    }
    auto as_array() {
        return std::get<C::template Member<JsonArray<C>>>(*this);
    }
    auto as_dict() {
        return std::get<C::template Member<JsonDict<C>>>(*this);
    }
    auto as_string() {
        return std::get<std::string>(*this);
    }
    auto as_bool() {
        return std::get<bool>(*this);
    }
    auto as_number() {
        return std::get<double>(*this);
    }

    size_t object_size() const {
        return sizeof(*this);
    }
    size_t object_alignment() const {
        return alignof(std::decay_t<decltype(*this)>);
    }
};
template<class C>
struct Parser {

    std::string src;
    size_t pos{};
    Parser(const std::string &src) : src(src) {
    }
    char get(size_t offset = 0) {
        auto idx = pos + offset;
        if (idx >= src.size()) {
            return '\0';
        }
        return src[idx];
    }
    void skip_ws() {
        while (std::isspace(get())) {
            pos++;
        }
    }
    C::template Owned<JsonValue<C>> parse_value() {
        skip_ws();
        char c = get();
        if (c == '{') {
            auto dict = parse_dict();
            typename C::template Ptr<JsonDict<C>> ptr = dict.get();
            auto v = C::template make<JsonValue<C>>(ptr);
            // printf("v.index = %d\n", v->index());
            return v;
        } else if (c == '[') {
            auto array = parse_array();
            return C::template make<JsonValue<C>>(array.get());
        } else if (c == '"') {
            return C::template make<JsonValue<C>>(parse_string());
        } else if (c == 't' || c == 'f') {
            return C::template make<JsonValue<C>>(parse_bool());
        } else if (c == 'n') {
            parse_null();
            return C::template make<JsonValue<C>>();
        } else {
            return C::template make<JsonValue<C>>(parse_number());
        }
    }
    bool parse_bool() {
        if (get() == 't') {
            if (get(1) == 'r' && get(2) == 'u' && get(3) == 'e') {
                pos += 4;
                return true;
            }
        } else if (get() == 'f') {
            if (get(1) == 'a' && get(2) == 'l' && get(3) == 's' && get(4) == 'e') {
                pos += 5;
                return false;
            }
        }
        throw std::runtime_error("Invalid bool");
    }
    C::template Owned<JsonValue<C>> parse_null() {
        if (get() == 'n' && get(1) == 'u' && get(2) == 'l' && get(3) == 'l') {
            pos += 4;
            return C::template make<JsonValue<C>>();
        }
        throw std::runtime_error("Invalid null");
    }
    double parse_number() {
        int integer = 0;
        int fraction = 0;
        int exponent = 0;
        bool negative = false;
        if (get() == '-') {
            negative = true;
            pos++;
        }
        while (std::isdigit(get())) {
            integer = integer * 10 + (get() - '0');
            pos++;
        }
        if (get() == '.') {
            pos++;
            int p = 1;
            while (std::isdigit(get())) {
                fraction = fraction * 10 + (get() - '0');
                p *= 10;
                pos++;
            }
            return (integer + static_cast<double>(fraction) / p) * (negative ? -1 : 1);
        } else if (get() == 'E' || get() == 'e') {
            pos++;
            bool negative_exponent = false;
            if (get() == '-') {
                negative_exponent = true;
                pos++;
            } else if (get() == '+') {
                pos++;
            }
            while (std::isdigit(get())) {
                exponent = exponent * 10 + (get() - '0');
                pos++;
            }
            double result = integer;
            if (exponent > 0) {
                result *= std::pow(10, negative_exponent ? -exponent : exponent);
            }
            return result;
        } else {
            return integer * (negative ? -1 : 1);
        }
    }
    std::string parse_string() {
        std::stringstream ss;
        pos++;
        while (true) {
            char c = get();
            if (c == '\0') {
                throw std::runtime_error("Unexpected end of string");
            }
            if (c == '"') {
                pos++;
                break;
            }
            if (c == '\\') {
                pos++;
                c = get();
                if (c == 'n') {
                    ss << '\n';
                } else if (c == 't') {
                    ss << '\t';
                } else if (c == 'r') {
                    ss << '\r';
                } else {
                    ss << c;
                }
            } else {
                ss << c;
            }
            pos++;
        }
        return ss.str();
    }
    C::template Owned<JsonArray<C>> parse_array() {
        auto array = C::template make<JsonArray<C>>();
        pos++;
        while (true) {
            skip_ws();
            auto c = get();
            if (c == '\0') {
                throw std::runtime_error("Unexpected end of array");
            }
            if (c == ']') {
                pos++;
                break;
            }
            auto value = parse_value();
            array->push_back(value);
            skip_ws();
            if (get() == ',') {
                pos++;
            }
        }
        return array;
    }
    C::template Owned<JsonDict<C>> parse_dict() {
        auto dict = C::template make<JsonDict<C>>();
        pos++;
        while (true) {
            skip_ws();
            auto c = get();
            if (c == '\0') {
                throw std::runtime_error("Unexpected end of dict");
            }
            if (c == '}') {
                pos++;
                break;
            }
            auto key = parse_string();
            skip_ws();
            if (get() != ':') {
                throw std::runtime_error("Expected ':'");
            }
            pos++;
            auto value = parse_value();
            dict->insert(std::make_pair(C::template make<gc::Adaptor<std::string>>(std::move(key)), value));
            skip_ws();
            if (get() == ',') {
                pos++;
            }
        }
        return dict;
    }
};
template<class C>
struct Formatter {
    int indent = 0;
    std::stringstream ss;

    void write_indent() {
        for (int i = 0; i < indent; i++) {
            ss << "  ";
        }
    }
    void write(const std::string &s) {
        ss << s;
    }
    void write_line(const std::string &s) {
        write_indent();
        write(s);
        ss << "\n";
    }
    void format_impl(const JsonValue<C> &value) {
        std::visit([&](const auto &v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) {
                write("null");
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, bool>) {
                write(v ? "true" : "false");
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, double>) {
                ss << std::format("{:.17g}", v);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                ss << std::format("\"{}\"", v);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, typename C::template Member<JsonDict<C>>>) {
                write_line("{");
                indent++;
                for (auto &&[key, value] : *v) {
                    write_indent();
                    ss << std::format("\"{}\": ", key->c_str());
                    format_impl(*value);
                    ss << ",\n";
                }
                indent--;
                write_line("}");
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, typename C::template Member<JsonArray<C>>>) {
                write_line("[");
                indent++;
                for (auto &value : *v) {
                    write_indent();
                    format_impl(*value);
                    ss << ",\n";
                }
                indent--;
                write_line("]");
            }
        },
                   value);
    }
    static std::string format(const JsonValue<C> &value) {
        Formatter<C> formatter;
        formatter.format_impl(value);
        return formatter.ss.str();
    }
};
template<class C>
C::template Owned<JsonValue<C>> parse_json(C policy, const std::string &json) {
    Parser<C> parser(json);
    return parser.parse_value();
}
using StatsTracker = gc::StatsTracker;
// int main() {
//     auto src = R"(
// {
//     "key1": "value1",
//     "key2": 123,
//     "key3": true,
//     "key4": null,
//     "key5": {
//         "key6": "value6",
//         "key7": 456,
//         "key8": false,
//         "key9": null
//     },
//     } )";
//     gc::GcOption option{};
//     option.max_heap_size = 1024 * 1024 * 1024;
//     GcPolicy{}.init();
//     {
//         auto json = parse_json(GcPolicy{}, src);
//         std::cout << Formatter<GcPolicy>::format(*json) << std::endl;
//     }
//     GcPolicy{}.finalize();
//     return 0;
// }
int main() {
    auto bench = []<class C>(C policy) {
        policy.init();
        StatsTracker tracker;
        for (int i = 0; i < 3; i++) {
            // download from https://github.com/json-iterator/test-data/blob/master/large-file.json
            std::ifstream ifs("large-file.json");
            if (!ifs) {
                std::cerr << "Failed to open file large-file.json, please download from https://raw.githubusercontent.com/json-iterator/test-data/refs/heads/master/large-file.json" << std::endl;
                std::exit(1);
            }
            auto t = std::chrono::high_resolution_clock::now();
            std::string json_s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            auto json = parse_json(policy, json_s);
            auto elapsed = (std::chrono::high_resolution_clock::now() - t).count() * 1e-9;
            tracker.update(elapsed);
            std::cout << Formatter<C>::format(*json) << std::endl;
        }
        tracker.print(policy.name().c_str());
        policy.finalize();
    };

    // bench(RcPolicy<rc::RefCounter>{});
    // bench(RcPolicy<rc::AtomicRefCounter>{});
    gc::GcOption option{};
    option.max_heap_size = 1024 * 1024 * 256;
    option.mode = gc::GcMode::INCREMENTAL;
    bench(GcPolicy{option});

    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});

    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
    option.n_collector_threads = 4;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
    return 0;
}