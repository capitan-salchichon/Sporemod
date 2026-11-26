// mock_main.cpp -- Linux simulation harness for ChiefObserver.cpp (see run_mock_tests.sh).
// Each scenario runs in its own process (the observer keeps global state).
#include "mock_stdafx.h"
#include "ChiefObserver.transformed.cpp"   // the real ChiefObserver.cpp with only include/DETOUR lines adapted

#include <cstdlib>
#include <iostream>
#include <tuple>

uintptr_t baseAddress = 0;

static int g_fail = 0, g_total = 0;
#define EXPECT(cond, msg) do { ++g_total; if (!(cond)) { ++g_fail; std::printf("  FAIL: %s  [%s]\n", msg, #cond); } else { std::printf("  ok:   %s\n", msg); } } while (0)

static std::string ReadFile(const std::string& path)
{
	std::ifstream in(path);
	std::stringstream ss; ss << in.rdbuf();
	return ss.str();
}
static int Count(const std::string& hay, const std::string& needle)
{
	int n = 0; size_t pos = 0;
	while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
	return n;
}

// What the "original ForwardWalkTree" received.
typedef std::tuple<float, double, void*, void*, void*, void*, void*, void*> CallArgs;
static std::vector<CallArgs> g_originalCalls;
static void MockOriginal(float f, double d, void* a, void* b, void* c, void* e, void* g, void* h)
{
	g_originalCalls.push_back(CallArgs(f, d, a, b, c, e, g, h));
}

static const std::string kDir = "/tmp/mock_mod/";
static const std::string kIni = kDir + "TribalChiefObserver.ini";
static const std::string kLog = kDir + "TribalChiefObserver.log";
static const uint32_t kThunkVA = 0x401000;              // image base 0x400000 + 0x1000 => offset 0x1000 inside the fake image

static void WriteIni(const std::string& body) { std::ofstream o(kIni); o << "[Observer]\n" << body; }

static void FillFakeThunk()
{
	MockInitImage();
	for (int i = 0; i < 32; ++i) Win().image[0x1000 + i] = (uint8_t)(0x30 + i * 5);   // 32 distinct bytes
	baseAddress = (uintptr_t)Win().image;
}
static std::string FakeThunkHex(int n) { return tco::BytesToHex(Win().image + 0x1000, (size_t)n); }

typedef ForwardWalkTree_detour Det;
static void CallHook(void* creature, float f = 1.5f, double t = 2.5)
{
	Det::detoured_function(f, t, (void*)0x11, creature, (void*)0x33, (void*)0x44, (void*)0x55, (void*)0x66);
}

int main(int argc, char** argv)
{
	const std::string scenario = argc > 1 ? argv[1] : "";
	std::system(("rm -rf " + kDir + " && mkdir -p " + kDir).c_str());
	FillFakeThunk();
	Det::test_original = MockOriginal;

	if (scenario == "probe") {
		std::printf("scenario probe (no .ini)\n");
		ChiefObserver::Attach(nullptr);
		std::string log = ReadFile(kLog);
		EXPECT(Count(log, "PROBE MODE") == 1, "probe mode announced");
		EXPECT(Det::attached_address == 0, "no detour attached without address");
	}
	else if (scenario == "nobytes") {
		std::printf("scenario nobytes\n");
		WriteIni("thunk_address=0x401000\n");
		ChiefObserver::Attach(nullptr);
		std::string log = ReadFile(kLog);
		EXPECT(Count(log, "NO_EXPECTED_BYTES") == 1, "missing expected_bytes reported");
		EXPECT(Count(log, "bytes at the candidate address") == 1, "probe dump written for manual verification");
		EXPECT(Det::attached_address == 0, "no detour attached");
	}
	else if (scenario == "badbytes") {
		std::printf("scenario badbytes\n");
		std::string good = FakeThunkHex(16);
		std::string bad = good; bad[0] = (bad[0] == 'A') ? 'B' : 'A';
		WriteIni("thunk_address=0x401000\nexpected_bytes=" + bad + "\n");
		ChiefObserver::Attach(nullptr);
		std::string log = ReadFile(kLog);
		EXPECT(Count(log, "MISMATCH") >= 1, "wrong bytes detected");
		EXPECT(Det::attached_address == 0, "no detour attached on mismatch");
	}
	else if (scenario == "short") {
		std::printf("scenario short expected_bytes\n");
		WriteIni("thunk_address=0x401000\nexpected_bytes=" + FakeThunkHex(8) + "\n");
		ChiefObserver::Attach(nullptr);
		EXPECT(Count(ReadFile(kLog), "EXPECTED_TOO_SHORT") == 1, "8 bytes are not enough");
		EXPECT(Det::attached_address == 0, "no detour attached");
	}
	else if (scenario == "outside") {
		std::printf("scenario outside\n");
		WriteIni("thunk_address=0x410000\nexpected_bytes=" + FakeThunkHex(16) + "\n");
		ChiefObserver::Attach(nullptr);
		EXPECT(Count(ReadFile(kLog), "outside the main module") == 1, "address outside the image rejected");
		EXPECT(Det::attached_address == 0, "no detour attached");
	}
	else if (scenario == "nonexec") {
		std::printf("scenario non-executable memory\n");
		Win().executable = false;
		WriteIni("thunk_address=0x401000\nexpected_bytes=" + FakeThunkHex(16) + "\n");
		ChiefObserver::Attach(nullptr);
		EXPECT(Count(ReadFile(kLog), "not readable executable memory") == 1, "non-executable memory rejected");
		EXPECT(Det::attached_address == 0, "no detour attached");
	}
	else if (scenario == "basemismatch") {
		std::printf("scenario ModAPI base mismatch\n");
		WriteIni("thunk_address=0x401000\nexpected_bytes=" + FakeThunkHex(16) + "\n");
		baseAddress += 0x1000;
		ChiefObserver::Attach(nullptr);
		EXPECT(Count(ReadFile(kLog), "differs from the main module base") == 1, "inconsistent base rejected");
		EXPECT(Det::attached_address == 0, "no detour attached");
	}
	else if (scenario == "verified" || scenario == "budget") {
		const bool budget = scenario == "budget";
		std::printf("scenario %s\n", scenario.c_str());
		WriteIni("thunk_address=0x00401000\nexpected_bytes=" + FakeThunkHex(20) + "\nstats_interval_ms=10000\n" +
			(budget ? "max_event_lines=6\n" : ""));
		ChiefObserver::Attach(nullptr);
		ChiefObserver::Initialize();
		std::string log = ReadFile(kLog);
		EXPECT(Det::attached_address == baseAddress + 0x1000, "detour attached at base + VA - 0x400000");
		EXPECT(Count(log, "verification of expected_bytes (20 configured): OK") == 1, "verification OK logged");
		EXPECT(Count(log, "DetourAttach OK") == 1, "attach logged");
		EXPECT(MsgMgr().listeners.size() == 2, "two mode listeners registered");

		Simulator::cGameNounManager* mgr = Simulator::cGameNounManager::Get();
		Simulator::cTribe tribe;
		Simulator::cCreatureCitizen chief1, chief2, citizenAlso;
		tribe.mpLeader.p = static_cast<Simulator::cCreatureBase*>(&chief1);
		mgr->mpPlayerTribe = &tribe;
		void* chief1Raw = (void*)static_cast<Simulator::cCreatureBase*>(&chief1);
		void* chief1Cit = (void*)&chief1;
		EXPECT(chief1Raw != chief1Cit, "harness: cCreatureBase* differs from cCreatureCitizen* (pointer adjustment)");

		std::vector<CallArgs> expectedCalls;
		auto Call = [&](void* creature) {
			CallHook(creature);
			expectedCalls.push_back(CallArgs(1.5f, 2.5, (void*)0x11, creature, (void*)0x33, (void*)0x44, (void*)0x55, (void*)0x66));
		};

		if (!budget) {
			Win().tick = 1000;
			for (int i = 0; i < 50; ++i) Call((void*)(uintptr_t)(0x10000 + i * 0x40));   // other creatures
			for (int i = 0; i < 200; ++i) Call(chief1Raw);                               // the chieftain
			log = ReadFile(kLog);
			EXPECT(g_originalCalls.size() == 250, "original executed on every call (250/250)");
			EXPECT(g_originalCalls == expectedCalls, "original received exactly the original arguments");
			EXPECT(Count(log, "CHIEF_DETECTED") == 1, "chief detected once (no log flooding)");
			EXPECT(Count(log, "matchRaw=1 matchCitizen=0") == 1, "detection reports which pointer matched");
			EXPECT(Count(log, "MISMATCH rawLeader") == 1, "raw vs citizen pointer mismatch reported once");

			Win().tick = 12000;
			Call(chief1Raw);
			log = ReadFile(kLog);
			EXPECT(Count(log, "STATS calls=251") == 1, "periodic statistics after the interval");
			EXPECT(Count(log, "chiefCalls=201") == 1, "statistics count chief calls");
			EXPECT(Count(log, "otherCreatures=50") == 1, "statistics count other creatures");

			// Leader replaced (death / promotion).
			tribe.mpLeader.p = static_cast<Simulator::cCreatureBase*>(&chief2);
			Call((void*)static_cast<Simulator::cCreatureBase*>(&chief2));
			log = ReadFile(kLog);
			EXPECT(Count(log, "LEADER changed") == 1, "leader change logged");
			EXPECT(Count(log, "CHIEF_DETECTED") == 2, "new chief detected after the change");
			// The old chief is no longer identified as the chief.
			size_t before = log.size();
			Call(chief1Raw);
			EXPECT(ReadFile(kLog).size() == before, "old chief is silently treated as a normal creature");

			// Leaving the Tribal stage: no player tribe.
			mgr->mpPlayerTribe = nullptr;
			Call((void*)static_cast<Simulator::cCreatureBase*>(&chief2));
			log = ReadFile(kLog);
			EXPECT(Count(log, "TRIBE disappeared") == 1, "tribe disappearance logged");
			EXPECT(Count(log, "LEADER disappeared") == 1, "leader disappearance logged");
			EXPECT(Count(log, "CHIEF_DETECTED") == 2, "no chief detected without a tribe");

			// Re-entering / loading a game with a tribe whose leader pointer is new.
			mgr->mpPlayerTribe = &tribe;
			tribe.mpLeader.p = static_cast<Simulator::cCreatureBase*>(&citizenAlso);
			Call((void*)static_cast<Simulator::cCreatureBase*>(&citizenAlso));
			log = ReadFile(kLog);
			EXPECT(Count(log, "TRIBE appeared") == 2, "tribe appearance logged again on re-entry (first entry + re-entry)");
			EXPECT(Count(log, "CHIEF_DETECTED") == 3, "chief detected again after re-entering");

			// Tribe without leader.
			tribe.mpLeader.p = nullptr;
			Call((void*)0x20000);
			EXPECT(Count(ReadFile(kLog), "LEADER disappeared") == 2, "tribe without leader handled");

			// Null creature argument never matches.
			Call(nullptr);
			EXPECT(g_originalCalls == expectedCalls, "original still received every argument set (null creature included)");

			// Mode logger.
			App::OnModeEnterMessage m; m.params[0] = kGameTribe; m.params[1] = kGameCreature;
			MsgMgr().listeners[0].first->HandleMessage(App::kMsgOnModeEnter, &m);
			EXPECT(Count(ReadFile(kLog), "MODE_ENTER id=0x01654C02 previous=0x01654C01") == 1, "mode enter logged");

			ChiefObserver::Dispose();
			EXPECT(Count(ReadFile(kLog), "FINAL calls=") == 1, "final statistics written");
			std::printf("  total log lines: %d\n", Count(ReadFile(kLog), "\n"));
		}
		else {
			// Event budget: 6 event lines at most, even with endless leader changes.
			Win().tick = 1000;
			for (int i = 0; i < 100; ++i) {
				tribe.mpLeader.p = (i % 2) ? static_cast<Simulator::cCreatureBase*>(&chief1) : static_cast<Simulator::cCreatureBase*>(&chief2);
				Call((void*)static_cast<Simulator::cCreatureBase*>(&chief1));
			}
			Win().tick = 20000;
			Call((void*)0x20000);
			std::string l = ReadFile(kLog);
			int events = Count(l, "TRIBE ") + Count(l, "LEADER ") + Count(l, "CHIEF_DETECTED") + Count(l, "MISMATCH rawLeader") + Count(l, "MODE_");
			EXPECT(events <= 6, "event lines never exceed max_event_lines");
			EXPECT(Count(l, "suppressedEventLines=") >= 1 && l.find("suppressedEventLines=0") == std::string::npos, "suppressed events are reported in the statistics");
			EXPECT(g_originalCalls.size() == 101, "original executed on every call");
		}
	}
	else {
		std::printf("unknown scenario\n");
		return 2;
	}
	std::printf("%d checks, %d failed\n", g_total, g_fail);
	return g_fail == 0 ? 0 : 1;
}
