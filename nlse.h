#ifndef NLSE_H
#define NLSE_H

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#define NLSE_API_VERSION 1

#define NLSE_GAME_VERSION(major, minor, build, revision)                                                       \
    (((uint64_t)(major) << 48) | ((uint64_t)(minor) << 32) | ((uint64_t)(build) << 16) | (uint64_t)(revision))

#ifdef __cplusplus
#define NLSE_EXPORT extern "C" __declspec(dllexport)
#else
#define NLSE_EXPORT __declspec(dllexport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t NLSEPluginHandle;

enum {
    NLSE_MESSAGE_POST_LOAD = 1,
    NLSE_MESSAGE_POST_POST_LOAD = 2,
};

typedef struct NLSEPluginVersion {
    uint32_t apiVersion;
    char name[64];
    char version[32];
    char author[64];
    uint64_t gameVersions[16];
} NLSEPluginVersion;

typedef struct NLSEPluginInfo {
    NLSEPluginHandle handle;
    const char* name;
    const char* version;
    const char* author;
} NLSEPluginInfo;

typedef struct NLSEMessage {
    const char* sender;
    uint32_t type;
    uint32_t size;
    const void* data;
} NLSEMessage;

typedef void (*NLSEMessageCallback)(const NLSEMessage* message);

typedef struct NLSEInterface {
    uint32_t apiVersion;
    const char* nlseVersion;
    uint64_t gameVersion;
    uintptr_t gameBase;
    const char* gameDirectory;
    NLSEPluginHandle (*GetPluginHandle)(void);
    const NLSEPluginInfo* (*GetPluginInfo)(const char* name);
    void (*Log)(NLSEPluginHandle plugin, const char* format, ...);
    bool (*RegisterListener)(NLSEPluginHandle plugin, const char* sender, NLSEMessageCallback callback);
    bool (*Dispatch)(NLSEPluginHandle plugin, uint32_t type, const void* data, uint32_t size, const char* receiver);
    uintptr_t (*FindPattern)(const char* pattern);
    bool (*WriteMemory)(uintptr_t address, const void* bytes, size_t size);
    uintptr_t (*WriteCall)(uintptr_t site, void* function);
    bool (*WriteJump)(uintptr_t site, void* function);
    bool (*HookImport)(const char* library, const char* function, void* replacement, void** original);
} NLSEInterface;

typedef bool (*NLSEPluginLoad)(const NLSEInterface* nlse);

#ifdef __cplusplus
}
#endif

#endif
