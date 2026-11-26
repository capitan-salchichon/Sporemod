// dllmain.cpp : Defines the entry point for the DLL application.
// Structure taken from the ModAPI "TemplatesExample" project.
#include "stdafx.h"
#include "ChiefObserver.h"

static HMODULE gThisModule = nullptr;

void Initialize()
{
	// Executed when the game starts, before the user interface is shown.
	ChiefObserver::Initialize();
}

void Dispose()
{
	// Called when the game is closing.
	ChiefObserver::Dispose();
}

void AttachDetours()
{
	// The detour is attached here, as required by the ModAPI detour documentation.
	// The observer only attaches it if the thunk address passes verification (see ChiefObserver.cpp).
	ChiefObserver::Attach(gThisModule);
}


// Generally, you don't need to touch any code here
BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		gThisModule = hModule;
		ModAPI::AddPostInitFunction(Initialize);
		ModAPI::AddDisposeFunction(Dispose);

		PrepareDetours(hModule);
		AttachDetours();
		CommitDetours();
		break;

	case DLL_PROCESS_DETACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
