// ChiefObserver.h
// Prototype 0.1: observes ForwardWalkTree and identifies the player's chieftain.
// It does NOT skip WalkTree, change orders, locomotion, animations or the avatar.
#pragma once

namespace ChiefObserver
{
	// Called from AttachDetours() (inside DllMain, between PrepareDetours/CommitDetours).
	// Reads the .ini, verifies the thunk address and, only if verification passes, attaches the detour.
	void Attach(HMODULE module);

	// Called after the game has initialized (ModAPI::AddPostInitFunction). Registers the mode logger.
	void Initialize();

	// Called when the game closes. Writes the final statistics.
	void Dispose();
}
