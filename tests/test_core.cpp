// Unit tests for ObserverCore.h (no framework; returns non-zero on failure).
#include "../ObserverCore.h"
#include <cstdio>

static int g_failed = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace tco;

static void TestParseAddress()
{
    uint32_t v = 0;
    CHECK(ParseAddress("0x00D995F0", v) && v == 0x00D995F0u);
    CHECK(ParseAddress("  00d995f0  ", v) && v == 0x00D995F0u);
    CHECK(ParseAddress("0XFFFFFFFF", v) && v == 0xFFFFFFFFu);
    CHECK(!ParseAddress("", v));
    CHECK(!ParseAddress("0x", v));
    CHECK(!ParseAddress("0x1FFFFFFFF", v));   // overflow
    CHECK(!ParseAddress("0xZZ", v));
    CHECK(!ParseAddress("12 34", v));
}

static void TestParseHexBytes()
{
    std::vector<uint8_t> b;
    CHECK(ParseHexBytes("55 8B EC", b) && b.size() == 3 && b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC);
    CHECK(ParseHexBytes("558BEC", b) && b.size() == 3 && b[2] == 0xEC);
    CHECK(ParseHexBytes("0x55, 0x8b;0xEC", b) && b.size() == 3 && b[1] == 0x8B);
    CHECK(!ParseHexBytes("", b) && b.empty());
    CHECK(!ParseHexBytes("5", b));              // odd digit count
    CHECK(!ParseHexBytes("55 8G", b));          // invalid digit
    CHECK(BytesToHex(b.data(), 0) == "");
    uint8_t raw[3] = { 0x01, 0xAB, 0xFF };
    CHECK(BytesToHex(raw, 3) == "01 AB FF");
}

static std::vector<uint8_t> Seq(size_t n, uint8_t start = 1)
{
    std::vector<uint8_t> v;
    for (size_t i = 0; i < n; ++i) v.push_back((uint8_t)(start + i * 7));
    return v;
}

static void TestVerifyBytes()
{
    std::vector<uint8_t> exp = Seq(16);
    std::vector<uint8_t> mem = Seq(32);
    CHECK(VerifyBytes(mem.data(), mem.size(), exp) == VerifyResult::Ok);
    CHECK(VerifyBytes(mem.data(), mem.size(), std::vector<uint8_t>()) == VerifyResult::NoExpectedBytes);
    CHECK(VerifyBytes(mem.data(), mem.size(), Seq(15)) == VerifyResult::ExpectedTooShort);
    CHECK(VerifyBytes(mem.data(), mem.size(), std::vector<uint8_t>(16, 0xCC)) == VerifyResult::ExpectedGeneric);
    CHECK(VerifyBytes(nullptr, 0, exp) == VerifyResult::UnreadableMemory);
    CHECK(VerifyBytes(mem.data(), 10, exp) == VerifyResult::UnreadableMemory);   // fewer readable bytes than expected
    mem[5] ^= 0xFF;
    CHECK(VerifyBytes(mem.data(), mem.size(), exp) == VerifyResult::Mismatch);
}

static void TestBudgetAndGate()
{
    EventBudget b(3);
    CHECK(b.Take() && b.Take() && b.Take());
    CHECK(!b.Take() && !b.Take());
    CHECK(b.Used() == 3 && b.Suppressed() == 2);

    IntervalGate g(1000);
    CHECK(!g.Due(5000));            // first call only starts the clock
    CHECK(!g.Due(5999));
    CHECK(g.Due(6000));
    CHECK(!g.Due(6500));
    CHECK(g.Due(7000));
    // wrap-around of the 32-bit millisecond counter
    IntervalGate w(1000);
    CHECK(!w.Due(0xFFFFFF00u));
    CHECK(!w.Due(0xFFFFFF80u));
    CHECK(w.Due(0x00000400u));      // wrapped: 0x400 - 0xFFFFFF00 = 0x500 = 1280 ms
}

static void TestClassify()
{
    // creature == leader (both pointers equal)
    IdentityResult r = Classify({ 0x1000, 0x2000, 0x1000, 0x1000 });
    CHECK(r.isChief && r.matchRaw && r.matchCitizen && !r.rawCitizenDiffer && r.tribeAvailable && r.leaderAvailable);

    // another citizen
    r = Classify({ 0x3000, 0x2000, 0x1000, 0x1000 });
    CHECK(!r.isChief && !r.matchRaw && !r.matchCitizen && !r.rawCitizenDiffer);

    // no tribe (not in the Tribal stage)
    r = Classify({ 0x3000, 0, 0, 0 });
    CHECK(!r.tribeAvailable && !r.leaderAvailable && !r.isChief);

    // tribe without leader
    r = Classify({ 0x3000, 0x2000, 0, 0 });
    CHECK(r.tribeAvailable && !r.leaderAvailable && !r.isChief);

    // null creature never matches, even if the leader pointers are null
    r = Classify({ 0, 0x2000, 0, 0 });
    CHECK(!r.isChief && !r.matchRaw && !r.matchCitizen);

    // pointer adjustment between cCreatureBase* and cCreatureCitizen*: the creature equals only one of them
    r = Classify({ 0x1004, 0x2000, 0x1000, 0x1004 });
    CHECK(r.isChief && !r.matchRaw && r.matchCitizen && r.rawCitizenDiffer);
}

static void TestTrackerAndStats()
{
    ValueTracker t;
    CHECK(t.Observe(0) == ValueTracker::Change::None);
    CHECK(t.Observe(0x10) == ValueTracker::Change::Appeared);
    CHECK(t.Observe(0x10) == ValueTracker::Change::None);
    CHECK(t.Observe(0x20) == ValueTracker::Change::Changed);
    CHECK(t.Previous() == 0x20);
    CHECK(t.Observe(0) == ValueTracker::Change::Disappeared);
    CHECK(t.Observe(0) == ValueTracker::Change::None);

    Stats s;
    s.Record(Classify({ 0x1000, 0x2000, 0x1000, 0x1000 }));   // chief
    s.Record(Classify({ 0x3000, 0x2000, 0x1000, 0x1000 }));   // other
    s.Record(Classify({ 0x3000, 0, 0, 0 }));                  // no tribe
    s.Record(Classify({ 0x3000, 0x2000, 0, 0 }));             // no leader
    s.Record(Classify({ 0x1004, 0x2000, 0x1000, 0x1004 }));   // chief via citizen pointer, mismatch
    CHECK(s.calls == 5 && s.chiefCalls == 2);
    CHECK(s.tribeUnavailable == 1 && s.leaderUnavailable == 1 && s.rawCitizenMismatch == 1);
    CHECK(s.windowCalls == 5 && s.windowChiefCalls == 2);
    s.ResetWindow();
    CHECK(s.windowCalls == 0 && s.windowChiefCalls == 0 && s.calls == 5);
}

int main()
{
    TestParseAddress();
    TestParseHexBytes();
    TestVerifyBytes();
    TestBudgetAndGate();
    TestClassify();
    TestTrackerAndStats();
    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
