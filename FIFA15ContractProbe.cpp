#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <atomic>

#pragma comment(lib, "Psapi.lib")

static CRITICAL_SECTION gLogLock;
static bool gLogReady = false;
static wchar_t gLogPath[MAX_PATH] = {0};

static constexpr uintptr_t WRITER_INSTRUCTION_RVA = 0x2F8CE2E;
static constexpr uintptr_t DISPATCHER_RETURN_RVA = 0x2F8382C;
static constexpr uintptr_t PACKET_CALLER_RETURN_RVA = 0x2F8D170;

static constexpr uint32_t CONTRACT_FIELD_OFFSET = 156;
static constexpr uint32_t CONTRACT_FIELD_DEPTH = 11;

static constexpr size_t RECORD_SIZE = 84;
static constexpr uint32_t PLAYER_ID_BIT_OFFSET = 520;
static constexpr uint32_t PLAYER_ID_BIT_DEPTH = 19;
static constexpr uint32_t JOIN_DATE_BIT_OFFSET = 458;
static constexpr uint32_t JOIN_DATE_BIT_DEPTH = 20;

static constexpr ULONGLONG MAX_SEQUENCE_DELAY_MS = 2000;

static DWORD gProcessId = 0;
static uintptr_t gModuleBase = 0;
static uintptr_t gBreakpointAddress = 0;
static PVOID gVehHandle = nullptr;

static std::atomic<bool> gArmed(false);
static std::atomic<LONG> gRelevantWrites(0);
static std::atomic<LONG> gCorrections(0);

struct SequenceState {
    uintptr_t record;
    uint32_t playerId;
    uint32_t correctYear;
    uint32_t joinDate;
    DWORD threadId;
    ULONGLONG tick;
};

static SequenceState gStates[1024]{};
static volatile LONG gStateLock = 0;

static void RawAppend(const char* text) {
    FILE* f = nullptr;
    _wfopen_s(&f, gLogPath, L"ab");
    if (f) {
        fputs(text, f);
        fputs("\r\n", f);
        fclose(f);
    }
}

static void Log(const char* fmt, ...) {
    if (!gLogReady) return;

    char line[8192]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);

    int n = sprintf_s(
        line,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
    );

    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&gLogLock);
    RawAppend(line);
    LeaveCriticalSection(&gLogLock);
}

static bool ReadMemory(uintptr_t address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address),
        out,
        size,
        &got
    ) && got == size;
}

static uint32_t ExtractBits(
    const unsigned char* record,
    uint32_t bitOffset,
    uint32_t depth
) {
    uint32_t value = 0;

    for (uint32_t bit = 0; bit < depth; ++bit) {
        const uint32_t absolute = bitOffset + bit;
        const uint32_t byteIndex = absolute / 8;
        const uint32_t bitIndex = absolute % 8;

        if (record[byteIndex] & (1u << bitIndex)) {
            value |= (1u << bit);
        }
    }

    return value;
}

static void LockStates() {
    while (InterlockedCompareExchange(&gStateLock, 1, 0) != 0) {
        YieldProcessor();
    }
}

static void UnlockStates() {
    InterlockedExchange(&gStateLock, 0);
}

static void RememberCorrectWrite(
    uintptr_t record,
    uint32_t playerId,
    uint32_t correctYear,
    uint32_t joinDate,
    DWORD threadId,
    ULONGLONG tick
) {
    LockStates();

    int slot = -1;
    int empty = -1;
    int oldest = 0;
    ULONGLONG oldestTick = ~0ULL;

    for (int i = 0; i < 1024; ++i) {
        if (gStates[i].record == record) {
            slot = i;
            break;
        }

        if (gStates[i].record == 0 && empty < 0) {
            empty = i;
        }

        if (gStates[i].tick < oldestTick) {
            oldestTick = gStates[i].tick;
            oldest = i;
        }
    }

    if (slot < 0) slot = empty >= 0 ? empty : oldest;

    gStates[slot].record = record;
    gStates[slot].playerId = playerId;
    gStates[slot].correctYear = correctYear;
    gStates[slot].joinDate = joinDate;
    gStates[slot].threadId = threadId;
    gStates[slot].tick = tick;

    UnlockStates();
}

static bool MatchWrongOverwrite(
    uintptr_t record,
    uint32_t playerId,
    uint32_t incomingYear,
    DWORD threadId,
    ULONGLONG tick,
    uint32_t& correctYear,
    uint32_t& previousJoinDate,
    DWORD& previousThreadId,
    ULONGLONG& deltaMs
) {
    correctYear = 0;
    previousJoinDate = 0;
    previousThreadId = 0;
    deltaMs = 0;

    LockStates();

    for (int i = 0; i < 1024; ++i) {
        SequenceState& state = gStates[i];

        if (
            state.record == record &&
            state.playerId == playerId &&
            state.correctYear == incomingYear + 1 &&
            tick >= state.tick
        ) {
            deltaMs = tick - state.tick;

            if (deltaMs <= MAX_SEQUENCE_DELAY_MS) {
                correctYear = state.correctYear;
                previousJoinDate = state.joinDate;
                previousThreadId = state.threadId;

                // Consume the state so the same stale sequence cannot match twice.
                state = SequenceState{};

                UnlockStates();
                return true;
            }
        }
    }

    UnlockStates();
    return false;
}

static bool ProgramBreakpointForThread(DWORD threadId) {
    if (!gArmed.load() || !gBreakpointAddress) return false;

    HANDLE thread = OpenThread(
        THREAD_GET_CONTEXT |
        THREAD_SET_CONTEXT |
        THREAD_SUSPEND_RESUME |
        THREAD_QUERY_INFORMATION,
        FALSE,
        threadId
    );

    if (!thread) return false;

    bool suspended = false;

    if (threadId != GetCurrentThreadId()) {
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            return false;
        }
        suspended = true;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    bool ok = false;

    if (GetThreadContext(thread, &ctx)) {
        ctx.Dr0 = static_cast<DWORD64>(gBreakpointAddress);
        ctx.Dr7 |= 0x1ULL;
        ctx.Dr7 &= ~(0xFULL << 16);
        ctx.Dr6 = 0;
        ok = SetThreadContext(thread, &ctx) != FALSE;
    }

    if (suspended) ResumeThread(thread);
    CloseHandle(thread);
    return ok;
}

static int ProgramAllThreads() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    int count = 0;

    if (Thread32First(snap, &te)) {
        do {
            if (
                te.th32OwnerProcessID == gProcessId &&
                ProgramBreakpointForThread(te.th32ThreadID)
            ) {
                ++count;
            }
        } while (Thread32Next(snap, &te));
    }

    CloseHandle(snap);
    return count;
}

static LONG CALLBACK VehHandler(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* ctx = info->ContextRecord;
    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);

    if (!(ctx->Dr6 & 0x1) || rip != gBreakpointAddress) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ctx->Dr6 = 0;

    uint32_t descriptorOffset = 0;
    uint32_t descriptorDepth = 0;
    uint32_t incomingYear = 0;

    if (
        !ReadMemory(
            static_cast<uintptr_t>(ctx->Rbx) + 4,
            &descriptorOffset,
            sizeof(descriptorOffset)
        ) ||
        !ReadMemory(
            static_cast<uintptr_t>(ctx->Rbx) + 0xC,
            &descriptorDepth,
            sizeof(descriptorDepth)
        ) ||
        !ReadMemory(
            static_cast<uintptr_t>(ctx->Rdi),
            &incomingYear,
            sizeof(incomingYear)
        )
    ) {
        ctx->EFlags |= 0x10000ULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (
        descriptorOffset != CONTRACT_FIELD_OFFSET ||
        descriptorDepth != CONTRACT_FIELD_DEPTH ||
        incomingYear < 2014 ||
        incomingYear > 2050
    ) {
        ctx->EFlags |= 0x10000ULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    uintptr_t dispatcherReturn = 0;
    uintptr_t packetCallerReturn = 0;

    ReadMemory(
        static_cast<uintptr_t>(ctx->Rsp) + 0x38,
        &dispatcherReturn,
        sizeof(dispatcherReturn)
    );

    ReadMemory(
        static_cast<uintptr_t>(ctx->Rsp) + 0x1E8,
        &packetCallerReturn,
        sizeof(packetCallerReturn)
    );

    const uintptr_t dispatcherRva =
        dispatcherReturn >= gModuleBase
        ? dispatcherReturn - gModuleBase
        : 0;

    const uintptr_t packetCallerRva =
        packetCallerReturn >= gModuleBase
        ? packetCallerReturn - gModuleBase
        : 0;

    if (
        dispatcherRva != DISPATCHER_RETURN_RVA ||
        packetCallerRva != PACKET_CALLER_RETURN_RVA
    ) {
        ctx->EFlags |= 0x10000ULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const uintptr_t recordAddress = static_cast<uintptr_t>(ctx->Rsi);
    const uintptr_t sourcePtr = static_cast<uintptr_t>(ctx->Rdi);
    const uintptr_t writeTarget =
        static_cast<uintptr_t>(ctx->Rdx) + static_cast<uintptr_t>(ctx->Rsi);

    unsigned char record[RECORD_SIZE]{};

    if (!ReadMemory(recordAddress, record, sizeof(record))) {
        ctx->EFlags |= 0x10000ULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const uint32_t storedYearBeforeWrite =
        ExtractBits(record, CONTRACT_FIELD_OFFSET, CONTRACT_FIELD_DEPTH);

    const uint32_t playerId =
        ExtractBits(record, PLAYER_ID_BIT_OFFSET, PLAYER_ID_BIT_DEPTH);

    const uint32_t joinDate =
        ExtractBits(record, JOIN_DATE_BIT_OFFSET, JOIN_DATE_BIT_DEPTH);

    uint32_t signingMode = 0xFFFFFFFFU;
    ReadMemory(sourcePtr - 28, &signingMode, sizeof(signingMode));

    const DWORD threadId = GetCurrentThreadId();
    const ULONGLONG now = GetTickCount64();

    gRelevantWrites.fetch_add(1);

    // Proven signing sequence:
    // A) first write submits the correct year under a non-zero packet mode;
    // B) within a few milliseconds, the same record/thread receives mode 0
    //    with exactly correctYear - 1.
    //
    // We remember A and neutralize B by replacing RAX with the qword already
    // stored at the target address. The original instruction then writes the
    // unchanged qword, so the correct contract year remains intact.
    if (signingMode != 0) {
        RememberCorrectWrite(
            recordAddress,
            playerId,
            incomingYear,
            joinDate,
            threadId,
            now
        );
    }
    else {
        uint32_t correctYear = 0;
        uint32_t previousJoinDate = 0;
        DWORD previousThreadId = 0;
        ULONGLONG deltaMs = 0;

        if (
            MatchWrongOverwrite(
                recordAddress,
                playerId,
                incomingYear,
                threadId,
                now,
                correctYear,
                previousJoinDate,
                previousThreadId,
                deltaMs
            )
        ) {
            uint64_t existingQword = 0;

            if (ReadMemory(writeTarget, &existingQword, sizeof(existingQword))) {
                ctx->Rax = existingQword;

                const LONG count = gCorrections.fetch_add(1) + 1;

                Log(
                    "GENERAL_SIGNING_FIX_APPLIED "
                    "playerId=%u record=%p thread=%lu "
                    "correctYear=%u blockedWrongYear=%u "
                    "storedYearBeforeWrite=%u "
                    "previousJoinDate=%u currentJoinDate=%u "
                    "deltaMs=%llu signingMode=%u matchThread=%u "
                    "writeTarget=%p corrections=%ld",
                    playerId,
                    reinterpret_cast<void*>(recordAddress),
                    threadId,
                    correctYear,
                    incomingYear,
                    storedYearBeforeWrite,
                    previousJoinDate,
                    joinDate,
                    static_cast<unsigned long long>(deltaMs),
                    signingMode,
                    previousThreadId == threadId ? 1u : 0u,
                    reinterpret_cast<void*>(writeTarget),
                    count
                );
            }
            else {
                Log(
                    "GENERAL_SIGNING_FIX_READ_FAILED "
                    "playerId=%u record=%p writeTarget=%p error=%lu",
                    playerId,
                    reinterpret_cast<void*>(recordAddress),
                    reinterpret_cast<void*>(writeTarget),
                    GetLastError()
                );
            }
        }
    }

    ctx->EFlags |= 0x10000ULL;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI Worker(LPVOID) {
    gProcessId = GetCurrentProcessId();

    HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};

    if (
        !exe ||
        !GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi))
    ) {
        Log("ABORT unable to resolve FIFA15 module information");
        return 0;
    }

    gModuleBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    gBreakpointAddress = gModuleBase + WRITER_INSTRUCTION_RVA;

    Log(
        "CONTRACT_FIX_V1_1_LOADED processId=%lu moduleBase=%p "
        "breakpointAddress=%p writerInstructionRVA=0x%llX",
        gProcessId,
        reinterpret_cast<void*>(gModuleBase),
        reinterpret_cast<void*>(gBreakpointAddress),
        static_cast<unsigned long long>(WRITER_INSTRUCTION_RVA)
    );

    gArmed.store(true);

    const int initialThreads = ProgramAllThreads();

    Log(
        "CONTRACT_FIX_V1_1_ARMED threads=%d "
        "fieldOffset=%u fieldDepth=%u "
        "dispatcherReturnRVA=0x%llX packetCallerReturnRVA=0x%llX",
        initialThreads,
        CONTRACT_FIELD_OFFSET,
        CONTRACT_FIELD_DEPTH,
        static_cast<unsigned long long>(DISPATCHER_RETURN_RVA),
        static_cast<unsigned long long>(PACKET_CALLER_RETURN_RVA)
    );

    int seconds = 0;

    for (;;) {
        const int threads = ProgramAllThreads();
        ++seconds;

        Sleep(100);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&gLogLock);

        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);

        swprintf_s(
            gLogPath,
            L"%sFIFA15_ContractFix_v1.1_CalendarContract.log",
            temp
        );

        gLogReady = true;

        Log(
            "FIFA15 Contract Fix v1.1 attached module=%p",
            module
        );

        gVehHandle = AddVectoredExceptionHandler(1, VehHandler);

        HANDLE thread =
            CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

        if (thread) CloseHandle(thread);
        else Log("ERROR CreateThread failed=%lu", GetLastError());
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (gVehHandle) {
            RemoveVectoredExceptionHandler(gVehHandle);
            gVehHandle = nullptr;
        }

        if (gLogReady) {
            Log(
                "FIFA15 Contract Fix v1.1 unloading "
                "relevantWrites=%ld corrections=%ld",
                gRelevantWrites.load(),
                gCorrections.load()
            );
        }
    }

    return TRUE;
}
