#include <windows.h>
#include <d3d12.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <atomic>
#include <new>
#include <dxgi.h>

// SpiderFix - proxy d3d12.dll para Marvel's Spider-Man 2 em GPU sem FL 12_1.
// Funcao principal: fallback de feature level em D3D12CreateDevice, spoof de
// FEATURE_LEVELS para 12_0, retry de PSO depth-only, supressao do aviso de GPU.
//
// Limitacoes conhecidas (nao cobertas, sem quebrar o jogo):
// - Devices criados via D3D12GetInterface/DeviceFactory nao passam pelo fallback.
// - IAT patch cobre user32.dll e API-set dialogbox do exe (MessageBoxA/W/ExA/ExW).
//   Delay-load, outras DLLs, MessageBoxIndirect e TaskDialog nao sao cobertos.
// - Aviso emitido antes do primeiro D3D12CreateDevice nao e suprimido (patch
//   em DllMain exigiria trabalho sob loader lock).
// - Nao finge D3D12_OPTIONS (ROV, conservative raster): mentir ali faria o
//   jogo usar recurso que a GPU nao tem e travar.
// - Retry remove o PS. Na RX 580 o driver recusa (E_INVALIDARG) PSO depth-only
//   cujo PS usa ROV (RasterizerOrdered*); clip/discard passa normal. Logo o
//   que se perde sao as escritas ROV desse passe. Jogo vivo > pixel perfeito.
//   Nao restringimos o retry a PS com ROV: detectar exige refletir DXBC/DXIL
//   (dxcompiler) e um falso negativo traria de volta o device hung.

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED 0x887A0004L
#endif

#define SPIDERFIX_VERSION 0x00020100u // v2.1.0

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
using CreatePipelineState_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device2*,
    const D3D12_PIPELINE_STATE_STREAM_DESC*,
    REFIID,
    void**);
using CreatePipelineLibrary_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    const void*,
    SIZE_T,
    REFIID,
    void**);

// Indices na vtable contados na declaracao do SDK (d3d12.h): 10
// CreateGraphicsPipelineState e 13 CheckFeatureSupport (ID3D12Device, apos
// IUnknown 0-2 + ID3D12Object 3-6); 44 CreatePipelineLibrary
// (ID3D12Device1); 47 CreatePipelineState (ID3D12Device2, stream desc).
static constexpr size_t kCreateGraphicsPipelineStateVtableIndex = 10;
static constexpr size_t kCheckFeatureSupportVtableIndex = 13;
static constexpr size_t kCreatePipelineLibraryVtableIndex = 44;
static constexpr size_t kCreatePipelineStateVtableIndex = 47;
static constexpr size_t kMaxVtableHooks = 32;
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
    CreatePipelineState_t psoStream;
    CreatePipelineLibrary_t libCreate;
};
// Escrita so sob g_devicePatchLock (exclusivo); leitura sem lock: a entrada e
// preenchida antes de publicar o contador (release) e nunca muda depois.
static VtableHook g_vhooks[kMaxVtableHooks] = {};
static std::atomic<size_t> g_vhookCount{0};

// Tabela de repasse dos exports (ordem sincronizada com D3D12ProxyStubs.asm).
// Preenchida uma vez no InitOriginalD3D12; entradas ausentes ganham fallback
// local, entao o stub vira so jmp (preserva XMM0-3, sem unwind, sem chamada).
// D3D12DeviceRemovedExtendedData nao entra: na DLL real e export de DADOS.
static constexpr size_t kProcCount = 17; // 16 por nome + ordinal 99
extern "C" void* g_procs[kProcCount] = {};

static const char* const kProcNames[kProcCount - 1] = {
    "SetAppCompatStringPointer", "D3D12GetDebugInterface",
    "D3D12CoreCreateLayeredDevice", "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterLayers", "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12EnableExperimentalFeatures", "D3D12GetInterface",
    "D3D12PIXEventsReplaceBlock", "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal", "D3D12PIXReportCounter",
    "D3D12SerializeRootSignature", "D3D12SerializeVersionedRootSignature",
    "GetBehaviorValue",
};
// true = retorna HRESULT (fallback E_NOTIMPL); false = ponteiro/void/SIZE_T/UINT64 (fallback 0).
static const bool kProcIsHResult[kProcCount] = {
    false, true, true, false, true, true, true, true, true,
    false, false, false, false, true, true, true,
    false, // ordinal 99
};

static HRESULT WINAPI SpiderFixNotImpl() { return E_NOTIMPL; }
static void* WINAPI SpiderFixZero() { return nullptr; }

static void* ProcFallback(size_t idx) {
    return kProcIsHResult[idx] ? reinterpret_cast<void*>(&SpiderFixNotImpl)
                               : reinterpret_cast<void*>(&SpiderFixZero);
}

alignas(64) static LONG g_featureLogBudget = 80;
alignas(64) static LONG g_psoFailureLogBudget = 80;

static LONG g_logDisabled = 0; // 1 = log desligado
static LONG g_logLevel = 1;    // 0 = off, 1 = com orcamento, 2 = sem limite
static LONG g_depthRetry = 1;  // 0 = SPIDERFIX_DEPTH_RETRY=0
static LONG g_hookLib = 1;     // 0 = sem wrapper de PipelineLibrary
static LONG g_showOsd = 1;     // 0 = sem aviso visual
static LONG g_force = 0;       // 1 = fix em qualquer GPU (SPIDERFIX_FORCE / ForceActive)
static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static ULONGLONG g_logBytes = 0;
static LONG g_logFull = 0;
static wchar_t g_logPath[MAX_PATH] = {};
// Avisos do parser do ini: Log() dentro do InitOnce da config reentraria no
// InitOnce (deadlock), entao ficam aqui e saem no primeiro Log seguinte.
static char g_configWarn[256] = {};
static LONG g_configWarnLogged = 0;

// "<pasta da proxy>\name". Falha (sem fallback relativo ao CWD) se nao couber
// em cap: GetModuleFileNameW trunca e ainda devolve sucesso.
static bool PathNextToSelf(const wchar_t* name, wchar_t* out, size_t cap) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PathNextToSelf), &self) || !self) {
        return false;
    }
    DWORD n = GetModuleFileNameW(self, out, static_cast<DWORD>(cap));
    if (n == 0 || n >= cap) {
        out[0] = L'\0';
        return false;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    size_t dirLen = slash ? static_cast<size_t>(slash - out + 1) : 0;
    size_t nameLen = wcslen(name);
    if (!dirLen || dirLen + nameLen + 1 > cap) {
        out[0] = L'\0';
        return false;
    }
    memcpy(out + dirLen, name, (nameLen + 1) * sizeof(wchar_t));
    return true;
}

// 0/1/2, y/n, yes/no, true/false, on/off. -1 = invalido.
static int ParseBoolValue(const char* value) {
    if (value[0] >= '0' && value[0] <= '2' && value[1] == '\0') {
        return value[0] - '0';
    }
    static const char* const kOn[] = { "y", "yes", "true", "on" };
    static const char* const kOff[] = { "n", "no", "false", "off" };
    for (const char* s : kOn) {
        if (!_stricmp(value, s)) {
            return 1;
        }
    }
    for (const char* s : kOff) {
        if (!_stricmp(value, s)) {
            return 0;
        }
    }
    return -1;
}

static void AddConfigWarn(const char* what, const char* key) {
    size_t used = strlen(g_configWarn);
    if (used + 4 < sizeof(g_configWarn)) {
        std::snprintf(g_configWarn + used, sizeof(g_configWarn) - used,
            "%s%s '%s'", used ? "; " : "", what, key);
    }
}

// Valores lidos do ini; -1 = chave ausente. Aplicados depois do parse para
// que LogLevel venca Log independente da ordem das linhas.
struct IniValues {
    int log = -1;
    int logLevel = -1;
    int force = -1;
    int depthRetry = -1;
    int hookLib = -1;
    int osd = -1;
};

static void ApplyIniValue(IniValues* ini, const char* key, const char* value) {
    struct Key {
        const char* name;
        int IniValues::* field;
    };
    static const Key kKeys[] = {
        { "Log", &IniValues::log },
        { "LogLevel", &IniValues::logLevel },
        { "ForceActive", &IniValues::force },
        { "DepthRetry", &IniValues::depthRetry },
        { "HookPipelineLibrary", &IniValues::hookLib },
        { "ShowOSD", &IniValues::osd },
    };
    for (const Key& k : kKeys) {
        if (!_stricmp(key, k.name)) {
            int v = ParseBoolValue(value);
            if (v < 0) {
                AddConfigWarn("valor invalido em", key);
            } else {
                ini->*k.field = v;
            }
            return;
        }
    }
    AddConfigWarn("chave desconhecida", key);
}

static bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// spiderfix.ini ao lado da DLL (Log, LogLevel 0|1|2, ForceActive, DepthRetry,
// HookPipelineLibrary, ShowOSD). Chave presente vence a variavel de ambiente.
// Aceita ANSI/UTF-8 (com ou sem BOM), UTF-16 LE com BOM, comentario inline.
static void LoadIniFile(IniValues* ini) {
    wchar_t path[MAX_PATH] = {};
    if (!PathNextToSelf(L"spiderfix.ini", path, MAX_PATH)) {
        return;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    // static: InitOnce garante uma unica thread aqui; evita 48KB de pilha.
    static char raw[16384];
    static char utf8[sizeof(raw) * 2];
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw, sizeof(raw) - 2, &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        return;
    }
    raw[read] = '\0';
    raw[read + 1] = '\0';
    if (read == sizeof(raw) - 2) {
        AddConfigWarn("ini maior que 16KB, truncado em", "spiderfix.ini");
    }
    const unsigned char* u = reinterpret_cast<const unsigned char*>(raw);
    char* text = raw;
    if (read >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        int n = WideCharToMultiByte(CP_UTF8, 0,
            reinterpret_cast<const wchar_t*>(raw + 2), static_cast<int>((read - 2) / 2),
            utf8, static_cast<int>(sizeof(utf8) - 1), nullptr, nullptr);
        utf8[n > 0 ? n : 0] = '\0';
        text = utf8;
    } else if (read >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        text = raw + 3;
    }
    char* line = text;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next = '\0';
        }
        while (IsBlank(*line)) {
            ++line;
        }
        if (*line && *line != '#' && *line != ';' && *line != '[') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char* key = line;
                char* value = eq + 1;
                char* comment = strpbrk(value, "#;");
                if (comment) {
                    *comment = '\0';
                }
                while (*value == ' ' || *value == '\t') {
                    ++value;
                }
                char* kend = key + strlen(key);
                while (kend > key && IsBlank(kend[-1])) {
                    *--kend = '\0';
                }
                char* vend = value + strlen(value);
                while (vend > value && IsBlank(vend[-1])) {
                    *--vend = '\0';
                }
                if (*key) {
                    ApplyIniValue(ini, key, value);
                }
            }
        }
        line = next ? next + 1 : nullptr;
    }
}

static int EnvBool(const char* name) {
    char v[16] {};
    DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return -1;
    }
    return ParseBoolValue(v);
}

static BOOL CALLBACK InitConfig(PINIT_ONCE, PVOID, PVOID*) {
    g_logDisabled = (EnvBool("SPIDERFIX_LOG") == 0) ? 1 : 0;
    g_depthRetry = (EnvBool("SPIDERFIX_DEPTH_RETRY") == 0) ? 0 : 1;
    g_hookLib = (EnvBool("SPIDERFIX_HOOK_LIB") == 0) ? 0 : 1;
    g_showOsd = (EnvBool("SPIDERFIX_OSD") == 0) ? 0 : 1;
    g_force = (EnvBool("SPIDERFIX_FORCE") > 0) ? 1 : 0;

    IniValues ini;
    LoadIniFile(&ini);
    if (ini.logLevel >= 0) {
        g_logLevel = ini.logLevel;
        g_logDisabled = (ini.logLevel == 0) ? 1 : 0;
    } else if (ini.log >= 0) {
        g_logDisabled = (ini.log == 0) ? 1 : 0;
    }
    if (ini.force >= 0) {
        g_force = ini.force ? 1 : 0;
    }
    if (ini.depthRetry >= 0) {
        g_depthRetry = ini.depthRetry ? 1 : 0;
    }
    if (ini.hookLib >= 0) {
        g_hookLib = ini.hookLib ? 1 : 0;
    }
    if (ini.osd >= 0) {
        g_showOsd = ini.osd ? 1 : 0;
    }
    return TRUE;
}

static void Log(const char* msg);

static void EnsureConfig() {
    PVOID ctx = nullptr;
    InitOnceExecuteOnce(&g_configOnce, InitConfig, nullptr, &ctx);
    if (g_configWarn[0] && InterlockedCompareExchange(&g_configWarnLogged, 1, 0) == 0) {
        char buf[320] {};
        std::snprintf(buf, sizeof(buf), "spiderfix.ini: %s", g_configWarn);
        Log(buf);
    }
}

// Move o log para .old e abre um novo. Se o move falhar (arquivo preso por
// outro processo), para de logar em vez de truncar o conteudo atual.
static void RotateLogLocked() {
    if (g_hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hLog);
        g_hLog = INVALID_HANDLE_VALUE;
    }
    wchar_t oldp[MAX_PATH] = {};
    size_t len = wcslen(g_logPath);
    static const wchar_t kOld[] = L".old";
    if (len + (sizeof(kOld) / sizeof(kOld[0])) > MAX_PATH) {
        g_logFull = 1;
        return;
    }
    memcpy(oldp, g_logPath, len * sizeof(wchar_t));
    memcpy(oldp + len, kOld, sizeof(kOld));
    if (!MoveFileExW(g_logPath, oldp, MOVEFILE_REPLACE_EXISTING)) {
        g_logFull = 1;
        return;
    }
    g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_logBytes = 0;
    if (g_hLog == INVALID_HANDLE_VALUE) {
        g_logFull = 1;
    }
}

static void EnsureLogOpenLocked() {
    if (g_hLog != INVALID_HANDLE_VALUE || g_logFull) {
        return;
    }
    if (!g_logPath[0] && !PathNextToSelf(L"spiderfix.log", g_logPath, MAX_PATH)) {
        g_logFull = 1;
        return;
    }
    g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hLog == INVALID_HANDLE_VALUE) {
        g_logFull = 1;
        return;
    }
    LARGE_INTEGER sz {};
    g_logBytes = GetFileSizeEx(g_hLog, &sz) ? static_cast<ULONGLONG>(sz.QuadPart) : 0;
    if (g_logBytes > kMaxLogBytes) {
        RotateLogLocked(); // Rotacao real: acima de 512KB vai para .old.
    }
}

// Reentrada na mesma thread (ex.: excecao dentro do Log e um handler chamando
// MessageBox, que tambem loga) travaria no SRW nao recursivo: descarta a linha.
static thread_local bool t_inLog = false;

static void Log(const char* msg) {
    EnsureConfig();
    if (g_logDisabled || !msg || t_inLog) {
        return;
    }
    t_inLog = true;
    AcquireSRWLockExclusive(&g_logLock);
    EnsureLogOpenLocked();
    if (g_logFull || g_hLog == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_logLock);
        t_inLog = false;
        return;
    }
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char line[2048] {};
    int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u - %s\r\n",
        static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond), msg);
    if (n > 0) {
        size_t len = static_cast<size_t>(n);
        if (len >= sizeof(line)) {
            // Truncado sem quebrar o delimitador de linha.
            line[sizeof(line) - 5] = '.';
            line[sizeof(line) - 4] = '.';
            line[sizeof(line) - 3] = '\r';
            line[sizeof(line) - 2] = '\n';
            line[sizeof(line) - 1] = '\0';
            len = sizeof(line) - 1;
        }
        DWORD written = 0;
        if (WriteFile(g_hLog, line, static_cast<DWORD>(len), &written, nullptr)) {
            g_logBytes += written;
        }
        if (g_logBytes > kMaxLogBytes) {
            RotateLogLocked(); // Ciclica: sessoes longas continuam logando.
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
    t_inLog = false;
}

// So decrementa com orcamento positivo: falhas tardias continuam visiveis.
// Nivel 2 registra tudo sem gastar. Leitura simples antes do atomico: depois
// de esgotado, o caminho quente nao paga operacao com lock.
static bool TakeBudget(LONG* budget) {
    if (g_logLevel >= 2) {
        return true;
    }
    if (*static_cast<volatile LONG*>(budget) <= 0) {
        return false;
    }
    return InterlockedDecrement(budget) >= 0;
}

// Primeira ocorrencia por id (0..127), em qualquer nivel de log: o jogo
// consulta a mesma feature dezenas de vezes por segundo (ruido que esgotava o
// orcamento e, no nivel 2, serializava as threads no log).
static LONG64 g_seenFeatures[2] = {};
// Ids livres no fim da faixa (D3D12_FEATURE real < 64) para mensagens de spoof:
// a primeira de cada uma sai mesmo que a 1a consulta FEATURE_LEVELS nao spoofe.
static constexpr unsigned kLogIdSpoof = 126;
static constexpr unsigned kLogIdSpoofUnsupported = 127;

static bool FirstFeatureSeen(unsigned id) {
    if (id >= 128) {
        return TakeBudget(&g_featureLogBudget);
    }
    LONG64 bit = LONG64(1) << (id & 63);
    LONG64* word = &g_seenFeatures[id >> 6];
    if (*static_cast<volatile LONG64*>(word) & bit) {
        return false;
    }
    return (InterlockedOr64(word, bit) & bit) == 0;
}

// Retries de PSO: loga a primeira vez de cada hash de PS (tabela sem lock).
// Repeticoes so contam; a cada 100 retries sai um resumo. Tabela cheia cai no
// orcamento comum.
static constexpr size_t kSeenPsSlots = 512;
static LONG g_seenPs[kSeenPsSlots] = {};
static LONG g_retryCount = 0;

static bool FirstRetryOfPs(uint32_t hash) {
    uint32_t ukey = hash ? hash : 1u; // 0 = slot vazio
    LONG key = static_cast<LONG>(ukey);
    size_t i = ukey % kSeenPsSlots;
    for (size_t n = 0; n < kSeenPsSlots; ++n, i = (i + 1) % kSeenPsSlots) {
        LONG cur = *static_cast<volatile LONG*>(&g_seenPs[i]);
        if (cur == key) {
            return false;
        }
        if (cur == 0) {
            LONG prev = InterlockedCompareExchange(&g_seenPs[i], key, 0);
            if (prev == 0) {
                return true;
            }
            if (prev == key) {
                return false;
            }
        }
    }
    return TakeBudget(&g_psoFailureLogBudget);
}

static void Log(const char* msg);

static void CountRetry() {
    LONG n = InterlockedIncrement(&g_retryCount);
    if (n % 100 == 0) {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "Retries de PSO depth-only ate agora: %ld", n);
        Log(buf);
    }
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

// Chave do PS para dedupe/log: containers DXBC/DXIL trazem um digest de 16
// bytes no offset 4, entao a chave custa O(1) em vez de varrer 25-75KB.
static uint32_t PsKey(const D3D12_SHADER_BYTECODE& ps) {
    const unsigned char* b = static_cast<const unsigned char*>(ps.pShaderBytecode);
    if (!b || !ps.BytecodeLength) {
        return 0;
    }
    if (ps.BytecodeLength >= 20 && b[0] == 'D' && b[1] == 'X' && b[2] == 'B' && b[3] == 'C') {
        static const unsigned char kZero[16] = {};
        if (memcmp(b + 4, kZero, 16) != 0) { // DXIL sem validator vem com digest zerado
            return Fnv1a(b + 4, 16);
        }
    }
    return Fnv1a(b, ps.BytecodeLength);
}

// Copia do stream sem PS: pilha ate 4KB (stream valido maximo ~1,1KB), heap acima.
struct StreamCopy {
    alignas(8) unsigned char stack[4096];
    unsigned char* p;
    void* heap;
    SIZE_T cap;
    explicit StreamCopy(SIZE_T need) : p(stack), heap(nullptr), cap(sizeof(stack)) {
        if (need > sizeof(stack)) {
            heap = HeapAlloc(GetProcessHeap(), 0, need);
            p = static_cast<unsigned char*>(heap);
            cap = heap ? need : 0;
        }
    }
    ~StreamCopy() {
        if (heap) {
            HeapFree(GetProcessHeap(), 0, heap);
        }
    }
    StreamCopy(const StreamCopy&) = delete;
    StreamCopy& operator=(const StreamCopy&) = delete;
};

static void LogStreamCopyFailed(SIZE_T size) {
    static LONG s_once = 0;
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0) {
        char buf[128] {};
        std::snprintf(buf, sizeof(buf), "Stream PSO sem copia para retry (size=%zu): retry ignorado",
            static_cast<size_t>(size));
        Log(buf);
    }
}

static bool ContainsInsensitiveW(const wchar_t* text, const wchar_t* needle) {
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

template<size_t N>
static bool AnyW(LPCWSTR text, const wchar_t* const (&words)[N]) {
    for (const wchar_t* w : words) {
        if (ContainsInsensitiveW(text, w)) {
            return true;
        }
    }
    return false;
}

// O aviso de GPU do jogo sai na inicializacao: fora desta janela (contada a
// partir do patch da IAT, feito no primeiro D3D12CreateDevice) nada e suprimido.
static constexpr ULONGLONG kSuppressWindowMs = 3 * 60 * 1000;
static ULONGLONG g_msgboxPatchTick = 0;

// Suprime so o que parece aviso de requisito/compatibilidade de GPU e nao
// parece erro real (crash, device removed/hung): esses precisam aparecer.
static bool IsGpuCompatibilityWarning(LPCWSTR text, LPCWSTR caption) {
    static const wchar_t* const kGpu[] = {
        L"gpu", L"video", L"graphics", L"grafico", L"placa", L"driver", L"directx",
    };
    static const wchar_t* const kWarn[] = {
        L"compat", L"minimum", L"minimo", L"requisitos", L"suport", L"obsoleto",
    };
    static const wchar_t* const kRealError[] = {
        L"crash", L"removed", L"hung", L"lost", L"reset", L"0x887a", L"dxgi_error",
    };
    if (GetTickCount64() - g_msgboxPatchTick > kSuppressWindowMs) {
        return false;
    }
    if (AnyW(text, kRealError) || AnyW(caption, kRealError)) {
        return false;
    }
    return (text && AnyW(text, kGpu) && AnyW(text, kWarn)) ||
        (caption && AnyW(caption, kGpu) && AnyW(caption, kWarn));
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

// Decide e registra no log toda MessageBox que passa pelo hook (texto inteiro),
// suprimida ou nao: base para ajustar a heuristica por relato de usuario.
static bool ShouldSuppress(LPCWSTR text, LPCWSTR caption, UINT type) {
    bool suppress = IsGpuCompatibilityWarning(text, caption);
    char t[1200] {};
    char c[256] {};
    if (text) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, t, static_cast<int>(sizeof(t) - 1), nullptr, nullptr);
    }
    if (caption) {
        WideCharToMultiByte(CP_UTF8, 0, caption, -1, c, static_cast<int>(sizeof(c) - 1), nullptr, nullptr);
    }
    char buf[1600] {};
    std::snprintf(buf, sizeof(buf), "%s (type=0x%X) [%s]: %s",
        suppress ? "Aviso GPU suprimido" : "MessageBox repassado",
        static_cast<unsigned>(type), c, text ? t : "(null)");
    Log(buf);
    return suppress;
}

// Copia ANSI -> UTF-16 (CP_ACP) para uma unica checagem nas versoes A.
struct WideCopy {
    wchar_t buf[1024];
    const wchar_t* p;
    explicit WideCopy(LPCSTR s) : buf{}, p(nullptr) {
        if (s) {
            MultiByteToWideChar(CP_ACP, 0, s, -1, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0]) - 1));
            p = buf;
        }
    }
};

extern "C" int WINAPI HookedMessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    if (ShouldSuppress(WideCopy(text).p, WideCopy(caption).p, type)) {
        return SuppressedResult(type);
    }
    return g_originalMessageBoxA ?
        g_originalMessageBoxA(hwnd, text, caption, type) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) {
    if (ShouldSuppress(text, caption, type)) {
        return SuppressedResult(type);
    }
    return g_originalMessageBoxW ?
        g_originalMessageBoxW(hwnd, text, caption, type) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxExA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type, WORD lang) {
    if (ShouldSuppress(WideCopy(text).p, WideCopy(caption).p, type)) {
        return SuppressedResult(type);
    }
    return g_originalMessageBoxExA ?
        g_originalMessageBoxExA(hwnd, text, caption, type, lang) :
        SuppressedResult(type);
}

extern "C" int WINAPI HookedMessageBoxExW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) {
    if (ShouldSuppress(text, caption, type)) {
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
    g_msgboxPatchTick = GetTickCount64();
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

    // user32 direto ou via API-set (exe linkado contra OneCore/umbrella libs).
    static const char* const kModules[] = {
        "user32.dll",
        "ext-ms-win-ntuser-dialogbox-l1-1-0.dll",
        "ext-ms-win-ntuser-dialogbox-l1-1-1.dll",
    };
    for (const char* mod : kModules) {
        PatchImport(mod, "MessageBoxA", reinterpret_cast<void*>(&HookedMessageBoxA),
            reinterpret_cast<void**>(&g_originalMessageBoxA));
        PatchImport(mod, "MessageBoxW", reinterpret_cast<void*>(&HookedMessageBoxW),
            reinterpret_cast<void**>(&g_originalMessageBoxW));
        PatchImport(mod, "MessageBoxExA", reinterpret_cast<void*>(&HookedMessageBoxExA),
            reinterpret_cast<void**>(&g_originalMessageBoxExA));
        PatchImport(mod, "MessageBoxExW", reinterpret_cast<void*>(&HookedMessageBoxExW),
            reinterpret_cast<void**>(&g_originalMessageBoxExW));
    }
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
    for (size_t i = 0; i < kProcCount - 1; ++i) {
        FARPROC p = GetProcAddress(g_originalD3D12, kProcNames[i]);
        g_procs[i] = p ? reinterpret_cast<void*>(p) : ProcFallback(i);
    }
    FARPROC ord99 = GetProcAddress(g_originalD3D12,
        reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(99)));
    g_procs[kProcCount - 1] = ord99 ? reinterpret_cast<void*>(ord99)
                                    : ProcFallback(kProcCount - 1);
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

// Slow path dos stubs ASM (jogo chamou export antes de D3D12CreateDevice):
// garante g_procs sem entradas nulas. Se a DLL original nao carregar, cada
// slot vazio recebe o fallback do seu tipo (E_NOTIMPL p/ HRESULT, nunca S_OK).
extern "C" void SpiderFixEnsureProcs() {
    if (LoadOriginalD3D12()) {
        return;
    }
    for (size_t i = 0; i < kProcCount; ++i) {
        InterlockedCompareExchangePointer(&g_procs[i], ProcFallback(i), nullptr);
    }
}

// Sonda o adaptador que o jogo pediu (nao o padrao: notebook hibrido tem
// iGPU e dGPU com niveis diferentes). ppDevice nulo nao cria device: S_FALSE
// = suporta 12_1. Falha de sonda assume fix (comportamento seguro).
// Resultado guardado por LUID (nullptr = adaptador padrao): a sonda roda uma
// vez por adaptador, nao a cada D3D12CreateDevice.
struct AdapterProbe {
    bool isDefault;
    LUID luid;
    bool needsFix;
};
static AdapterProbe g_probes[8] = {};
static size_t g_probeCount = 0;
static SRWLOCK g_probeLock = SRWLOCK_INIT;

static bool AdapterNeedsFix(IUnknown* pAdapter) {
    AdapterProbe key {};
    key.isDefault = (pAdapter == nullptr);
    bool keyed = key.isDefault;
    if (pAdapter) {
        IDXGIAdapter* dxgi = nullptr;
        DXGI_ADAPTER_DESC desc {};
        if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIAdapter),
                reinterpret_cast<void**>(&dxgi))) && dxgi) {
            if (SUCCEEDED(dxgi->GetDesc(&desc))) {
                key.luid = desc.AdapterLuid;
                keyed = true;
            }
            dxgi->Release();
        }
    }
    if (keyed) {
        AcquireSRWLockShared(&g_probeLock);
        for (size_t i = 0; i < g_probeCount; ++i) {
            const AdapterProbe& p = g_probes[i];
            if (p.isDefault == key.isDefault && p.luid.LowPart == key.luid.LowPart &&
                p.luid.HighPart == key.luid.HighPart) {
                bool r = p.needsFix;
                ReleaseSRWLockShared(&g_probeLock);
                return r;
            }
        }
        ReleaseSRWLockShared(&g_probeLock);
    }
    HRESULT hr = g_originalD3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_1,
        __uuidof(ID3D12Device), nullptr);
    key.needsFix = FAILED(hr);
    // So guarda resposta definitiva; falha transitoria (memoria, device
    // removido) assume fix agora e sonda de novo na proxima chamada.
    bool definitive = SUCCEEDED(hr) || hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED) ||
        hr == E_INVALIDARG;
    if (keyed && definitive) {
        AcquireSRWLockExclusive(&g_probeLock);
        if (g_probeCount < sizeof(g_probes) / sizeof(g_probes[0])) {
            g_probes[g_probeCount++] = key;
        }
        ReleaseSRWLockExclusive(&g_probeLock);
    }
    return key.needsFix;
}

// Copia os originais da vtable do proprio objeto, sem lock (ver g_vhooks).
// Vtable desconhecida (outro hooker trocou o ponteiro de vtable do objeto por
// uma copia com os nossos ponteiros): acha a entrada pelos slots que nao
// hookamos (QueryInterface e GetNodeCount, 0 e 7), que a copia mantem. Com uma
// so entrada usa ela; sem casamento entre varias devolve vazio (E_FAIL) em vez
// de chamar o driver errado.
static VtableHook LookupOriginals(const void* self) {
    VtableHook found {};
    if (!self) {
        return found;
    }
    void* vt = *reinterpret_cast<void* const*>(self);
    size_t n = g_vhookCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (g_vhooks[i].vtable == vt) {
            return g_vhooks[i];
        }
    }
    void* const* cvt = static_cast<void* const*>(vt);
    for (size_t i = 0; i < n; ++i) {
        void* const* rvt = static_cast<void* const*>(g_vhooks[i].vtable);
        if (rvt[0] == cvt[0] && rvt[7] == cvt[7]) {
            found = g_vhooks[i];
            break;
        }
    }
    if (!found.vtable && n == 1) {
        found = g_vhooks[0];
    }
    static LONG s_warned = 0;
    if (n > 0 && InterlockedCompareExchange(&s_warned, 1, 0) == 0) {
        Log(found.vtable ?
            "Vtable desconhecida (outro hook trocou a tabela?): usando originais registrados" :
            "Vtable desconhecida sem correspondencia entre varios drivers: chamada recusada");
    }
    return found;
}

static HRESULT STDMETHODCALLTYPE HookedCheckFeatureSupport(
    ID3D12Device* self,
    D3D12_FEATURE feature,
    void* pFeatureSupportData,
    UINT featureSupportDataSize
) {
    CheckFeatureSupport_t original = LookupOriginals(self).check;
    if (!original) {
        return E_FAIL;
    }

    HRESULT hr = original(self, feature, pFeatureSupportData, featureSupportDataSize);

    // Uma linha por feature em qualquer nivel: no nivel 2 logar toda chamada
    // serializava as threads no log (picos de ms medidos em FORMAT_SUPPORT).
    if (FirstFeatureSeen(static_cast<unsigned>(feature))) {
        char buf[256] {};
        std::snprintf(
            buf,
            sizeof(buf),
            "CheckFeatureSupport feature=%u: HRESULT 0x%08lX",
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
        D3D_FEATURE_LEVEL maxRequested = static_cast<D3D_FEATURE_LEVEL>(0);
        for (UINT i = 0; i < levels->NumFeatureLevels; ++i) {
            D3D_FEATURE_LEVEL lv = levels->pFeatureLevelsRequested[i];
            if (lv == D3D_FEATURE_LEVEL_12_0) {
                wants12_0 = true;
            }
            if (lv > maxRequested) {
                maxRequested = lv;
            }
        }
        // Consulta unaria em 12_1: responde o melhor possivel (12_0).
        bool unary12_1 = !wants12_0 && maxRequested == D3D_FEATURE_LEVEL_12_1;
        if (SUCCEEDED(hr)) {
            if (levels->MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0) {
                return hr; // Nativo ja serve: nao rebaixa 12_1/12_2.
            }
            if (wants12_0 || unary12_1) {
                levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
                if (FirstFeatureSeen(kLogIdSpoof)) {
                    Log("Spoof: FEATURE_LEVELS -> 12_0");
                }
                return S_OK;
            }
            return hr;
        }
        if (hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED) &&
            (wants12_0 || unary12_1)) {
            levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
            if (FirstFeatureSeen(kLogIdSpoofUnsupported)) {
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

// ---- Stream desc (ID3D12Device2::CreatePipelineState, vtable 47) ----
template<typename T> struct StreamSub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type;
    T Data;
};
template<typename T> static size_t StreamDataOff() {
    return (4 + alignof(T) - 1) & ~(alignof(T) - 1);
}
template<typename T> static size_t StreamTotal() {
    return (StreamDataOff<T>() + sizeof(T) + 7) & ~size_t(7);
}

static bool StreamStride(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t,
    size_t* stride, size_t* dataOff) {
    switch (t) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        *stride = StreamTotal<ID3D12RootSignature*>();
        *dataOff = StreamDataOff<ID3D12RootSignature*>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        *stride = StreamTotal<D3D12_SHADER_BYTECODE>();
        *dataOff = StreamDataOff<D3D12_SHADER_BYTECODE>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        *stride = StreamTotal<D3D12_STREAM_OUTPUT_DESC>();
        *dataOff = StreamDataOff<D3D12_STREAM_OUTPUT_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        *stride = StreamTotal<D3D12_BLEND_DESC>();
        *dataOff = StreamDataOff<D3D12_BLEND_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        *stride = StreamTotal<UINT>();
        *dataOff = StreamDataOff<UINT>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        *stride = StreamTotal<D3D12_RASTERIZER_DESC>();
        *dataOff = StreamDataOff<D3D12_RASTERIZER_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        *stride = StreamTotal<D3D12_DEPTH_STENCIL_DESC>();
        *dataOff = StreamDataOff<D3D12_DEPTH_STENCIL_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        *stride = StreamTotal<D3D12_INPUT_LAYOUT_DESC>();
        *dataOff = StreamDataOff<D3D12_INPUT_LAYOUT_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        *stride = StreamTotal<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
        *dataOff = StreamDataOff<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        *stride = StreamTotal<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
        *dataOff = StreamDataOff<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        *stride = StreamTotal<D3D12_RT_FORMAT_ARRAY>();
        *dataOff = StreamDataOff<D3D12_RT_FORMAT_ARRAY>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        *stride = StreamTotal<DXGI_FORMAT>();
        *dataOff = StreamDataOff<DXGI_FORMAT>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        *stride = StreamTotal<DXGI_SAMPLE_DESC>();
        *dataOff = StreamDataOff<DXGI_SAMPLE_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        *stride = StreamTotal<UINT>();
        *dataOff = StreamDataOff<UINT>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        *stride = StreamTotal<D3D12_CACHED_PIPELINE_STATE>();
        *dataOff = StreamDataOff<D3D12_CACHED_PIPELINE_STATE>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        *stride = StreamTotal<D3D12_PIPELINE_STATE_FLAGS>();
        *dataOff = StreamDataOff<D3D12_PIPELINE_STATE_FLAGS>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        *stride = StreamTotal<D3D12_DEPTH_STENCIL_DESC1>();
        *dataOff = StreamDataOff<D3D12_DEPTH_STENCIL_DESC1>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        *stride = StreamTotal<D3D12_VIEW_INSTANCING_DESC>();
        *dataOff = StreamDataOff<D3D12_VIEW_INSTANCING_DESC>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        *stride = StreamTotal<D3D12_SHADER_BYTECODE>();
        *dataOff = StreamDataOff<D3D12_SHADER_BYTECODE>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
        *stride = StreamTotal<D3D12_DEPTH_STENCIL_DESC2>();
        *dataOff = StreamDataOff<D3D12_DEPTH_STENCIL_DESC2>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
        *stride = StreamTotal<D3D12_RASTERIZER_DESC1>();
        *dataOff = StreamDataOff<D3D12_RASTERIZER_DESC1>();
        return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
        *stride = StreamTotal<D3D12_RASTERIZER_DESC2>();
        *dataOff = StreamDataOff<D3D12_RASTERIZER_DESC2>();
        return true;
    default:
        return false;
    }
}

struct StreamScan {
    bool valid;
    bool depthOnlyPs;
    const D3D12_SHADER_BYTECODE* ps;
};

// Varredura fail-closed: qualquer layout fora do esperado aborta o retry.
static StreamScan ScanStream(const void* stream, SIZE_T size) {
    StreamScan r {};
    if (!stream || size < 8 || size > 65536) {
        return r;
    }
    const unsigned char* p = static_cast<const unsigned char*>(stream);
    const unsigned char* end = p + size;
    unsigned rtvCount = 0;
    bool sawRtv = false;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    bool sawDsv = false;
    const D3D12_SHADER_BYTECODE* ps = nullptr;
    while (p + 8 <= end) {
        int ti = 0;
        memcpy(&ti, p, 4);
        if (ti < 0 || ti >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID) {
            return r;
        }
        auto type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(ti);
        size_t stride = 0;
        size_t dataOff = 0;
        if (!StreamStride(type, &stride, &dataOff) || !stride || p + stride > end) {
            return r;
        }
        const unsigned char* data = p + dataOff;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
            ps = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(data);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
            auto* arr = reinterpret_cast<const D3D12_RT_FORMAT_ARRAY*>(data);
            sawRtv = true;
            rtvCount = 0;
            for (int i = 0; i < 8; ++i) {
                if (arr->RTFormats[i] != DXGI_FORMAT_UNKNOWN) {
                    ++rtvCount;
                }
            }
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
            sawDsv = true;
            memcpy(&dsv, data, sizeof(dsv));
        }
        p += stride;
    }
    if (p != end) {
        return r;
    }
    r.valid = true;
    r.depthOnlyPs = sawRtv && rtvCount == 0 && sawDsv && dsv != DXGI_FORMAT_UNKNOWN &&
        ps && ps->pShaderBytecode && ps->BytecodeLength > 0;
    r.ps = ps;
    return r;
}

// Recopia o stream sem o subobjeto PS. Devolve false se nao couber ou falhar.
static bool BuildStreamWithoutPs(const void* stream, SIZE_T size,
    unsigned char* out, SIZE_T outCap, SIZE_T* outSize) {
    const unsigned char* p = static_cast<const unsigned char*>(stream);
    const unsigned char* end = p + size;
    SIZE_T used = 0;
    while (p + 8 <= end) {
        int ti = 0;
        memcpy(&ti, p, 4);
        auto type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(ti);
        size_t stride = 0;
        size_t dataOff = 0;
        (void)dataOff;
        if (!StreamStride(type, &stride, &dataOff) || !stride || p + stride > end) {
            return false;
        }
        if (type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
            if (used + stride > outCap) {
                return false;
            }
            memcpy(out + used, p, stride);
            used += stride;
        }
        p += stride;
    }
    if (p != end) {
        return false;
    }
    *outSize = used;
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(
    ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** ppPipelineState
) {
    CreateGraphicsPipelineState_t original = LookupOriginals(self).pso;
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
        uint32_t psKey = PsKey(desc->PS);
        CountRetry();
        if (FirstRetryOfPs(psKey) || g_logLevel >= 2) {
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
                psKey,
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

static HRESULT STDMETHODCALLTYPE HookedCreatePipelineStateStream(
    ID3D12Device2* self,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    REFIID riid,
    void** ppPipelineState
) {
    CreatePipelineState_t original = LookupOriginals(self).psoStream;
    if (!original) {
        return E_FAIL;
    }

    HRESULT hr = original(self, desc, riid, ppPipelineState);
    if (g_depthRetry == 1 && hr == E_INVALIDARG && desc &&
        desc->pPipelineStateSubobjectStream && desc->SizeInBytes > 0) {
        StreamScan s = ScanStream(
            desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
        if (s.valid && s.depthOnlyPs) {
            StreamCopy buf(desc->SizeInBytes);
            SIZE_T outSize = 0;
            if (!buf.cap || !BuildStreamWithoutPs(desc->pPipelineStateSubobjectStream,
                    desc->SizeInBytes, buf.p, buf.cap, &outSize)) {
                LogStreamCopyFailed(desc->SizeInBytes);
            } else {
                D3D12_PIPELINE_STATE_STREAM_DESC patched = *desc;
                patched.pPipelineStateSubobjectStream = buf.p;
                patched.SizeInBytes = outSize;
                HRESULT retryHr = original(self, &patched, riid, ppPipelineState);
                uint32_t psKey = PsKey(*s.ps);
                CountRetry();
                if (FirstRetryOfPs(psKey) || g_logLevel >= 2) {
                    char retryBuf[256] {};
                    std::snprintf(retryBuf, sizeof(retryBuf),
                        "Stream PSO retry without PS: original 0x%08lX retry 0x%08lX PS=%zu(hash 0x%08X)",
                        static_cast<unsigned long>(hr),
                        static_cast<unsigned long>(retryHr),
                        s.ps->BytecodeLength,
                        psKey);
                    Log(retryBuf);
                }
                if (SUCCEEDED(retryHr)) {
                    return retryHr;
                }
            }
        }
    }
    if (FAILED(hr) && TakeBudget(&g_psoFailureLogBudget)) {
        char buf[256] {};
        std::snprintf(buf, sizeof(buf),
            "CreatePipelineState(stream) failed 0x%08lX size=%zu",
            static_cast<unsigned long>(hr),
            desc ? static_cast<size_t>(desc->SizeInBytes) : 0);
        Log(buf);
    }
    return hr;
}

// Wrapper de ID3D12PipelineLibrary1: repassa tudo, retenta LoadGraphicsPipeline
// e LoadPipeline (stream) depth-only sem PS. Refcount espelha o real.
// Limite conhecido: identidade COM parcial. QI de IIDs que o wrapper nao
// declara devolve o objeto real; se o ultimo Release vier por esse ponteiro, o
// wrapper (poucos bytes, um por biblioteca) vaza. Aceito: o jogo usa a
// interface que recebeu.
class PipelineLibraryHook : public ID3D12PipelineLibrary1 {
    ID3D12PipelineLibrary* m_real;
    ID3D12PipelineLibrary1* m_real1; // Sem ref propria (mesmo objeto de m_real); nulo sem stream.

    PipelineLibraryHook(ID3D12PipelineLibrary* real, ID3D12PipelineLibrary1* real1)
        : m_real(real), m_real1(real1) {
    }

public:
    static PipelineLibraryHook* Create(ID3D12PipelineLibrary* real, ID3D12PipelineLibrary1* real1) {
        void* mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PipelineLibraryHook));
        if (!mem) {
            return nullptr;
        }
        return new (mem) PipelineLibraryHook(real, real1);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12PipelineLibrary) ||
            (m_real1 && riid == __uuidof(ID3D12PipelineLibrary1))) {
            *pp = static_cast<ID3D12PipelineLibrary1*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_real->AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_real->Release();
        if (c == 0) {
            this->~PipelineLibraryHook();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return c;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return m_real->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return m_real->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        return m_real->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
        return m_real->SetName(name);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override {
        return m_real->GetDevice(riid, pp);
    }
    HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState* pipe) override {
        return m_real->StorePipeline(name, pipe);
    }
    HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** pp) override {
        return m_real->LoadComputePipeline(name, desc, riid, pp);
    }
    HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pp) override {
        HRESULT hr = m_real->LoadGraphicsPipeline(name, desc, riid, pp);
        if (g_depthRetry == 1 && hr == E_INVALIDARG && desc &&
            desc->NumRenderTargets == 0 &&
            desc->DSVFormat != DXGI_FORMAT_UNKNOWN &&
            desc->PS.pShaderBytecode && desc->PS.BytecodeLength > 0) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC patched = *desc;
            patched.PS = {};
            // E_INVALIDARG tambem e o cache miss normal (nome ausente): o retry
            // custa uma chamada a mais. So loga se o retry mudou o resultado.
            HRESULT retryHr = m_real->LoadGraphicsPipeline(name, &patched, riid, pp);
            if (retryHr != hr) {
                CountRetry();
            }
            // Dedupe compartilhado com CreateGraphicsPipelineState: o mesmo PS ja
            // logado la nao repete aqui.
            if (retryHr != hr && (FirstRetryOfPs(PsKey(desc->PS)) || g_logLevel >= 2)) {
                char buf[256] {};
                std::snprintf(buf, sizeof(buf),
                    "Library LoadGraphicsPipeline retry without PS: original 0x%08lX retry 0x%08lX",
                    static_cast<unsigned long>(hr),
                    static_cast<unsigned long>(retryHr));
                Log(buf);
            }
            if (SUCCEEDED(retryHr)) {
                return retryHr;
            }
        }
        return hr;
    }
    SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
        return m_real->GetSerializedSize();
    }
    HRESULT STDMETHODCALLTYPE Serialize(void* data, SIZE_T size) override {
        return m_real->Serialize(data, size);
    }
    HRESULT STDMETHODCALLTYPE LoadPipeline(LPCWSTR name,
        const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** pp) override {
        ID3D12PipelineLibrary1* lib1 = m_real1;
        if (!lib1) {
            return E_NOINTERFACE;
        }
        HRESULT hr = lib1->LoadPipeline(name, desc, riid, pp);
        if (g_depthRetry == 1 && hr == E_INVALIDARG && desc &&
            desc->pPipelineStateSubobjectStream && desc->SizeInBytes > 0) {
            StreamScan s = ScanStream(
                desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
            if (s.valid && s.depthOnlyPs) {
                StreamCopy buf(desc->SizeInBytes);
                SIZE_T outSize = 0;
                if (!buf.cap || !BuildStreamWithoutPs(desc->pPipelineStateSubobjectStream,
                        desc->SizeInBytes, buf.p, buf.cap, &outSize)) {
                    LogStreamCopyFailed(desc->SizeInBytes);
                } else {
                    D3D12_PIPELINE_STATE_STREAM_DESC patched = *desc;
                    patched.pPipelineStateSubobjectStream = buf.p;
                    patched.SizeInBytes = outSize;
                    HRESULT retryHr = lib1->LoadPipeline(name, &patched, riid, pp);
                    if (retryHr != hr) {
                        CountRetry();
                    }
                    if (retryHr != hr && (FirstRetryOfPs(PsKey(*s.ps)) || g_logLevel >= 2)) {
                        char retryBuf[256] {};
                        std::snprintf(retryBuf, sizeof(retryBuf),
                            "Library LoadPipeline retry without PS: original 0x%08lX retry 0x%08lX",
                            static_cast<unsigned long>(hr),
                            static_cast<unsigned long>(retryHr));
                        Log(retryBuf);
                    }
                    if (SUCCEEDED(retryHr)) {
                        return retryHr;
                    }
                }
            }
        }
        return hr;
    }
};

static HRESULT STDMETHODCALLTYPE HookedCreatePipelineLibrary(
    ID3D12Device* self,
    const void* blob,
    SIZE_T blobLen,
    REFIID riid,
    void** ppPipelineLibrary
) {
    CreatePipelineLibrary_t original = LookupOriginals(self).libCreate;
    if (!original) {
        return E_FAIL;
    }

    HRESULT hr = original(self, blob, blobLen, riid, ppPipelineLibrary);
    if (SUCCEEDED(hr) && ppPipelineLibrary && *ppPipelineLibrary && g_hookLib == 1) {
        ID3D12PipelineLibrary* real =
            static_cast<ID3D12PipelineLibrary*>(*ppPipelineLibrary);
        ID3D12PipelineLibrary1* lib1 = nullptr;
        if (FAILED(real->QueryInterface(__uuidof(ID3D12PipelineLibrary1),
                reinterpret_cast<void**>(&lib1)))) {
            lib1 = nullptr;
        }
        if (lib1) {
            lib1->Release(); // Ponteiro mantido sem ref: vive enquanto real viver.
        }
        PipelineLibraryHook* wrapped = PipelineLibraryHook::Create(real, lib1);
        if (wrapped) {
            *ppPipelineLibrary = wrapped;
            if (TakeBudget(&g_featureLogBudget)) {
                Log("PipelineLibrary encapsulada para retry depth-only");
            }
        }
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
    bool wantStream = false;
    bool wantLibrary = (g_hookLib == 1);
    ID3D12Device2* dev2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device2),
            reinterpret_cast<void**>(&dev2))) && dev2) {
        wantStream = true;
        dev2->Release();
    } else if (g_hookLib == 1) {
        ID3D12Device1* dev1 = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(ID3D12Device1),
                reinterpret_cast<void**>(&dev1))) || !dev1) {
            wantLibrary = false;
        } else {
            dev1->Release();
        }
    } else {
        wantLibrary = false;
    }
    size_t maxIdx = kCheckFeatureSupportVtableIndex;
    if (wantStream) {
        maxIdx = kCreatePipelineStateVtableIndex;
    } else if (wantLibrary) {
        maxIdx = kCreatePipelineLibraryVtableIndex;
    }
    MEMORY_BASIC_INFORMATION mbi {};
    SIZE_T need = sizeof(void*) * (maxIdx + 1);
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
            reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState) &&
        (!wantStream || vtable[kCreatePipelineStateVtableIndex] ==
            reinterpret_cast<void*>(&HookedCreatePipelineStateStream)) &&
        (!wantLibrary || vtable[kCreatePipelineLibraryVtableIndex] ==
            reinterpret_cast<void*>(&HookedCreatePipelineLibrary))) {
        device->Release();
        return; // Ja hookado por outro device da mesma vtable.
    }

    VtableHook e {};
    bool known = false;
    bool tableFull = false;
    bool protFail = false;
    int written = 0;
    AcquireSRWLockExclusive(&g_devicePatchLock);
    size_t count = g_vhookCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (g_vhooks[i].vtable == vtable) {
            e = g_vhooks[i];
            known = true;
            break;
        }
    }
    if (!known) {
        if (count >= kMaxVtableHooks) {
            tableFull = true;
        } else {
            // Slot que ja aponta para nosso hook (copia parcial de vtable feita
            // por terceiro) nao vira "original": chamaria a si mesmo sem fim.
            auto orig = [&](size_t idx, void* hook) -> void* {
                return vtable[idx] == hook ? nullptr : vtable[idx];
            };
            e.vtable = vtable;
            e.check = reinterpret_cast<CheckFeatureSupport_t>(orig(
                kCheckFeatureSupportVtableIndex, reinterpret_cast<void*>(&HookedCheckFeatureSupport)));
            e.pso = reinterpret_cast<CreateGraphicsPipelineState_t>(orig(
                kCreateGraphicsPipelineStateVtableIndex,
                reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState)));
            if (wantStream) {
                e.psoStream = reinterpret_cast<CreatePipelineState_t>(orig(
                    kCreatePipelineStateVtableIndex,
                    reinterpret_cast<void*>(&HookedCreatePipelineStateStream)));
            }
            if (wantLibrary) {
                e.libCreate = reinterpret_cast<CreatePipelineLibrary_t>(orig(
                    kCreatePipelineLibraryVtableIndex,
                    reinterpret_cast<void*>(&HookedCreatePipelineLibrary)));
            }
            // Publica antes de escrever na vtable: quem cair no hook ja acha a entrada.
            g_vhooks[count] = e;
            g_vhookCount.store(count + 1, std::memory_order_release);
        }
    }
    if (!tableFull) {
        // Mesma rotina para o patch inicial e para reaplicar: so troca slot que
        // ainda tem o ponteiro original do driver (CAS). Slot ja nosso ou de
        // outro hooker que veio depois (e nos chama) fica como esta. Cobre o
        // "unhook" de terceiro que restaura o original por cima do nosso.
        struct Slot { size_t idx; void* orig; void* hook; };
        const Slot slots[] = {
            { kCreateGraphicsPipelineStateVtableIndex, reinterpret_cast<void*>(e.pso),
              reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState) },
            { kCheckFeatureSupportVtableIndex, reinterpret_cast<void*>(e.check),
              reinterpret_cast<void*>(&HookedCheckFeatureSupport) },
            { kCreatePipelineLibraryVtableIndex, reinterpret_cast<void*>(e.libCreate),
              reinterpret_cast<void*>(&HookedCreatePipelineLibrary) },
            { kCreatePipelineStateVtableIndex, reinterpret_cast<void*>(e.psoStream),
              reinterpret_cast<void*>(&HookedCreatePipelineStateStream) },
        };
        size_t hi = kCheckFeatureSupportVtableIndex;
        for (const Slot& sl : slots) {
            if (sl.orig && sl.idx > hi) {
                hi = sl.idx;
            }
        }
        bool pending = false;
        for (const Slot& sl : slots) {
            if (sl.orig && vtable[sl.idx] == sl.orig) {
                pending = true;
            }
        }
        if (pending) {
            DWORD oldProtect = 0;
            SIZE_T range = sizeof(void*) * (hi - kCreateGraphicsPipelineStateVtableIndex + 1);
            if (VirtualProtect(&vtable[kCreateGraphicsPipelineStateVtableIndex],
                    range, PAGE_READWRITE, &oldProtect)) {
                for (const Slot& sl : slots) {
                    if (sl.orig && InterlockedCompareExchangePointer(
                            &vtable[sl.idx], sl.hook, sl.orig) == sl.orig) {
                        ++written;
                    }
                }
                DWORD ignored = 0;
                VirtualProtect(&vtable[kCreateGraphicsPipelineStateVtableIndex],
                    range, oldProtect, &ignored);
            } else {
                protFail = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_devicePatchLock);
    device->Release();

    if (tableFull) {
        Log("Tabela de vtables cheia, patch ignorado");
    } else if (protFail) {
        Log("VirtualProtect falhou ao patchar metodos do ID3D12Device");
    } else if (!known) {
        Log("Patch aplicado: ID3D12Device + Device1/Device2");
    } else if (written > 0) {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf),
            "Patch reaplicado: %d slot(s) tinham voltado ao original do driver", written);
        Log(buf);
    }
}

static LONG s_osdShown = 0;
static ATOM s_osdAtom = 0;

// Aviso visual de 4s, sem roubar foco (NOACTIVATE), em thread propria.
static DWORD WINAPI OsdThread(LPVOID param) {
    int mode = static_cast<int>(reinterpret_cast<INT_PTR>(param));
    const wchar_t* text = (mode == 0) ?
        L"SpiderFix ativo - modo BYPASS (GPU 12_1+)" :
        L"SpiderFix ativo - modo FIX (GPU 12_0)";
    static const wchar_t kCls[] = L"SpiderFixOsd";
    HMODULE inst = GetModuleHandleW(nullptr);
    if (!s_osdAtom) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kCls;
        s_osdAtom = RegisterClassExW(&wc);
    }
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    // LAYERED + TRANSPARENT: cliques atravessam a janela e o DWM compoe so a
    // regiao dela, sem disputar foco com o jogo.
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kCls, text, WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (sw > 420 ? sw - 420 : 0), (sh > 140 ? sh - 140 : 0),
        400, 80, nullptr, nullptr, inst, nullptr);
    if (!w) {
        return 0;
    }
    SetLayeredWindowAttributes(w, 0, 230, LWA_ALPHA); // Sem isso janela layered nao aparece.
    ShowWindow(w, SW_SHOWNOACTIVATE);
    SetTimer(w, 1, 4000, nullptr);
    MSG msg {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(w);
    return 0;
}

static void ShowOsdOnce(int mode) {
    if (g_showOsd != 1) {
        return;
    }
    if (InterlockedCompareExchange(&s_osdShown, 1, 0) != 0) {
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, OsdThread,
        reinterpret_cast<LPVOID>(static_cast<INT_PTR>(mode)), 0, nullptr);
    if (th) {
        CloseHandle(th);
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

    // A partir daqui IAT do exe, vtables e thread do OSD apontam para codigo
    // desta DLL: fixa o modulo para um FreeLibrary nao deixar ponteiro solto.
    static LONG s_pinned = 0;
    if (InterlockedCompareExchange(&s_pinned, 1, 0) == 0) {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&D3D12CreateDevice), &pinned);
    }

    // O fix sustenta ate 12_1. Pedido acima (12_2: mesh shaders, sampler
    // feedback...) passa direto: fingir suporte faria o jogo usar recurso que a
    // GPU nao tem. Vale para a sonda (ppDevice nulo) e para a criacao real.
    if (minimumFeatureLevel > D3D_FEATURE_LEVEL_12_1) {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "FL pedido 0x%X > 12_1: repasse direto",
            static_cast<unsigned>(minimumFeatureLevel));
        Log(buf);
        return g_originalD3D12CreateDevice(pAdapter, minimumFeatureLevel, riid, ppDevice);
    }

    // Adaptador do jogo com 12_1+ ignora o fix (passthrough puro, sem hooks).
    // 12_0 ou menos segue o caminho do fix. ForceActive/SPIDERFIX_FORCE=1
    // forca o fix em qualquer GPU.
    if (g_force == 0) {
        if (!AdapterNeedsFix(pAdapter)) {
            Log("GPU nativa 12_1+: bypass, fix ignorado");
            ShowOsdOnce(0);
            return g_originalD3D12CreateDevice(
                pAdapter, minimumFeatureLevel, riid, ppDevice);
        }
    }
    ShowOsdOnce(1);

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
