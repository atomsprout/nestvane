// Reads system properties, to check the property area that properties.py writes.
#include <stdio.h>
#include <sys/system_properties.h>

static void show(const prop_info* info, void* cookie) {
    char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
    __system_property_read(info, name, value);
    printf("[%s]: [%s]\n", name, value);
    ++*(int*)cookie;
}

int main(int argc, char** argv) {
    char value[PROP_VALUE_MAX];
    for (int index = 1; index < argc; ++index) {
        const int length = __system_property_get(argv[index], value);
        printf("%s=%s (%d)\n", argv[index], value, length);
    }
    int count = 0;
    if (argc == 1) __system_property_foreach(show, &count);
    if (argc == 1) printf("properties=%d\n", count);
    return 0;
}
