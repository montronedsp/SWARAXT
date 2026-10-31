// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Linux test-only interposition, scoped to this suite's private fixture names.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
namespace {
bool fixture(const char* path) { return path != nullptr && std::strstr(path, "/swara-fault-") != nullptr; }
bool cleanupFault(const char* path) {
    return fixture(path) && std::getenv("SWARA_TEST_NO_UNLINK") != nullptr
        && std::strlen(path) >= 4 && std::strcmp(path + std::strlen(path) - 4, ".tmp") == 0;
}
}
extern "C" int link(const char* from, const char* to) noexcept {
    if (fixture(to) && std::getenv("SWARA_TEST_NO_LINK") != nullptr) { errno = EOPNOTSUPP; return -1; }
    static auto real = reinterpret_cast<int (*)(const char*,const char*)>(dlsym(RTLD_NEXT,"link"));
    return real(from,to);
}
extern "C" int unlink(const char* path) noexcept {
    if (cleanupFault(path)) { errno=EACCES; return -1; }
    static auto real = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_NEXT,"unlink"));
    return real(path);
}
extern "C" int remove(const char* path) noexcept {
    if (cleanupFault(path)) { errno=EACCES; return -1; }
    static auto real = reinterpret_cast<int (*)(const char*)>(dlsym(RTLD_NEXT,"remove"));
    return real(path);
}

extern "C" ssize_t write(int fd, const void* data, size_t bytes) {
    static auto real = reinterpret_cast<ssize_t (*)(int,const void*,size_t)>(dlsym(RTLD_NEXT,"write"));
    if (std::getenv("SWARA_TEST_SHORT_WRITE") != nullptr && bytes > 1) {
        char descriptor[64], path[4096];
        std::snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
        const auto size = readlink(descriptor,path,sizeof(path)-1);
        if (size > 0) {
            path[size] = 0;
            if (fixture(path) && std::strstr(path,".tmp") != nullptr)
                return real(fd,data,bytes/2);
        }
    }
    return real(fd,data,bytes);
}
