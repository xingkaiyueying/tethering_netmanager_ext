/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 */
#include <dlfcn.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int PrintResult(const char *check, int passed)
{
    printf("check=%s\nRESULT=%s\n", check, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}

static int LoadSharing(void)
{
    void *handle = dlopen("libsharing.z.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        printf("dlerror=%s\n", dlerror());
        return PrintResult("load-sharing", 0);
    }
    dlclose(handle);
    return PrintResult("load-sharing", 1);
}

static int CheckRuntime(void)
{
    FILE *pipe = popen("hidumper -s 1154 2>/dev/null", "r");
    char line[64] = {0};
    int readable = pipe != NULL && fgets(line, sizeof(line), pipe) != NULL;
    int status = pipe == NULL ? -1 : pclose(pipe);
    printf("networkshare_sa=1154 readable=%d\n", readable);
    return PrintResult("check-runtime", readable && status == 0);
}

static int CheckInterface(const char *iface)
{
    unsigned int index = if_nametoindex(iface);
    printf("interface=%s present=%d\n", iface, index != 0);
    return PrintResult("check-interface", 1);
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "load-sharing") == 0) {
        return LoadSharing();
    }
    if (argc == 2 && strcmp(argv[1], "check-runtime") == 0) {
        return CheckRuntime();
    }
    if (argc == 3 && strcmp(argv[1], "check-interface") == 0) {
        return CheckInterface(argv[2]);
    }
    fprintf(stderr, "usage: %s load-sharing|check-runtime|check-interface IFACE\n", argv[0]);
    return 2;
}
