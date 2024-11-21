
#include "gc.h"
#include "rc.h"
#include <array>


struct JsonValue : gc::Traceable {
    enum class Type {
        NULL_VALUE,
        BOOL,
        NUMBER,
        STRING,
        ARRAY,
        OBJECT
    };
};

int main() {

}