#include "handoff.h"
#include <cstddef>
#include <cstring>

namespace handoff {

namespace {

enum : LONG { kWaiting = 0, kClaimed = 1, kAbandoned = 2 };

std::wstring ticketName(DWORD pid, LPARAM serial) {
    return L"Local\\AudioClipEditor.HandOff." + std::to_wstring(pid) + L"." +
           std::to_wstring((unsigned long long)serial);
}

} // namespace

// The shared memory: the flag, then the files, each NUL-terminated.
struct TicketMemory {
    volatile LONG state;
    DWORD chars;        // wchar_t in `text`
    wchar_t text[1];
};

UINT message() {
    static const UINT m = RegisterWindowMessageW(L"AudioClipEditor.HandOff");
    return m;
}

Ticket::Ticket(const std::vector<std::wstring>& files) {
    std::wstring text;
    for (const std::wstring& f : files) { text += f; text.push_back(L'\0'); }
    // The process id keeps tickets of different copies apart, the counter those of one
    // copy -- and those of an earlier process that had the same id, one of whose
    // messages might still be on its way.
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    pid = GetCurrentProcessId();
    serial = (LPARAM)now.QuadPart;
    const size_t bytes = offsetof(TicketMemory, text) + (text.size() + 1) * sizeof(wchar_t);
    SetLastError(ERROR_SUCCESS);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)bytes,
                                 ticketName(pid, serial).c_str());
    if (!mapping) return;
    if (GetLastError() != ERROR_ALREADY_EXISTS)   // someone else's is not ours to use
        shared = (TicketMemory*)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes);
    if (!shared) {   // invalid: leave nothing a receiver could claim
        CloseHandle(mapping);
        mapping = nullptr;
        return;
    }
    shared->chars = (DWORD)text.size();
    std::memcpy(shared->text, text.data(), text.size() * sizeof(wchar_t));
    shared->text[text.size()] = L'\0';
    InterlockedExchange(&shared->state, kWaiting);   // a full barrier: the files first
}

Ticket::~Ticket() {
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
}

bool Ticket::claimed() const {
    return shared && InterlockedCompareExchange(&shared->state, kWaiting, kWaiting) == kClaimed;
}

bool Ticket::abandon() {
    if (!shared) return true;
    return InterlockedCompareExchange(&shared->state, kAbandoned, kWaiting) != kClaimed;
}

std::optional<std::vector<std::wstring>> claim(WPARAM wp, LPARAM lp) {
    if (wp == 0 || wp > MAXDWORD) return std::nullopt;
    HANDLE m = OpenFileMappingW(FILE_MAP_WRITE, FALSE, ticketName((DWORD)wp, lp).c_str());
    if (!m) return std::nullopt;   // its sender has gone, or there is no such ticket
    std::optional<std::vector<std::wstring>> files;
    auto* s = (TicketMemory*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi{};
    // The sender wrote `chars`; it is read once, and believed only as far as the
    // memory goes.
    const size_t chars = s ? (size_t)s->chars : 0;
    if (s && VirtualQuery(s, &mbi, sizeof(mbi)) &&
        mbi.RegionSize >= offsetof(TicketMemory, text) &&
        (mbi.RegionSize - offsetof(TicketMemory, text)) / sizeof(wchar_t) >= chars &&
        InterlockedCompareExchange(&s->state, kClaimed, kWaiting) == kWaiting) {
        files.emplace();
        const wchar_t* p = s->text;
        for (size_t i = 0; i < chars; ) {
            size_t j = i;
            while (j < chars && p[j]) ++j;
            if (j > i) files->emplace_back(p + i, j - i);
            i = j + 1;
        }
    }
    if (s) UnmapViewOfFile(s);
    CloseHandle(m);
    return files;
}

} // namespace handoff
