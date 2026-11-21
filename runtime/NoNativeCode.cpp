// The table of translated guest code for programs that are built without any. Programs with
// translations link the modules.c that tools/build-native-code.py writes instead.
#include "NativeCode.hpp"

extern "C" {
extern const nestvane::native::Module nestvane_native_modules[] = {{0, 0, nullptr, 0, nullptr}};
extern const int nestvane_native_module_count = 0;
}
