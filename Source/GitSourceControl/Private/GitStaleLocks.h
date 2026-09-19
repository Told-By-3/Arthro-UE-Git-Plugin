// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"

/**
 * A Git LFS lock owned by the current user on a file that has no local work in progress:
 * no uncommitted change (staged or not) and no change in a local commit that hasn't been pushed yet.
 * Typically left behind when content is committed & pushed outside of the Editor (Rider, CLI...),
 * since only the Editor's own check-in releases the locks.
 */
struct FGitStaleLock
{
	/** Git LFS lock ID */
	FString Id;
	/** Path relative to the root of the Git repository, as reported by Git LFS */
	FString Path;
	/** When the lock was taken */
	FDateTime LockedAt;
};

namespace GitStaleLocks
{
	/**
	 * Query the LFS server for the current user's locks, and keep those on files without local work in progress.
	 * Blocking (network + several git processes): call from a background thread.
	 */
	bool FindStaleLocks(const FString& InPathToGitBinary, const FString& InRepositoryRoot, TArray<FGitStaleLock>& OutStaleLocks, TArray<FString>& OutErrorMessages);

	/**
	 * Release the given locks, then query the server again to find out which ones were actually released.
	 * Blocking: call from a background thread.
	 */
	void ReleaseLocks(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FGitStaleLock>& InLocks, TArray<FGitStaleLock>& OutReleased, TArray<FString>& OutErrorMessages);

	/**
	 * Look for stale locks in the background, and open the release dialog if any are found.
	 * @param bIsStartupCheck	Automatic check on Editor launch: silent when nothing is found, and skipped if the user opted out.
	 */
	void CheckForStaleLocks(bool bIsStartupCheck);

	/** Per-user opt-out of the automatic check on Editor launch */
	bool IsStartupCheckEnabled();
	void SetStartupCheckEnabled(bool bEnabled);
}
