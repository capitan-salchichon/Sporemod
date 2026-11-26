// ObserverCore.h
// Pure logic of the "Tribal Chief Observer" prototype (v0.1).
//
// This header has NO dependency on the Spore ModAPI or on Windows, so it can be
// unit-tested with any C++14 compiler (see tests/test_core.cpp).
//
// Important: none of the "pointers" handled here is ever dereferenced. They are
// plain integers used only for comparison and logging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tco {

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

// Parses a hexadecimal address ("0x00D995F0" or "00D995F0"). 32-bit only (Spore is x86).
// Returns false on empty text, invalid characters or overflow.
inline bool ParseAddress(const std::string& text, uint32_t& out)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    size_t end = text.size();
    while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    if (end - i >= 2 && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
    if (i >= end) return false;
    uint64_t value = 0;
    for (; i < end; ++i) {
        char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        value = value * 16 + digit;
        if (value > 0xFFFFFFFFull) return false;
    }
    out = (uint32_t)value;
    return true;
}

// Parses a byte list. Accepts "55 8B EC", "558BEC", "0x55, 0x8B" (separators: space, tab, comma, semicolon).
// Every token must contain an even number of hex digits (after an optional 0x prefix).
inline bool ParseHexBytes(const std::string& text, std::vector<uint8_t>& out)
{
    out.clear();
    size_t i = 0;
    const size_t n = text.size();
    auto isSep = [](char c) { return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r' || c == '\n'; };
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (i < n) {
        while (i < n && isSep(text[i])) ++i;
        if (i >= n) break;
        size_t start = i;
        while (i < n && !isSep(text[i])) ++i;
        size_t tokEnd = i;
        if (tokEnd - start >= 2 && text[start] == '0' && (text[start + 1] == 'x' || text[start + 1] == 'X')) start += 2;
        size_t len = tokEnd - start;
        if (len == 0 || (len % 2) != 0) { out.clear(); return false; }
        for (size_t k = start; k < tokEnd; k += 2) {
            int hi = hexVal(text[k]), lo = hexVal(text[k + 1]);
            if (hi < 0 || lo < 0) { out.clear(); return false; }
            out.push_back((uint8_t)(hi * 16 + lo));
        }
    }
    return !out.empty();
}

inline std::string BytesToHex(const uint8_t* data, size_t len)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i) s.push_back(' ');
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

inline std::string FormatPtr(uintptr_t p)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)p);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Verification of the thunk address (no blind detours)
// ---------------------------------------------------------------------------

enum class VerifyResult {
    Ok,
    NoExpectedBytes,   // nothing configured
    ExpectedTooShort,  // fewer than kMinExpectedBytes
    ExpectedGeneric,   // all bytes equal (padding such as CC CC CC ...)
    UnreadableMemory,  // pointer null or not enough readable bytes
    Mismatch           // the bytes in memory differ from the configured ones
};

constexpr size_t kMinExpectedBytes = 16;

inline const char* VerifyResultName(VerifyResult r)
{
    switch (r) {
    case VerifyResult::Ok: return "OK";
    case VerifyResult::NoExpectedBytes: return "NO_EXPECTED_BYTES";
    case VerifyResult::ExpectedTooShort: return "EXPECTED_TOO_SHORT";
    case VerifyResult::ExpectedGeneric: return "EXPECTED_GENERIC";
    case VerifyResult::UnreadableMemory: return "UNREADABLE_MEMORY";
    case VerifyResult::Mismatch: return "MISMATCH";
    }
    return "?";
}

// `mem`/`memLen` is what was read at the candidate address.
inline VerifyResult VerifyBytes(const uint8_t* mem, size_t memLen, const std::vector<uint8_t>& expected)
{
    if (expected.empty()) return VerifyResult::NoExpectedBytes;
    if (expected.size() < kMinExpectedBytes) return VerifyResult::ExpectedTooShort;
    bool allSame = true;
    for (size_t i = 1; i < expected.size(); ++i)
        if (expected[i] != expected[0]) { allSame = false; break; }
    if (allSame) return VerifyResult::ExpectedGeneric;
    if (mem == nullptr || memLen < expected.size()) return VerifyResult::UnreadableMemory;
    for (size_t i = 0; i < expected.size(); ++i)
        if (mem[i] != expected[i]) return VerifyResult::Mismatch;
    return VerifyResult::Ok;
}

// ---------------------------------------------------------------------------
// Log throttling
// ---------------------------------------------------------------------------

// Limits the number of "event" lines written during a run.
class EventBudget {
public:
    explicit EventBudget(uint32_t maxLines) : max_(maxLines), used_(0), suppressed_(0) {}
    bool Take()
    {
        if (used_ < max_) { ++used_; return true; }
        ++suppressed_;
        return false;
    }
    uint32_t Used() const { return used_; }
    uint32_t Suppressed() const { return suppressed_; }
private:
    uint32_t max_, used_, suppressed_;
};

// Fires at most once per `intervalMs`. `nowMs` may wrap around (unsigned arithmetic).
class IntervalGate {
public:
    explicit IntervalGate(uint32_t intervalMs) : interval_(intervalMs), last_(0), started_(false) {}
    bool Due(uint32_t nowMs)
    {
        if (!started_) { started_ = true; last_ = nowMs; return false; }
        if ((uint32_t)(nowMs - last_) >= interval_) { last_ = nowMs; return true; }
        return false;
    }
private:
    uint32_t interval_, last_;
    bool started_;
};

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

struct IdentityInput {
    uintptr_t creature;       // 4th argument of ForwardWalkTree
    uintptr_t tribe;          // GetPlayerTribe()
    uintptr_t rawLeader;      // tribe->mpLeader.get()
    uintptr_t citizenLeader;  // tribe->GetLeaderCitizen()
};

struct IdentityResult {
    bool tribeAvailable;
    bool leaderAvailable;     // at least one of the two leader pointers is non-null
    bool matchRaw;            // creature == rawLeader
    bool matchCitizen;        // creature == citizenLeader
    bool rawCitizenDiffer;    // both non-null and different (pointer adjustment / layout problem)
    bool isChief;             // creature != 0 && (matchRaw || matchCitizen)
};

inline IdentityResult Classify(const IdentityInput& in)
{
    IdentityResult r;
    r.tribeAvailable = in.tribe != 0;
    r.leaderAvailable = (in.rawLeader != 0) || (in.citizenLeader != 0);
    r.matchRaw = in.creature != 0 && in.creature == in.rawLeader;
    r.matchCitizen = in.creature != 0 && in.creature == in.citizenLeader;
    r.rawCitizenDiffer = in.rawLeader != 0 && in.citizenLeader != 0 && in.rawLeader != in.citizenLeader;
    r.isChief = r.matchRaw || r.matchCitizen;
    return r;
}

// Detects changes of a value over time (tribe pointer, leader pointer).
class ValueTracker {
public:
    enum class Change { None, Appeared, Changed, Disappeared };
    ValueTracker() : prev_(0) {}
    Change Observe(uintptr_t value)
    {
        Change c = Change::None;
        if (prev_ == 0 && value != 0) c = Change::Appeared;
        else if (prev_ != 0 && value == 0) c = Change::Disappeared;
        else if (prev_ != 0 && value != prev_) c = Change::Changed;
        prev_ = value;
        return c;
    }
    uintptr_t Previous() const { return prev_; }
private:
    uintptr_t prev_;
};

inline const char* ChangeName(ValueTracker::Change c)
{
    switch (c) {
    case ValueTracker::Change::None: return "none";
    case ValueTracker::Change::Appeared: return "appeared";
    case ValueTracker::Change::Changed: return "changed";
    case ValueTracker::Change::Disappeared: return "disappeared";
    }
    return "?";
}

// Counters. Totals since start plus a window reset at every periodic report.
struct Stats {
    uint32_t calls = 0;
    uint32_t chiefCalls = 0;
    uint32_t tribeUnavailable = 0;
    uint32_t leaderUnavailable = 0;
    uint32_t rawCitizenMismatch = 0;
    uint32_t windowCalls = 0;
    uint32_t windowChiefCalls = 0;

    void Record(const IdentityResult& r)
    {
        ++calls; ++windowCalls;
        if (r.isChief) { ++chiefCalls; ++windowChiefCalls; }
        if (!r.tribeAvailable) ++tribeUnavailable;
        else if (!r.leaderAvailable) ++leaderUnavailable;
        if (r.rawCitizenDiffer) ++rawCitizenMismatch;
    }
    void ResetWindow() { windowCalls = 0; windowChiefCalls = 0; }
};

} // namespace tco
