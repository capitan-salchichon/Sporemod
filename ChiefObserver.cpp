// ChiefObserver.cpp
// Prototype 0.1 -- identification of the player's chieftain from a ForwardWalkTree observer.
//
// SAFETY CONTRACT of this prototype:
//   * The detour ALWAYS calls the original function with the original arguments.
//   * It never skips WalkTree, never touches behavior trees, orders, locomotion, animations,
//     the avatar (no SetAvatar) or saved games.
//   * The detour is only attached if the thunk address configured in the .ini passes verification
//     (inside the main module, executable memory, bytes equal to the configured ones).
//   * Creature pointers are only compared and logged as numbers; they are never dereferenced
//     and never kept as live references between calls.
#include "stdafx.h"
#include "ChiefObserver.h"
#include "ObserverCore.h"

#include <Spore\Simulator\cCreatureCitizen.h>
#include <Spore\Simulator\cTribe.h>
#include <Spore\Simulator\SubSystem\GameNounManager.h>
#include <Spore\App\IGameModeManager.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	// ---------------------------------------------------------------------------------------------
	// Configuration (TribalChiefObserver.ini, next to this DLL)
	// ---------------------------------------------------------------------------------------------
	struct Config
	{
		bool iniFound = false;
		uint32_t thunkVA = 0;                 // VA of ForwardWalkTree in the TARGET exe, image base 0x400000 (as shown by Ghidra/IDA)
		std::vector<uint8_t> expectedBytes;   // first bytes of the thunk, supplied by the user after checking the exe
		uint32_t probeBytes = 32;             // bytes dumped to the log for manual verification (0..64)
		uint32_t statsIntervalMs = 10000;
		uint32_t maxEventLines = 200;
		std::string iniPath;
		std::string logPath;
	};

	constexpr uint32_t kPreferredImageBase = 0x400000;   // same convention as ModAPI's Address() macro

	Config gCfg;
	HMODULE gModule = nullptr;

	// ---------------------------------------------------------------------------------------------
	// Logging
	// ---------------------------------------------------------------------------------------------
	CRITICAL_SECTION gLogCS;
	bool gLogCSReady = false;
	FILE* gLog = nullptr;

	tco::EventBudget gEvents(200);
	tco::IntervalGate gGate(10000);

	void LogRaw(const char* line)
	{
		if (gLogCSReady) EnterCriticalSection(&gLogCS);
		if (gLog) {
			std::fprintf(gLog, "[%10lu] %s\n", (unsigned long)GetTickCount(), line);
			std::fflush(gLog);
		}
		if (gLogCSReady) LeaveCriticalSection(&gLogCS);
		OutputDebugStringA("TribalChiefObserver: ");
		OutputDebugStringA(line);
		OutputDebugStringA("\n");
	}

	// Always written (banner, verification, periodic statistics).
	void Log(const char* fmt, ...)
	{
		char buf[768];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		LogRaw(buf);
	}

	// Subject to the event budget (so that a misbehaving situation cannot flood the log).
	void Event(const char* fmt, ...)
	{
		if (!gEvents.Take()) return;
		char buf[768];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		LogRaw(buf);
	}

	std::string DirectoryOf(HMODULE module)
	{
		char path[MAX_PATH] = {};
		DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
		std::string p(path, n);
		size_t slash = p.find_last_of("\\/");
		return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
	}

	std::string ReadIniString(const char* key, const char* def)
	{
		char buf[1024] = {};
		GetPrivateProfileStringA("Observer", key, def, buf, (DWORD)sizeof(buf), gCfg.iniPath.c_str());
		return std::string(buf);
	}

	void LoadConfig(HMODULE module)
	{
		const std::string dir = DirectoryOf(module);
		gCfg.iniPath = dir + "TribalChiefObserver.ini";
		DWORD attrs = GetFileAttributesA(gCfg.iniPath.c_str());
		gCfg.iniFound = (attrs != INVALID_FILE_ATTRIBUTES);

		std::string logName = ReadIniString("log_file", "TribalChiefObserver.log");
		gCfg.logPath = (logName.find_first_of("\\/:") == std::string::npos) ? dir + logName : logName;

		gCfg.statsIntervalMs = (uint32_t)GetPrivateProfileIntA("Observer", "stats_interval_ms", 10000, gCfg.iniPath.c_str());
		if (gCfg.statsIntervalMs < 1000) gCfg.statsIntervalMs = 1000;
		gCfg.maxEventLines = (uint32_t)GetPrivateProfileIntA("Observer", "max_event_lines", 200, gCfg.iniPath.c_str());
		gCfg.probeBytes = (uint32_t)GetPrivateProfileIntA("Observer", "probe_bytes", 32, gCfg.iniPath.c_str());
		if (gCfg.probeBytes > 64) gCfg.probeBytes = 64;

		uint32_t va = 0;
		if (tco::ParseAddress(ReadIniString("thunk_address", ""), va)) gCfg.thunkVA = va;
		tco::ParseHexBytes(ReadIniString("expected_bytes", ""), gCfg.expectedBytes);
	}

	void OpenLog()
	{
		gLog = _fsopen(gCfg.logPath.c_str(), "a", _SH_DENYWR);
	}

	// ---------------------------------------------------------------------------------------------
	// Address checks
	// ---------------------------------------------------------------------------------------------
	struct ImageInfo { uintptr_t base = 0; uint32_t size = 0; uint32_t preferredBase = 0; bool ok = false; };

	ImageInfo GetMainImageInfo()
	{
		ImageInfo info;
		HMODULE h = GetModuleHandleA(nullptr);
		if (!h) return info;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)h;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return info;
		const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)h + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return info;
		info.base = (uintptr_t)h;
		info.size = nt->OptionalHeader.SizeOfImage;
		info.preferredBase = nt->OptionalHeader.ImageBase;
		info.ok = true;
		return info;
	}

	bool IsReadableExecutable(const void* p, size_t len)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT) return false;
		const DWORD prot = mbi.Protect & 0xFF;
		const bool exec = prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
		if (!exec) return false;
		const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		return regionEnd >= (uintptr_t)p + len;
	}

	// ---------------------------------------------------------------------------------------------
	// Observer state
	// ---------------------------------------------------------------------------------------------
	std::atomic<bool> gHookAttached(false);
	std::atomic<bool> gDisabled(false);          // set after the first fault inside the observer
	std::atomic_flag gBusy = ATOMIC_FLAG_INIT;   // protects the slow path; if busy the sample is skipped

	tco::Stats gStats;
	tco::ValueTracker gTribeTracker;
	tco::ValueTracker gLeaderTracker;
	uintptr_t gLastChiefLogged = 0;              // number only, never dereferenced
	uintptr_t gLastMismatchRaw = 0, gLastMismatchCit = 0;
	tco::IdentityInput gLastInput = { 0, 0, 0, 0 };

	void PeriodicStats(const char* tag)
	{
		const uint32_t others = gStats.calls - gStats.chiefCalls;
		Log("%s calls=%u (+%u this window) chiefCalls=%u (+%u) otherCreatures=%u tribeUnavailable=%u leaderUnavailable=%u "
			"rawVsCitizenMismatch=%u | tribe=%s rawLeader=%s citizenLeader=%s | suppressedEventLines=%u",
			tag, gStats.calls, gStats.windowCalls, gStats.chiefCalls, gStats.windowChiefCalls, others,
			gStats.tribeUnavailable, gStats.leaderUnavailable, gStats.rawCitizenMismatch,
			tco::FormatPtr(gLastInput.tribe).c_str(), tco::FormatPtr(gLastInput.rawLeader).c_str(),
			tco::FormatPtr(gLastInput.citizenLeader).c_str(), gEvents.Suppressed());
		gStats.ResetWindow();
	}

	// Runs under gBusy. Updates counters and writes the (rate limited) log lines.
	void RecordAndReport(const tco::IdentityInput& in, const tco::IdentityResult& r)
	{
		gStats.Record(r);
		gLastInput = in;

		// Tribe availability / identity changes (entering/leaving the Tribal stage, loading a game).
		const tco::ValueTracker::Change tc = gTribeTracker.Observe(in.tribe);
		if (tc != tco::ValueTracker::Change::None) {
			if (tc == tco::ValueTracker::Change::Disappeared || tc == tco::ValueTracker::Change::Changed) gLastChiefLogged = 0;
			Event("TRIBE %s: tribe=%s available=%d", tco::ChangeName(tc), tco::FormatPtr(in.tribe).c_str(), r.tribeAvailable ? 1 : 0);
		}

		// Leader (chieftain) changes: replacement, death, loading a game.
		const uintptr_t leaderNow = in.citizenLeader ? in.citizenLeader : in.rawLeader;
		const tco::ValueTracker::Change lc = gLeaderTracker.Observe(leaderNow);
		if (lc != tco::ValueTracker::Change::None)
			Event("LEADER %s: citizenLeader=%s rawLeader=%s tribe=%s", tco::ChangeName(lc),
				tco::FormatPtr(in.citizenLeader).c_str(), tco::FormatPtr(in.rawLeader).c_str(), tco::FormatPtr(in.tribe).c_str());

		// cCreatureBase* vs cCreatureCitizen* pointers of the leader differ (pointer adjustment or layout problem).
		if (r.rawCitizenDiffer && (in.rawLeader != gLastMismatchRaw || in.citizenLeader != gLastMismatchCit)) {
			gLastMismatchRaw = in.rawLeader;
			gLastMismatchCit = in.citizenLeader;
			Event("MISMATCH rawLeader=%s citizenLeader=%s (mpLeader.get() != GetLeaderCitizen())",
				tco::FormatPtr(in.rawLeader).c_str(), tco::FormatPtr(in.citizenLeader).c_str());
		}

		// The creature running the tree is the chieftain: log once per distinct leader pointer.
		if (r.isChief) {
			const uintptr_t key = r.matchCitizen ? in.citizenLeader : in.rawLeader;
			if (key != gLastChiefLogged) {
				gLastChiefLogged = key;
				Event("CHIEF_DETECTED creature=%s rawLeader=%s citizenLeader=%s matchRaw=%d matchCitizen=%d tribe=%s "
					"tribeAvailable=%d detections=%u",
					tco::FormatPtr(in.creature).c_str(), tco::FormatPtr(in.rawLeader).c_str(), tco::FormatPtr(in.citizenLeader).c_str(),
					r.matchRaw ? 1 : 0, r.matchCitizen ? 1 : 0, tco::FormatPtr(in.tribe).c_str(),
					r.tribeAvailable ? 1 : 0, gStats.chiefCalls);
			}
		}

		if (gGate.Due(GetTickCount())) PeriodicStats("STATS");
	}

	void DisableAfterFault()
	{
		if (!gDisabled.exchange(true)) Log("FAULT inside the observer: it has been disabled for the rest of the session (the game is not affected).");
	}

	void ObserveImpl(void* creature)
	{
		// Read-only queries. In ModAPI GetPlayerTribe() is a field read (mpPlayerTribe) and Get() is the
		// game's singleton getter; GetLeaderCitizen() reads mpLeader and calls Cast() on that object.
		Simulator::cGameNounManager* mgr = Simulator::cGameNounManager::Get();
		Simulator::cTribe* tribe = mgr ? mgr->GetPlayerTribe() : nullptr;
		void* rawLeader = nullptr;
		void* citizenLeader = nullptr;
		if (tribe) {
			rawLeader = (void*)tribe->mpLeader.get();
			citizenLeader = (void*)tribe->GetLeaderCitizen();
		}

		const tco::IdentityInput in = { (uintptr_t)creature, (uintptr_t)tribe, (uintptr_t)rawLeader, (uintptr_t)citizenLeader };
		const tco::IdentityResult r = tco::Classify(in);

		if (gBusy.test_and_set(std::memory_order_acquire)) return;   // another call is reporting: skip this sample
		try {
			RecordAndReport(in, r);
		} catch (...) {
			gDisabled = true;
		}
		gBusy.clear(std::memory_order_release);
	}

	// No C++ objects with destructors in this function, so __try/__except is allowed here.
	void ObserveSafely(void* creature)
	{
		if (gDisabled.load(std::memory_order_relaxed)) return;
		__try {
			ObserveImpl(creature);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			DisableAfterFault();
		}
	}
}

// -------------------------------------------------------------------------------------------------
// The detour. Signature taken from the byte-exact decompilation of 0x00D995F0
// (mangled name ?ForwardWalkTree@@YAXMNPAX00000@Z): void __cdecl (float, double, 6 x void*).
// 4th argument = the creature running the tree (in cSPCreatureCitizen::Update it is `this`).
// -------------------------------------------------------------------------------------------------
static_detour(ForwardWalkTree_detour, void(float, double, void*, void*, void*, void*, void*, void*)) {};

// ModAPI declares detour_pointer as Result(*)(Parameters...), i.e. the compiler's default calling convention.
// This assertion fails to compile if the project is built with a non-cdecl default (/Gz, /Gr, /Gv).
static_assert(std::is_same<ForwardWalkTree_detour::detour_pointer,
	void(__cdecl*)(float, double, void*, void*, void*, void*, void*, void*)>::value,
	"ForwardWalkTree must be detoured with the __cdecl calling convention");

void ForwardWalkTree_detour::DETOUR(float f, double t, void* a3, void* creature, void* a5, void* a6, void* a7, void* a8)
{
	ObserveSafely(creature);
	// Prototype 0.1: the original function is ALWAYS executed, with the original arguments.
	original_function(f, t, a3, creature, a5, a6, a7, a8);
}

// -------------------------------------------------------------------------------------------------
// Game mode logger (scenarios E and F: leaving the Tribal stage, loading a game)
// -------------------------------------------------------------------------------------------------
class ModeLogListener
	: public App::IMessageListener
	, public DefaultRefCounted
{
public:
	int AddRef() override { return DefaultRefCounted::AddRef(); }
	int Release() override { return DefaultRefCounted::Release(); }

	bool HandleMessage(uint32_t messageID, void* message) override
	{
		if (messageID == App::kMsgOnModeEnter) {
			App::OnModeEnterMessage* m = (App::OnModeEnterMessage*)message;
			Event("MODE_ENTER id=0x%08X previous=0x%08X (tribe stage = 0x%08X)", (unsigned)m->GetModeID(), (unsigned)m->GetPreviousModeID(), (unsigned)GameModeIDs::kGameTribe);
		}
		else if (messageID == App::kMsgOnModeExit) {
			App::OnModeExitMessage* m = (App::OnModeExitMessage*)message;
			Event("MODE_EXIT id=0x%08X previous=0x%08X", (unsigned)m->GetModeID(), (unsigned)m->GetPreviousModeID());
		}
		return false;   // we only observe; other listeners are unaffected either way
	}
};

// -------------------------------------------------------------------------------------------------
// Public entry points
// -------------------------------------------------------------------------------------------------
namespace ChiefObserver
{
	void Attach(HMODULE module)
	{
		gModule = module;
		InitializeCriticalSection(&gLogCS);
		gLogCSReady = true;

		LoadConfig(module);
		gEvents = tco::EventBudget(gCfg.maxEventLines);
		gGate = tco::IntervalGate(gCfg.statsIntervalMs);
		OpenLog();

		Log("=== TribalChiefObserver prototype 0.1 (observer only) ===");
		Log("config file: %s (%s)", gCfg.iniPath.c_str(), gCfg.iniFound ? "found" : "NOT found");
		Log("log file: %s", gCfg.logPath.c_str());

		const ImageInfo img = GetMainImageInfo();
		Log("main module: base=%s size=0x%X preferredBase=%s | ModAPI baseAddress=%s",
			tco::FormatPtr(img.base).c_str(), (unsigned)img.size, tco::FormatPtr(img.preferredBase).c_str(), tco::FormatPtr(baseAddress).c_str());

		if (gCfg.thunkVA == 0) {
			Log("PROBE MODE: thunk_address is not configured, so NO detour is installed. See README.md to locate and verify ForwardWalkTree in your executable.");
			return;
		}
		if (!img.ok) { Log("ABORT: cannot read the main module headers. No detour installed."); return; }
		if (baseAddress != img.base) {
			Log("ABORT: ModAPI baseAddress (%s) differs from the main module base (%s). No detour installed.",
				tco::FormatPtr(baseAddress).c_str(), tco::FormatPtr(img.base).c_str());
			return;
		}

		// Same convention as ModAPI's Address(): the configured value is a VA of the image linked at 0x400000.
		const uintptr_t runtimeAddr = img.base + gCfg.thunkVA - kPreferredImageBase;
		Log("thunk_address (VA @0x400000)=%s -> runtime address=%s", tco::FormatPtr(gCfg.thunkVA).c_str(), tco::FormatPtr(runtimeAddr).c_str());

		const size_t probeLen = gCfg.expectedBytes.size() > gCfg.probeBytes ? gCfg.expectedBytes.size() : gCfg.probeBytes;
		const bool inside = runtimeAddr >= img.base && runtimeAddr + probeLen <= img.base + img.size;
		if (!inside) { Log("ABORT: the address (+%u bytes) is outside the main module image. No detour installed.", (unsigned)probeLen); return; }
		if (!IsReadableExecutable((const void*)runtimeAddr, probeLen)) { Log("ABORT: the address is not readable executable memory. No detour installed."); return; }

		if (gCfg.probeBytes > 0) {
			std::string dump = tco::BytesToHex((const uint8_t*)runtimeAddr, gCfg.probeBytes);
			Log("bytes at the candidate address (%u): %s", (unsigned)gCfg.probeBytes, dump.c_str());
		}

		const tco::VerifyResult v = tco::VerifyBytes((const uint8_t*)runtimeAddr, probeLen, gCfg.expectedBytes);
		Log("verification of expected_bytes (%u configured): %s", (unsigned)gCfg.expectedBytes.size(), tco::VerifyResultName(v));
		if (v != tco::VerifyResult::Ok) {
			Log("No detour installed (unverified address). Configure expected_bytes (>= %u bytes) after confirming the function in your executable.", (unsigned)tco::kMinExpectedBytes);
			return;
		}

		const LONG rc = ForwardWalkTree_detour::attach((UINT)runtimeAddr);
		if (rc != NO_ERROR) { Log("DetourAttach FAILED (error %ld). No detour installed.", (long)rc); return; }
		gHookAttached = true;
		Log("DetourAttach OK: the observer will be active once CommitDetours() succeeds.");
	}

	void Initialize()
	{
		MessageManager.AddListener(new ModeLogListener(), App::kMsgOnModeEnter);
		MessageManager.AddListener(new ModeLogListener(), App::kMsgOnModeExit);
		Log("Initialize: hookAttached=%d (mode listeners registered)", gHookAttached.load() ? 1 : 0);
	}

	void Dispose()
	{
		Log("Dispose: hookAttached=%d disabledByFault=%d", gHookAttached.load() ? 1 : 0, gDisabled.load() ? 1 : 0);
		// Never block at exit: if the slow path is busy (or was left locked by a fault) just report without the lock.
		const bool locked = !gBusy.test_and_set(std::memory_order_acquire);
		PeriodicStats("FINAL");
		if (locked) gBusy.clear(std::memory_order_release);
	}
}
