/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <property_info_serializer/property_info_serializer.h>
#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    std::string bytes, error;
    if (!android::properties::BuildTrie({}, "u:object_r:nce_property:s0", "string", &bytes, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    FILE *out = fopen(argv[1], "wb");
    if (!out)
        return 3;
    bool wrote = fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    return fclose(out) || !wrote ? 4 : 0;
}
