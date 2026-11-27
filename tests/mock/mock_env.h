// mock_env.h -- MINIMAL stand-ins for <windows.h> and the Spore ModAPI, used ONLY by the Linux simulation
// harness (run_mock_tests.sh). They let the real ChiefObserver.cpp be compiled and exercised without MSVC.
// They are NOT the real APIs: this harness validates the prototype's own logic, not the ModAPI integration.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

// ------------------------------ Windows -------------------------------------------------------------
typedef void* HMODULE;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef int BOOL;
#define APIENTRY
#define MAX_PATH 260
#define NO_ERROR 0
#define __cdecl
#undef __try
#define __try if (true)
#define __except(x) if (false)
#define EXCEPTION_EXECUTE_HANDLER 1
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define _SH_DENYWR 0
#define MEM_COMMIT 0x1000
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
struct IMAGE_DOS_HEADER { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; };
struct IMAGE_NT_HEADERS32 { DWORD Signature; struct { DWORD ImageBase; DWORD SizeOfImage; } OptionalHeader; };
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; size_t RegionSize; DWORD State; DWORD Protect; };
struct CRITICAL_SECTION { int dummy; };

struct MockWin {
	uint32_t tick = 1000;
	std::string dllPath = "/tmp/mock_mod/TribalChiefObserver.dll";
	uint8_t* image = nullptr;           // fake main module (mapped below 4 GB so that 32-bit casts are lossless)
	uint32_t imageSize = 0x4000;
	bool executable = true;
};
inline MockWin& Win() { static MockWin w; return w; }

inline void MockInitImage()
{
	MockWin& w = Win();
	if (w.image) return;
	void* p = mmap(nullptr, w.imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	w.image = (uint8_t*)p;
	std::memset(w.image, 0, w.imageSize);
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)w.image;
	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = 0x80;
	IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(w.image + 0x80);
	nt->Signature = IMAGE_NT_SIGNATURE;
	nt->OptionalHeader.ImageBase = 0x400000;
	nt->OptionalHeader.SizeOfImage = w.imageSize;
}

inline HMODULE GetModuleHandleA(const char*) { MockInitImage(); return (HMODULE)Win().image; }
inline DWORD GetModuleFileNameA(HMODULE, char* buf, DWORD size)
{
	const std::string& p = Win().dllPath;
	DWORD n = (DWORD)(p.size() < size ? p.size() : size - 1);
	std::memcpy(buf, p.data(), n);
	buf[n] = 0;
	return n;
}
inline DWORD GetFileAttributesA(const char* path) { FILE* f = std::fopen(path, "rb"); if (!f) return INVALID_FILE_ATTRIBUTES; std::fclose(f); return 0; }
inline DWORD GetTickCount() { return Win().tick; }
inline void OutputDebugStringA(const char*) {}
inline void InitializeCriticalSection(CRITICAL_SECTION*) {}
inline void EnterCriticalSection(CRITICAL_SECTION*) {}
inline void LeaveCriticalSection(CRITICAL_SECTION*) {}
inline FILE* _fsopen(const char* path, const char* mode, int) { return std::fopen(path, mode); }
inline size_t VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* mbi, size_t)
{
	MockWin& w = Win();
	uintptr_t a = (uintptr_t)p, b = (uintptr_t)w.image;
	if (a < b || a >= b + w.imageSize) return 0;
	mbi->BaseAddress = w.image; mbi->RegionSize = w.imageSize; mbi->State = MEM_COMMIT;
	mbi->Protect = w.executable ? PAGE_EXECUTE_READ : 0x02;
	return sizeof(*mbi);
}

// Tiny .ini reader: [Observer] key=value, ';' comments.
inline std::string MockIniGet(const char* key, const char* path, bool& found)
{
	found = false;
	std::ifstream in(path);
	std::string line;
	bool inSection = false;
	while (std::getline(in, line)) {
		size_t c = line.find(';'); if (c != std::string::npos) line = line.substr(0, c);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
		if (line.empty()) continue;
		if (line[0] == '[') { inSection = (line == "[Observer]"); continue; }
		if (!inSection) continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		while (!k.empty() && k.back() == ' ') k.pop_back();
		while (!v.empty() && v.front() == ' ') v.erase(v.begin());
		if (k == key) { found = true; return v; }
	}
	return std::string();
}
inline DWORD GetPrivateProfileStringA(const char*, const char* key, const char* def, char* buf, DWORD size, const char* path)
{
	bool found; std::string v = MockIniGet(key, path, found);
	if (!found) v = def;
	DWORD n = (DWORD)(v.size() < size ? v.size() : size - 1);
	std::memcpy(buf, v.data(), n); buf[n] = 0; return n;
}
inline UINT GetPrivateProfileIntA(const char*, const char* key, int def, const char* path)
{
	bool found; std::string v = MockIniGet(key, path, found);
	return found ? (UINT)std::strtol(v.c_str(), nullptr, 10) : (UINT)def;
}

// ------------------------------ Spore ModAPI ---------------------------------------------------------
extern uintptr_t baseAddress;

template <class D, class Sig> struct static_detour_;
template <class D, class R, class... P> struct static_detour_<D, R(P...)> {
	typedef R (*detour_pointer)(P...);
	typedef static_detour_<D, R(P...)> detour_object;
	static detour_pointer original_function;
	static detour_pointer test_original;            // harness only: what "attach" installs as the original
	static uintptr_t attached_address;              // harness only
	static R detoured_function(P... args) { int fake; return ((D*)&fake)->detoured(args...); }
	R detoured(P...);
	static LONG attach(UINT function_address) { attached_address = function_address; original_function = test_original; return 0; }
};
template <class D, class R, class... P> typename static_detour_<D, R(P...)>::detour_pointer static_detour_<D, R(P...)>::original_function = nullptr;
template <class D, class R, class... P> typename static_detour_<D, R(P...)>::detour_pointer static_detour_<D, R(P...)>::test_original = nullptr;
template <class D, class R, class... P> uintptr_t static_detour_<D, R(P...)>::attached_address = 0;
#define static_detour(name, declaration) struct name : public static_detour_<name, declaration>


// member_detour (used by the cCreatureCitizen::Update hook) and the ModAPI address table access macros
template <class D, class Base, class Virt, class Sig> struct member_detour_;
template <class D, class Base, class Virt, class R, class... P>
struct member_detour_<D, Base, Virt, R(P...)> : public Base {
	typedef R (*detour_pointer)(Virt*, P...);
	static detour_pointer original_function;
	static detour_pointer test_original;            // harness only
	static uintptr_t attached_address;              // harness only
	static R detoured_function(Virt* object, P... args) { return ((D*)object)->detoured(args...); }
	R detoured(P...);
	static LONG attach(UINT function_address) { attached_address = function_address; original_function = test_original; return 0; }
};
template <class D, class B, class V, class R, class... P> typename member_detour_<D, B, V, R(P...)>::detour_pointer member_detour_<D, B, V, R(P...)>::original_function = nullptr;
template <class D, class B, class V, class R, class... P> typename member_detour_<D, B, V, R(P...)>::detour_pointer member_detour_<D, B, V, R(P...)>::test_original = nullptr;
template <class D, class B, class V, class R, class... P> uintptr_t member_detour_<D, B, V, R(P...)>::attached_address = 0;
#define member_detour(name, baseClass, declaration) struct name : public member_detour_<name, baseClass, baseClass, declaration>
#define Addresses(name) name##_addresses
#define GetAddress(namespaceName, name) (Addresses(namespaceName)::name + baseAddress)
namespace Simulator { namespace cCreatureCitizen_addresses { const uintptr_t Update = 0x1000; } }   // offset from the module base

template <class T> struct mock_intrusive_ptr { T* p = nullptr; T* get() const { return p; } };

namespace Simulator {
	struct cCreatureBase { virtual ~cCreatureBase() {} int baseData[2] = { 0, 0 }; };
	struct Pad { virtual ~Pad() {} int data[6]; };   // makes cCreatureCitizen* differ from cCreatureBase* (pointer adjustment)
	struct cCreatureCitizen : Pad, cCreatureBase {};
	struct cCommunity {
		mock_intrusive_ptr<cCreatureBase> mpLeader;
		cCreatureCitizen* GetLeaderCitizen() { return dynamic_cast<cCreatureCitizen*>(mpLeader.get()); }
		virtual ~cCommunity() {}
	};
	struct cTribe : cCommunity {};
	struct cGameNounManager {
		cTribe* mpPlayerTribe = nullptr;
		static cGameNounManager* Get() { static cGameNounManager m; return &m; }
		cTribe* GetPlayerTribe() { return mpPlayerTribe; }
	};
}

enum GameModeIDs { kGameCreature = 0x1654C01, kGameTribe = 0x1654C02 };
namespace App {
	enum { kMsgOnModeExit = 0x212D3E7, kMsgOnModeEnter = 0x22D1ADC };
	struct IMessageListener { virtual ~IMessageListener() {} virtual int AddRef() = 0; virtual int Release() = 0; virtual bool HandleMessage(uint32_t, void*) = 0; };
	struct OnModeEnterMessage { uint32_t params[2]; uint32_t GetModeID() { return params[0]; } uint32_t GetPreviousModeID() { return params[1]; } };
	struct OnModeExitMessage { uint32_t params[2]; uint32_t GetModeID() { return params[0]; } uint32_t GetPreviousModeID() { return params[1]; } };
}
struct DefaultRefCounted { int refs = 0; int AddRef() { return ++refs; } int Release() { return --refs; } };
struct MockMessageManager {
	std::vector<std::pair<App::IMessageListener*, uint32_t>> listeners;
	void AddListener(App::IMessageListener* l, uint32_t id) { listeners.push_back({ l, id }); }
};
inline MockMessageManager& MsgMgr() { static MockMessageManager m; return m; }
#define MessageManager MsgMgr()
