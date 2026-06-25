// Shared main() for every host test binary. Links against Test.h's registry.
#include "Test.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    return ::test::Registry::instance().run(filter);
}
