#include <windows.h>
#include <d3d12.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

// SpiderFix - proxy d3d12.dll para Marvel's Spider-Man 2 em GPU sem FL 12_1.
// Funcao principal: fallback de feature level em D3D12CreateDevice, spoof de
// FEATURE_LEVELS para 12_0, retry de PSO depth-only, supressao do aviso de GPU.
//
// Limitacoes conhecidas (nao cobertas, sem quebrar o jogo):
// - Retry depth-only cobre ID3D12Device::CreateGraphicsPipelineState (vtable 10).
//   ID3D12Device2::CreatePipelineState (stream, vtable 47) e
//   ID3D12PipelineLibrary::LoadGraphicsPipeline (cache.pso) nao passam pelo retry.
// - Devices criados via D3D12GetInterface/DeviceFactory nao passam pelo fallback.
// - IAT patch cobre user32.dll do exe (MessageBoxA/W/ExA/ExW). Delay-load, outras
//   DLLs, MessageBoxIndirect e TaskDialog nao sao cobertos.
// - Retry remove o PS: se ele faz alpha-test (clip/discard) ou escreve SV_Depth,
// sombra/profundidade sai errada nesses PSOs. Jogo vivo > pixel perfeito.

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED 0x887A0004L
#endif

#define SPIDERFIX_VERSION 0x00010100u // v1.1.0

using D3D12CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using MessageBoxA_t = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxExA_t = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT, WORD);
using MessageBoxExW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD);
using CheckFeatureSupport_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_FEATURE, void*, UINT);
using CreateGraphicsPipelineState_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
    REFIID,
    void**);

// Indices na vtable de ID3D12Device (apos IUnknown + ID3D12Object + ID3D12DeviceChild).
static constexpr size_t kCreateGraphicsPipelineStateVtableIndex = 10;
static constexpr size_t kCheckFeatureSupportVtableIndex = 13;
static constexpr size_t kMaxVtableHooks = 8;
static constexpr ULONGLONG kMaxLogBytes = 512 * 1024;

static INIT_ONCE g_d3d12InitOnce = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_configOnce = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_msgboxOnce = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_devicePatchLock = SRWLOCK_INIT;
static SRWLOCK g_logLock = SRWLOCK_INIT;

static HMODULE g_originalD3D12 = nullptr;
static D3D12CreateDevice_t g_originalD3D12CreateDevice = nullptr;
static MessageBoxA_t g_originalMessageBoxA = nullptr;
static MessageBoxW_t g_originalMessageBoxW = nullptr;
static MessageBoxExA_t g_originalMessageBoxExA = nullptr;
static MessageBoxExW_t g_originalMessageBoxExW = nullptr;

// Originais por vtable: cada driver/layer tem a sua; chamar funcao de outra
// vtable com this estranho quebra. Hooks resolvem pela vtable do proprio self.
struct VtableHook {
    void* vtable;
    CheckFeatureSupport_t check;
    CreateGraphicsPipelineState_t pso;
};
static VtableHook g_vhooks[kMaxVtableHooks] = {};
static size_t g_vhookCount = 0;

// Tabela de repasse dos exports (ordem sincronizada com D3D12ProxyStubs.asm).
// Preenchida uma vez no InitOriginalD3D12; entradas ausentes ganham fallback
// local, entao o stub vira so jmp (preserva XMM0-3, sem unwind, sem chamada).
extern "C" void* g_procs[18] = {};

static HRESULT WINAPI SpiderFixNotImpl() { return E_NOTIMPL; }
static void* WINAPI SpiderFixZero() { return nullptr; }

alignas(64) static LONG g_featureLogBudget = 80;
alignas(64) static LONG g_psoFailureLogBudget = 80;

static LONG g_logDisabled = 0; // 1 = SPIDERFIX_LOG=0
static LONG g_depthRetry = 1;  // 0 = SPIDERFIX_DEPTH_RETRY=0
static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static ULONGLONG g_logBytes = 0;
static LONG g_logFull = 0;
static wchar_t g_logPath[MAX_PATH] = {};

static BOOL CALLBACK InitConfig(PINIT_ONCE, PVOID, PVOID*) {
    char v[8] {};
    DWORD n = GetEnvironmentVariableA("SPIDERFIX_LOG", v, sizeof(v));
    g_logDisabled = (n > 0 && (v[0] == '0' || v[0] == 'N' || v[0] == 'n')) ? 1 : 0;
    n = GetEnvironmentVariableA("SPIDERFIX_DEPTH_RETRY", v, sizeof(v));
    g_depthRetry = (n > 0 && (v[0] == '0' || v[0] == 'N' || v[0] == 'n')) ? 0 : 1;
    return TRUE;
}

static void EnsureConfig() {
    PVOID ctx = nullptr;
    InitOnceExecuteOnce(&g_configOnce, InitConfig, nullptr, &ctx);
}

static void EnsureLogOpen() {
    if (g_hLog != INVALID_HANDLE_VALUE || g_logFull) {
        return;
    }
    if (!g_logPath[0]) {
        HMODULE self = nullptr;
        wchar_t dir[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&EnsureLogOpen), &self) && self &&
            GetModuleFileNameW(self, dir, MAX_PATH)) {
            wchar_t* slash = wcsrchr(dir, L'\\');
            size_t dirLen = slash ? static_cast<size_t>(slash - dir + 1) : 0;
            static const wchar_t kName[] = L"spiderfix.log";
            if (dirLen + 20 < MAX_PATH) {
                memcpy(g_logPath, dir, dirLen * sizeof(wchar_t));
                memcpy(g_logPath + dirLen, kName, sizeof(kName));
            }
        }
        if (!g_logPath[0]) {
            static const wchar_t kFallback[] = L"spiderfix.log";
            memcpy(g_logPath, kFallback, sizeof(kFallback));
        }
    }
    // Rotacao real: acima de 512KB move para .old e recomeca zerado.
    HANDLE probe = CreateFileW(g_logPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz {};
        bool big = GetFileSizeEx(probe, &sz) &&
            static_cast<ULONGLONG>(sz.QuadPart) > kMaxLogBytes;
        CloseHandle(probe);
        if (big) {
            wchar_t oldp[MAX_PATH] = {};
            size_t len = wcslen(g_logPath);
            if (len + 5 < MAX_PATH) {
                memcpy(oldp, g_logPath, len * sizeof(wchar_t));
                static const wchar_t kOld[] = L".old";
                memcpy(oldp + len, kOld, sizeof(kOld));
                DeleteFileW(oldp);
                MoveFileExW(g_logPath, oldp, MOVEFILE_REPLACE_EXISTING);
            }
            g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        } else {
            g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_hLog != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER zero {};
                LARGE_INTEGER end {};
                SetFilePointerEx(g_hLog, zero, &end, FILE_END);
                g_logBytes = static_cast<ULONGLONG>(end.QuadPart);
            }
        }
    } else {
        g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_hLog == INVALID_HANDLE_VALUE) {
        g_logFull = 1;
    }
}

static void Log(const char* msg) {
    EnsureConfig();
    if (g_logDisabled || !msg) {
        return;
    }
    AcquireSRWLockExclusive(&g_logLock);
    if (g_logFull) {
        ReleaseSRWLockExclusive(&g_logLock);
        return;
    }
    if (g_hLog == INVALID_HANDLE_VALUE) {
        EnsureLogOpen();
    }
    if (g_hLog == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_logLock);
        return;
    }
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char line[640] {};
    int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u - %s\r\n",
        static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond), msg);
    if (n > 0) {
        size_t len = static_cast<size_t>(n);
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        DWORD written = 0;
        if (WriteFile(g_hLog, line, static_cast<DWORD>(len), &written, nullptr)) {
            g_logBytes += written;
        }
        if (g_logBytes > kMaxLogBytes) {
            CloseHandle(g_hLog);
            g_hLog = INVALID_HANDLE_VALUE;
            g_logFull = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

// So decrementa com orcamento positivo: falhas tardias continuam visiveis.
static bool TakeBudget(LONG* budget) {
    if (InterlockedOr(budget, 0) <= 0) {
        return false;
    }
    return InterlockedDecrement(budget) >= 0;
}

static uint32_t Fnv1a(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static bool ContainsInsensitiveAscii(const char* text, const char* needle) {
    if (!text || !needle || !*needle) {
        return false;
    }
    size_t nLen = strlen(needle);
    for (const char* p = text; *p; ++p) {
        size_t i = 0;
        for (; i < nLen && p[i]; ++i) {
            if (tolower(static_cast<unsigned char>(p[i])) !=
                tolower(static_cast<unsigned char>(needle[i]))) {
                break;
            }
        }
        if (i == nLen) {
            return true;
        }
    }
    return false;
}

static bool ContainsInsensitiveWideAscii(const wchar_t* text, const wchar_t* needle) {
    if (!text || !needle || !*needle) {
        return false;
    }
    size_t nLen = wcslen(needle);
    for (const wchar_t* p = text; *p; ++p) {
        size_t i = 0;
        for (; i < nLen && p[i]; ++i) {
            if (towlower(p[i]) != towlower(needle[i])) {
                break;
            }
        }
        if (i == nLen) {
            return true;
        }
    }
    return false;
}

static bool IsGpuCompatibilityWarningA(LPCSTR text, LPCSTR caption) {
    bool body = text && ContainsInsensitiveAscii(text, "gpu") &&
        (ContainsInsensitiveAscii(text, "compat") ||
         ContainsInsensitiveAscii(text, "minimum") ||
         ContainsInsensitiveAscii(text, "requisitos"));
    bool head = caption && ContainsInsensitiveAscii(caption, "gpu") &&
        (ContainsInsensitiveAscii(caption, "compat") ||
         ContainsInsensitiveAscii(caption, "minimum") ||
         ContainsInsensitiveAscii(caption, "requisitos"));
    return body || head;
}

static bool IsGpuCompatibilityWarningW(LPCWSTR text, LPCWSTR caption) {
    bool body = text && ContainsInsensitiveWideAscii(text, L"gpu") &&
        (ContainsInsensitiveWideAscii(text, L"compat") ||
         ContainsInsensitiveWideAscii(text, L"minimum") ||
         ContainsInsensitiveWideAscii(text, L"requisitos"));
    bool head = caption && ContainsInsensitiveWideAscii(caption, L"gpu") &&
        (ContainsInsensitiveWideAscii(caption, L"compat") ||
         ContainsInsensitiveWideAscii(caption, L"minimum") ||
         ContainsInsensitiveWideAscii(caption, L"requisitos"));
    return body || head;
}

// Devolve o botao que o jogo espera para cada tipo: MB_YESNO espera IDYES/IDNO,
// e IDOK (1) cairia no ramo errado.
static int SuppressedResult(UINT type) {
    switch (type & 0xFu) { // MB_TYPEMASK
    case MB_YESNO:
    case MB_YESNOCANCEL:
        return IDYES;
    case MB_RETRYCANCEL:
        return IDRETRY;
    case MB_ABORTRETRYIGNORE:
        return IDIGNORE;
    case MB_CANCELTRYCONTINUE:
        return IDCONTINUE;
    default:
        return IDOK;
    }
}

static void LogSuppressedA(LPCSTR text, UINT type) {
    char buf[640] {};
    std::snprintf(buf, sizeof(buf), "Aviso GPU suprimido (A type=0x%X): %s",
        static_cast<unsigned>(type), text ? text : "(null)");
    Log(buf);
}

static void LogSuppressedW(LPCWSTR text, UINT type) {
    char narrow[512] {};
    if (text) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1,
            narrow, static_cast<int>(sizeof(narrow) - 1), nullptr, nullptr);
    } else {
        narrow[0] = '\0';
    }
    char buf[640] {};
    std::snprintf(buf, sizeof(buf), "Aviso GPU suprimido (W type=0x%X): %s",
        static_cast<unsigned>(type), narrow[0] ? narrow : "(null)");
    Log(buf);
}

extern "C" int WINAPI HookedMessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    if (IsGpuCompatibilityWarningA(text, caption)) {
        LogSuppressedA(text, type);
        return SuppressedResult(type);
    }
    return g_originalMessageBoxA ?
        g_originalMessageBoxA(hwnd, text, caption, type) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) {
    if (IsGpuCompatibilityWarningW(text, caption)) {
        LogSuppressedW(text, type);
        return SuppressedResult(type);
    }
    return g_originalMessageBoxW ?
        g_originalMessageBoxW(hwnd, text, caption, type) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxExA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type, WORD lang) {
    if (IsGpuCompatibilityWarningA(text, caption)) {
        LogSuppressedA(text, type);
        return SuppressedResult(type);
    }
    return g_originalMessageBoxExA ?
        g_originalMessageBoxExA(hwnd, text, caption, type, lang) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxExW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) {
    if (IsGpuCompatibilityWarningW(text, caption)) {
        LogSuppressedW(text, type);
        return SuppressedResult(type);
    }
    return g_originalMessageBoxExW ?
        g_originalMessageBoxExW(hwnd, text, caption, type, lang) :
        SuppressedResult(type);
}

static bool PatchImport(const char* moduleName, const char* functionName, void* hook, void** original) {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        return false;
    }

    auto* base = reinterpret_cast<unsigned char*>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) {
        return false;
    }

    auto* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; importDesc->Name; ++importDesc) {
        const char* importedModule = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(importedModule, moduleName) != 0) {
            continue;
        }

        if (!importDesc->OriginalFirstThunk) {
            continue; // Sem tabela de nomes: FirstThunk ja tem enderecos (item 14).
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);
        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + importDesc->OriginalFirstThunk);

        for (; thunk->u1.Function; ++thunk, ++originalThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), functionName) != 0) {
                continue;
            }

            // Preserva EXECUTE se a pagina ja for executavel (exe empacotado).
            MEMORY_BASIC_INFORMATION imbi {};
            DWORD req = PAGE_READWRITE;
            if (VirtualQuery(&thunk->u1.Function, &imbi, sizeof(imbi)) >= sizeof(imbi) &&
                (imbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                req = PAGE_EXECUTE_READWRITE;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), req, &oldProtect)) {
                return false;
            }

            if (original && !*original) {
                *original = reinterpret_cast<void*>(thunk->u1.Function);
            }
            if (thunk->u1.Function == reinterpret_cast<ULONG_PTR>(hook)) {
                DWORD ignored = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &ignored);
                return true;
            }
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);

            DWORD ignored = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &ignored);

            char buf[256] {};
            std::snprintf(buf, sizeof(buf), "IAT patch aplicado: %s!%s", moduleName, functionName);
            Log(buf);
            return true;
        }
    }

    return false;
}

static BOOL CALLBACK InitMsgboxPatch(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        user32 = LoadLibraryW(L"user32.dll");
    }
    if (user32) {
        if (!g_originalMessageBoxA) {
            g_originalMessageBoxA = reinterpret_cast<MessageBoxA_t>(
                GetProcAddress(user32, "MessageBoxA"));
        }
        if (!g_originalMessageBoxW) {
            g_originalMessageBoxW = reinterpret_cast<MessageBoxW_t>(
                GetProcAddress(user32, "MessageBoxW"));
        }
        if (!g_originalMessageBoxExA) {
            g_originalMessageBoxExA = reinterpret_cast<MessageBoxExA_t>(
                GetProcAddress(user32, "MessageBoxExA"));
        }
        if (!g_originalMessageBoxExW) {
            g_originalMessageBoxExW = reinterpret_cast<MessageBoxExW_t>(
                GetProcAddress(user32, "MessageBoxExW"));
        }
    }

    PatchImport("user32.dll", "MessageBoxA", reinterpret_cast<void*>(&HookedMessageBoxA),
        reinterpret_cast<void**>(&g_originalMessageBoxA));
    PatchImport("user32.dll", "MessageBoxW", reinterpret_cast<void*>(&HookedMessageBoxW),
        reinterpret_cast<void**>(&g_originalMessageBoxW));
    PatchImport("user32.dll", "MessageBoxExA", reinterpret_cast<void*>(&HookedMessageBoxExA),
        reinterpret_cast<void**>(&g_originalMessageBoxExA));
    PatchImport("user32.dll", "MessageBoxExW", reinterpret_cast<void*>(&HookedMessageBoxExW),
        reinterpret_cast<void**>(&g_originalMessageBoxExW));
    return TRUE;
}

static void EnsureMsgboxPatch() {
    PVOID ctx = nullptr;
    InitOnceExecuteOnce(&g_msgboxOnce, InitMsgboxPatch, nullptr, &ctx);
}

static const char* FeatureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "unknown";
    }
}

static const char* FeatureName(D3D12_FEATURE feature) {
    switch (feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: return "OPTIONS";
    case D3D12_FEATURE_ARCHITECTURE: return "ARCHITECTURE";
    case D3D12_FEATURE_FEATURE_LEVELS: return "FEATURE_LEVELS";
    case D3D12_FEATURE_FORMAT_SUPPORT: return "FORMAT_SUPPORT";
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: return "MULTISAMPLE_QUALITY_LEVELS";
    case D3D12_FEATURE_FORMAT_INFO: return "FORMAT_INFO";
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: return "GPU_VIRTUAL_ADDRESS_SUPPORT";
    case D3D12_FEATURE_SHADER_MODEL: return "SHADER_MODEL";
    case D3D12_FEATURE_D3D12_OPTIONS1: return "OPTIONS1";
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: return "PROTECTED_SESSION_SUPPORT";
    case D3D12_FEATURE_ROOT_SIGNATURE: return "ROOT_SIGNATURE";
    case D3D12_FEATURE_ARCHITECTURE1: return "ARCHITECTURE1";
    case D3D12_FEATURE_D3D12_OPTIONS2: return "OPTIONS2";
    case D3D12_FEATURE_SHADER_CACHE: return "SHADER_CACHE";
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: return "QUEUE_PRIORITY";
    case D3D12_FEATURE_D3D12_OPTIONS3: return "OPTIONS3";
    case D3D12_FEATURE_EXISTING_HEAPS: return "EXISTING_HEAPS";
    case D3D12_FEATURE_D3D12_OPTIONS4: return "OPTIONS4";
    case D3D12_FEATURE_SERIALIZATION: return "SERIALIZATION";
    case D3D12_FEATURE_CROSS_NODE: return "CROSS_NODE";
    case D3D12_FEATURE_D3D12_OPTIONS5: return "OPTIONS5";
    case D3D12_FEATURE_DISPLAYABLE: return "DISPLAYABLE";
    case D3D12_FEATURE_D3D12_OPTIONS6: return "OPTIONS6";
    case D3D12_FEATURE_QUERY_META_COMMAND: return "QUERY_META_COMMAND";
    case D3D12_FEATURE_D3D12_OPTIONS7: return "OPTIONS7";
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT: return "PROTECTED_TYPE_COUNT";
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: return "PROTECTED_TYPES";
    case D3D12_FEATURE_D3D12_OPTIONS8: return "OPTIONS8";
    case D3D12_FEATURE_D3D12_OPTIONS9: return "OPTIONS9";
    case D3D12_FEATURE_D3D12_OPTIONS10: return "OPTIONS10";
    case D3D12_FEATURE_D3D12_OPTIONS11: return "OPTIONS11";
    case D3D12_FEATURE_D3D12_OPTIONS12: return "OPTIONS12";
    case D3D12_FEATURE_D3D12_OPTIONS13: return "OPTIONS13";
    case D3D12_FEATURE_D3D12_OPTIONS14: return "OPTIONS14";
    case D3D12_FEATURE_D3D12_OPTIONS15: return "OPTIONS15";
    case D3D12_FEATURE_D3D12_OPTIONS16: return "OPTIONS16";
    case D3D12_FEATURE_D3D12_OPTIONS17: return "OPTIONS17";
    case D3D12_FEATURE_D3D12_OPTIONS18: return "OPTIONS18";
    case D3D12_FEATURE_D3D12_OPTIONS19: return "OPTIONS19";
    case D3D12_FEATURE_D3D12_OPTIONS20: return "OPTIONS20";
    case D3D12_FEATURE_PREDICATION: return "PREDICATION";
    case D3D12_FEATURE_PLACED_RESOURCE_SUPPORT_INFO: return "PLACED_RESOURCE_INFO";
    case D3D12_FEATURE_HARDWARE_COPY: return "HARDWARE_COPY";
    case D3D12_FEATURE_D3D12_OPTIONS21: return "OPTIONS21";
    default: return "UNKNOWN";
    }
}

static BOOL CALLBACK InitOriginalD3D12(PINIT_ONCE, PVOID, PVOID*) {
    // Sem std::string aqui: excecao atraves de callback C e indefinida.
    // Path absoluto: nome puro devolveria a propria proxy ja carregada (recursao).
    WCHAR sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return FALSE;
    }
    static const WCHAR kSuffix[] = L"\\d3d12.dll";
    size_t baseLen = wcslen(sys);
    if (baseLen + 11 >= MAX_PATH) {
        return FALSE;
    }
    WCHAR full[MAX_PATH] = {};
    memcpy(full, sys, baseLen * sizeof(WCHAR));
    memcpy(full + baseLen, kSuffix, sizeof(kSuffix));
    g_originalD3D12 = LoadLibraryExW(full, nullptr, 0);
    if (!g_originalD3D12) {
        return FALSE;
    }

    g_originalD3D12CreateDevice = reinterpret_cast<D3D12CreateDevice_t>(
        GetProcAddress(g_originalD3D12, "D3D12CreateDevice"));
    if (!g_originalD3D12CreateDevice) {
        FreeLibrary(g_originalD3D12);
        g_originalD3D12 = nullptr;
        return FALSE;
    }

    // Repasse resolvido uma vez: stubs ASM viram so jmp (sem GetProcAddress por chamada).
    static const char* kNames[17] = {
        "SetAppCompatStringPointer", "D3D12GetDebugInterface",
        "D3D12CoreCreateLayeredDevice", "D3D12CoreGetLayeredDeviceSize",
        "D3D12CoreRegisterLayers", "D3D12CreateRootSignatureDeserializer",
        "D3D12CreateVersionedRootSignatureDeserializer",
        "D3D12DeviceRemovedExtendedData", "D3D12EnableExperimentalFeatures",
        "D3D12GetInterface", "D3D12PIXEventsReplaceBlock",
        "D3D12PIXGetThreadInfo", "D3D12PIXNotifyWakeFromFenceSignal",
        "D3D12PIXReportCounter", "D3D12SerializeRootSignature",
        "D3D12SerializeVersionedRootSignature", "GetBehaviorValue",
    };
    // true = retorna HRESULT (fallback E_NOTIMPL); false = ponteiro/void/UINT64 (fallback 0).
    static const bool kIsHResult[17] = {
        false, true, true, true, true, true, true, true, true, true,
        false, false, false, false, true, true, true,
    };
    for (size_t i = 0; i < 17; ++i) {
        FARPROC p = GetProcAddress(g_originalD3D12, kNames[i]);
        if (p) {
            g_procs[i] = reinterpret_cast<void*>(p);
        } else if (kIsHResult[i]) {
            g_procs[i] = reinterpret_cast<void*>(&SpiderFixNotImpl);
        } else {
            g_procs[i] = reinterpret_cast<void*>(&SpiderFixZero);
        }
    }
    FARPROC ord99 = GetProcAddress(g_originalD3D12,
        reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(99)));
    g_procs[17] = ord99 ? reinterpret_cast<void*>(ord99)
                        : reinterpret_cast<void*>(&SpiderFixZero);
    return TRUE;
}

static bool LoadOriginalD3D12() {
    PVOID ctx = nullptr;
    if (!InitOnceExecuteOnce(&g_d3d12InitOnce, InitOriginalD3D12, nullptr, &ctx)) {
        Log("LoadLibraryExW falhou para d3d12.dll original");
        return false;
    }
    if (!g_originalD3D12CreateDevice) {
        Log("GetProcAddress falhou para D3D12CreateDevice original");
        return false;
    }
    return true;
}

// Slow path dos stubs ASM: jogo chamou export antes de D3D12CreateDevice.
// idx chega em ecx; devolve o proc (tabela ja vem com fallback, nunca nulo).
extern "C" FARPROC SpiderFixResolveProc(int idx) {
    if (idx < 0 || idx >= 18) {
        return reinterpret_cast<FARPROC>(&SpiderFixZero);
    }
    if (!LoadOriginalD3D12()) {
        return reinterpret_cast<FARPROC>(&SpiderFixZero);
    }
    void* p = g_procs[idx];
    if (!p) {
        p = reinterpret_cast<void*>(&SpiderFixZero);
    }
    return reinterpret_cast<FARPROC>(p);
}

// Resolve originais pela vtable do proprio objeto (leitura compartilhada barata).
static void LookupOriginals(ID3D12Device* self,
    CheckFeatureSupport_t* checkOut, CreateGraphicsPipelineState_t* psoOut) {
    *checkOut = nullptr;
    *psoOut = nullptr;
    if (!self) {
        return;
    }
    void* vt = *reinterpret_cast<void***>(self);
    AcquireSRWLockShared(&g_devicePatchLock);
    for (size_t i = 0; i < g_vhookCount; ++i) {
        if (g_vhooks[i].vtable == vt) {
            *checkOut = g_vhooks[i].check;
            *psoOut = g_vhooks[i].pso;
            break;
        }
    }
    ReleaseSRWLockShared(&g_devicePatchLock);
}

static HRESULT STDMETHODCALLTYPE HookedCheckFeatureSupport(
    ID3D12Device* self,
    D3D12_FEATURE feature,
    void* pFeatureSupportData,
    UINT featureSupportDataSize
) {
    CheckFeatureSupport_t original = nullptr;
    CreateGraphicsPipelineState_t ignored = nullptr;
    LookupOriginals(self, &original, &ignored);
    if (!original) {
        return E_FAIL;
    }

    HRESULT hr = original(self, feature, pFeatureSupportData, featureSupportDataSize);

    bool verbose = TakeBudget(&g_featureLogBudget);
    if (verbose) {
        char buf[256] {};
        std::snprintf(
            buf,
            sizeof(buf),
            "CheckFeatureSupport %s (%u): HRESULT 0x%08lX",
            FeatureName(feature),
            static_cast<unsigned>(feature),
            static_cast<unsigned long>(hr)
        );
        Log(buf);
    }

    if (!pFeatureSupportData) {
        return hr;
    }

    if (feature == D3D12_FEATURE_FEATURE_LEVELS &&
        featureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) {
        auto* levels = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(pFeatureSupportData);
        if (!levels->pFeatureLevelsRequested || levels->NumFeatureLevels == 0) {
            return hr; // Erro do chamador: nao inventa dado.
        }
        bool wants12_0 = false;
        for (UINT i = 0; i < levels->NumFeatureLevels; ++i) {
            if (levels->pFeatureLevelsRequested[i] == D3D_FEATURE_LEVEL_12_0) {
                wants12_0 = true;
                break;
            }
        }
        if (SUCCEEDED(hr)) {
            if (levels->MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0) {
                return hr; // Nativo ja serve: nao rebaixa 12_1/12_2.
            }
            if (wants12_0) {
                levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
                if (verbose) {
                    Log("Spoof: FEATURE_LEVELS -> 12_0");
                }
                return S_OK;
            }
            return hr;
        }
        if (hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED) && wants12_0) {
            levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
            if (verbose) {
                Log("Spoof: FEATURE_LEVELS UNSUPPORTED -> 12_0");
            }
            return S_OK;
        }
    }

    return hr;
}

static const char* TopologyTypeName(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    switch (type) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED: return "UNDEFINED";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: return "POINT";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: return "LINE";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: return "TRIANGLE";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH: return "PATCH";
    default: return "unknown";
    }
}

static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(
    ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** ppPipelineState
) {
    CheckFeatureSupport_t ignored = nullptr;
    CreateGraphicsPipelineState_t original = nullptr;
    LookupOriginals(self, &ignored, &original);
    if (!original) {
        return E_FAIL;
    }

    HRESULT hr = original(self, desc, riid, ppPipelineState);
    // Retry depth-only sem PS mantem jogo vivo (sem ele: E_INVALIDARG ->
    // device hung 0x887A0006). Desligue com SPIDERFIX_DEPTH_RETRY=0.
    if (g_depthRetry == 1 && hr == E_INVALIDARG &&
        desc &&
        desc->NumRenderTargets == 0 &&
        desc->DSVFormat != DXGI_FORMAT_UNKNOWN &&
        desc->PS.pShaderBytecode &&
        desc->PS.BytecodeLength > 0) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC patchedDesc = *desc;
        patchedDesc.PS = {};

        HRESULT retryHr = original(self, &patchedDesc, riid, ppPipelineState);
        if (TakeBudget(&g_psoFailureLogBudget)) {
            char retryBuf[256] {};
            std::snprintf(
                retryBuf,
                sizeof(retryBuf),
                "Depth-only PSO retry without PS: original 0x%08lX retry 0x%08lX",
                static_cast<unsigned long>(hr),
                static_cast<unsigned long>(retryHr));
            Log(retryBuf);
            char descBuf[512] {};
            std::snprintf(
                descBuf,
                sizeof(descBuf),
                "Retried PSO topo=%s RTVs=%u DSV=0x%X Sample=%u/%u IBStrip=0x%X VS=%zu PS=%zu(hash 0x%08X) DS=%zu HS=%zu GS=%zu",
                TopologyTypeName(desc->PrimitiveTopologyType),
                static_cast<unsigned>(desc->NumRenderTargets),
                static_cast<unsigned>(desc->DSVFormat),
                static_cast<unsigned>(desc->SampleDesc.Count),
                static_cast<unsigned>(desc->SampleDesc.Quality),
                static_cast<unsigned>(desc->IBStripCutValue),
                desc->VS.BytecodeLength,
                desc->PS.BytecodeLength,
                Fnv1a(desc->PS.pShaderBytecode, desc->PS.BytecodeLength),
                desc->DS.BytecodeLength,
                desc->HS.BytecodeLength,
                desc->GS.BytecodeLength);
            Log(descBuf);
        }

        if (SUCCEEDED(retryHr)) {
            return retryHr;
        }
    }

    if (FAILED(hr) && TakeBudget(&g_psoFailureLogBudget)) {
        char buf[512] {};
        if (desc) {
            std::snprintf(
                buf,
                sizeof(buf),
                "CreateGraphicsPipelineState failed 0x%08lX topo=%s RTVs=%u DSV=0x%X Sample=%u/%u IBStrip=0x%X VS=%zu PS=%zu DS=%zu HS=%zu GS=%zu BlendAlpha=%u Depth=%u Stencil=%u Conservative=%u",
                static_cast<unsigned long>(hr),
                TopologyTypeName(desc->PrimitiveTopologyType),
                static_cast<unsigned>(desc->NumRenderTargets),
                static_cast<unsigned>(desc->DSVFormat),
                static_cast<unsigned>(desc->SampleDesc.Count),
                static_cast<unsigned>(desc->SampleDesc.Quality),
                static_cast<unsigned>(desc->IBStripCutValue),
                desc->VS.BytecodeLength,
                desc->PS.BytecodeLength,
                desc->DS.BytecodeLength,
                desc->HS.BytecodeLength,
                desc->GS.BytecodeLength,
                static_cast<unsigned>(desc->BlendState.AlphaToCoverageEnable),
                static_cast<unsigned>(desc->DepthStencilState.DepthEnable),
                static_cast<unsigned>(desc->DepthStencilState.StencilEnable),
                static_cast<unsigned>(desc->RasterizerState.ConservativeRaster));
        } else {
            std::snprintf(
                buf,
                sizeof(buf),
                "CreateGraphicsPipelineState failed 0x%08lX with null desc",
                static_cast<unsigned long>(hr));
        }
        Log(buf);
    }

    return hr;
}

static void PatchDeviceMethods(IUnknown* deviceUnknown) {
    if (!deviceUnknown) {
        return;
    }

    ID3D12Device* device = nullptr;
    HRESULT hr = deviceUnknown->QueryInterface(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        Log("Nao foi possivel obter ID3D12Device para patchar CheckFeatureSupport");
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    MEMORY_BASIC_INFORMATION mbi {};
    SIZE_T need = sizeof(void*) * (kCheckFeatureSupportVtableIndex + 1);
    if (!vtable || VirtualQuery(vtable, &mbi, sizeof(mbi)) < sizeof(mbi) ||
        mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS ||
        (reinterpret_cast<char*>(vtable) + need >
         static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize)) {
        Log("Vtable ID3D12Device ilegivel, patch abortado");
        device->Release();
        return;
    }
    if (vtable[kCheckFeatureSupportVtableIndex] ==
            reinterpret_cast<void*>(&HookedCheckFeatureSupport) &&
        vtable[kCreateGraphicsPipelineStateVtableIndex] ==
            reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState)) {
        device->Release();
        return; // Ja hookado por outro device da mesma vtable.
    }

    bool patched = false;
    bool tableFull = false;
    bool protFail = false;
    AcquireSRWLockExclusive(&g_devicePatchLock);
    size_t slot = kMaxVtableHooks;
    for (size_t i = 0; i < g_vhookCount; ++i) {
        if (g_vhooks[i].vtable == vtable) {
            slot = i;
            break;
        }
    }
    if (slot != kMaxVtableHooks) {
        patched = true; // Outra thread hookou entre a checagem e o lock.
    } else if (g_vhookCount >= kMaxVtableHooks) {
        tableFull = true;
    } else {
        CheckFeatureSupport_t origCheck =
            reinterpret_cast<CheckFeatureSupport_t>(vtable[kCheckFeatureSupportVtableIndex]);
        CreateGraphicsPipelineState_t origPso =
            reinterpret_cast<CreateGraphicsPipelineState_t>(vtable[kCreateGraphicsPipelineStateVtableIndex]);
        DWORD oldProtect = 0;
        SIZE_T range = sizeof(void*) *
            (kCheckFeatureSupportVtableIndex - kCreateGraphicsPipelineStateVtableIndex + 1);
        if (VirtualProtect(&vtable[kCreateGraphicsPipelineStateVtableIndex],
                range, PAGE_READWRITE, &oldProtect)) {
            vtable[kCreateGraphicsPipelineStateVtableIndex] =
                reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState);
            vtable[kCheckFeatureSupportVtableIndex] =
                reinterpret_cast<void*>(&HookedCheckFeatureSupport);
            DWORD ignored = 0;
            VirtualProtect(&vtable[kCreateGraphicsPipelineStateVtableIndex],
                range, oldProtect, &ignored);
            g_vhooks[g_vhookCount].vtable = vtable;
            g_vhooks[g_vhookCount].check = origCheck;
            g_vhooks[g_vhookCount].pso = origPso;
            ++g_vhookCount;
            patched = true;
            HMODULE pinned = nullptr; // Fixa modulo: vtable apontaria p/ codigo descarregado.
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&PatchDeviceMethods), &pinned);
        } else {
            protFail = true;
        }
    }
    ReleaseSRWLockExclusive(&g_devicePatchLock);
    device->Release();

    if (patched) {
        Log("Patch aplicado: ID3D12Device::CreateGraphicsPipelineState + CheckFeatureSupport");
    } else if (tableFull) {
        Log("Tabela de vtables cheia, patch ignorado");
    } else if (protFail) {
        Log("VirtualProtect falhou ao patchar metodos do ID3D12Device");
    }
}

extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel,
    REFIID riid,
    void** ppDevice
) {
    EnsureConfig();
    static LONG s_firstLog = 0;
    if (InterlockedCompareExchange(&s_firstLog, 1, 0) == 0) {
        Log("=== d3d12.dll proxy carregado ===");
    }
    Log("=== Proxy D3D12CreateDevice interceptado ===");

    if (!LoadOriginalD3D12()) {
        if (ppDevice) {
            *ppDevice = nullptr;
        }
        return E_FAIL;
    }

    // Detecta GPU capaz uma vez: cria device temporario em 11_0 e le o nivel
    // nativo. 12_1+ ignora o fix (passthrough puro, sem hooks). 12_0 ou menos
    // segue o caminho do fix. SPIDERFIX_FORCE=1 forca o fix em qualquer GPU.
    static LONG s_force = -1;
    if (s_force == -1) {
        char v[8] {};
        DWORD n = GetEnvironmentVariableA("SPIDERFIX_FORCE", v, sizeof(v));
        InterlockedExchange(&s_force,
            (n > 0 && (v[0] == '1' || v[0] == 'Y' || v[0] == 'y')) ? 1 : 0);
    }
    if (s_force == 0) {
        void* tmp = nullptr;
        HRESULT tmpHr = g_originalD3D12CreateDevice(pAdapter,
            D3D_FEATURE_LEVEL_11_0, riid, &tmp);
        if (SUCCEEDED(tmpHr) && tmp) {
            IUnknown* unk = static_cast<IUnknown*>(tmp);
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
                static const D3D_FEATURE_LEVEL kProbe[] = {
                    D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                    D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                    D3D_FEATURE_LEVEL_11_0,
                };
                D3D12_FEATURE_DATA_FEATURE_LEVELS q {};
                q.NumFeatureLevels = 5;
                q.pFeatureLevelsRequested = kProbe;
                if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                        &q, sizeof(q))) &&
                    q.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_1) {
                    char cap[128] {};
                    std::snprintf(cap, sizeof(cap),
                        "GPU nativa %s: bypass, fix ignorado",
                        FeatureLevelName(q.MaxSupportedFeatureLevel));
                    Log(cap);
                    dev->Release();
                    unk->Release();
                    return g_originalD3D12CreateDevice(
                        pAdapter, minimumFeatureLevel, riid, ppDevice);
                }
                dev->Release();
            }
            unk->Release();
        }
    }

    EnsureMsgboxPatch();

    const D3D_FEATURE_LEVEL tryLevels[] = {
        D3D_FEATURE_LEVEL_12_0,
        minimumFeatureLevel,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    // Sonda de suporte (ppDevice nulo): mesmo loop, S_FALSE se algum nivel passa.
    if (!ppDevice) {
        HRESULT probeHr = E_FAIL;
        D3D_FEATURE_LEVEL tried[4] = {};
        size_t triedCount = 0;
        for (D3D_FEATURE_LEVEL level : tryLevels) {
            bool dup = false;
            for (size_t i = 0; i < triedCount; ++i) {
                if (tried[i] == level) { dup = true; break; }
            }
            if (dup) {
                continue;
            }
            tried[triedCount++] = level;
            probeHr = g_originalD3D12CreateDevice(pAdapter, level, riid, nullptr);
            if (SUCCEEDED(probeHr)) {
                return S_FALSE;
            }
            if (probeHr != static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED) &&
                probeHr != E_INVALIDARG) {
                return probeHr;
            }
        }
        return probeHr;
    }

    D3D_FEATURE_LEVEL tried[4] = {};
    size_t triedCount = 0;
    HRESULT lastHr = E_FAIL;

    for (D3D_FEATURE_LEVEL level : tryLevels) {
        bool dup = false;
        for (size_t i = 0; i < triedCount; ++i) {
            if (tried[i] == level) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        tried[triedCount++] = level;

        *ppDevice = nullptr;
        lastHr = g_originalD3D12CreateDevice(pAdapter, level, riid, ppDevice);

        char buf[256] {};
        std::snprintf(
            buf,
            sizeof(buf),
            "Tentativa D3D12CreateDevice FL %s: HRESULT 0x%08lX",
            FeatureLevelName(level),
            static_cast<unsigned long>(lastHr)
        );
        Log(buf);

        if (SUCCEEDED(lastHr)) {
            Log("D3D12CreateDevice liberado pelo proxy");
            PatchDeviceMethods(reinterpret_cast<IUnknown*>(*ppDevice));
            return lastHr;
        }
        // Nao esconde erro real: so insiste em nivel nao suportado ou arg invalido.
        if (lastHr != static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED) &&
            lastHr != E_INVALIDARG) {
            *ppDevice = nullptr;
            return lastHr;
        }
    }

    *ppDevice = nullptr;
    Log("FALHA TOTAL - nenhum feature level funcionou");
    return lastHr;
}

extern "C" DWORD WINAPI SpiderFixVersion() {
    return SPIDERFIX_VERSION;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
