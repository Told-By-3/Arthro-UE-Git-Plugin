// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitStaleLocks.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlUtils.h"
#include "SGitStaleLocksDialog.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Logging/MessageLog.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SourceControlOperations.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace
{
	const TCHAR* StaleLocksConfigSection = TEXT("GitSourceControl.StaleLocks");
	const TCHAR* StartupCheckConfigKey = TEXT("bCheckOnStartup");

	/** A check (or a release) is running in the background */
	bool bOperationInProgress = false;

	TWeakPtr<SWindow> DialogWindow;

	/** All locks owned by the current user, as seen by the LFS server */
	bool QueryOwnLocks(const FString& InPathToGitBinary, const FString& InRepositoryRoot, TArray<FGitStaleLock>& OutLocks, TArray<FString>& OutErrorMessages)
	{
		// --verify asks the server which locks are ours, instead of relying on the configured LFS user name
		const TArray<FString> Parameters{TEXT("--verify"), TEXT("--json")};
		TArray<FString> Results;
		if (!GitSourceControlUtils::RunLFSCommand(TEXT("locks"), InRepositoryRoot, InPathToGitBinary, Parameters, FGitSourceControlModule::GetEmptyStringArray(), Results, OutErrorMessages))
		{
			return false;
		}

		// RunCommand appends stderr to the results on success: keep only the JSON object, without any warning around it
		const FString Output = FString::Join(Results, TEXT("\n"));
		const int32 JsonStart = Output.Find(TEXT("{"));
		const int32 JsonEnd = Output.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create((JsonStart != INDEX_NONE && JsonEnd > JsonStart) ? Output.Mid(JsonStart, JsonEnd - JsonStart + 1) : Output);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			OutErrorMessages.Add(TEXT("Unable to parse the output of 'git lfs locks --verify --json'"));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* OwnLocks = nullptr;
		if (JsonObject->TryGetArrayField(TEXT("ours"), OwnLocks))
		{
			for (const TSharedPtr<FJsonValue>& Value : *OwnLocks)
			{
				const TSharedPtr<FJsonObject>* LockObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(LockObject))
				{
					continue;
				}
				FGitStaleLock Lock;
				(*LockObject)->TryGetStringField(TEXT("id"), Lock.Id);
				(*LockObject)->TryGetStringField(TEXT("path"), Lock.Path);
				FString LockedAt;
				if ((*LockObject)->TryGetStringField(TEXT("locked_at"), LockedAt))
				{
					FDateTime::ParseIso8601(*LockedAt, Lock.LockedAt);
				}
				if (!Lock.Id.IsEmpty() && !Lock.Path.IsEmpty())
				{
					OutLocks.Add(MoveTemp(Lock));
				}
			}
		}
		return true;
	}

	void AddPathsFromResults(const TArray<FString>& InResults, TSet<FString>& OutPaths)
	{
		for (const FString& Result : InResults)
		{
			FString Path = Result.TrimStartAndEnd();
			if (!Path.IsEmpty())
			{
				OutPaths.Add(MoveTemp(Path));
			}
		}
	}

	/** Repository-relative paths of every file with local work in progress, committed or not */
	bool GetWorkInProgressPaths(const FString& InPathToGitBinary, const FString& InRepositoryRoot, TSet<FString>& OutPaths, TArray<FString>& OutErrorMessages)
	{
		// core.quotepath=off so that non-ASCII paths come out verbatim, matching the paths reported by Git LFS
		TArray<FString> Results;

		// Uncommitted changes, staged or not, with both sides of renames
		const TArray<FString> DiffParameters{TEXT("--name-only"), TEXT("--no-renames"), TEXT("HEAD")};
		if (!GitSourceControlUtils::RunCommand(TEXT("-c core.quotepath=off diff"), InPathToGitBinary, InRepositoryRoot, DiffParameters, FGitSourceControlModule::GetEmptyStringArray(), Results, OutErrorMessages))
		{
			return false;
		}
		AddPathsFromResults(Results, OutPaths);

		// Changes committed locally but not pushed to any remote yet: those locks are still needed until the push
		Results.Reset();
		const TArray<FString> LogParameters{TEXT("--name-only"), TEXT("--no-renames"), TEXT("--format="), TEXT("HEAD"), TEXT("--not"), TEXT("--remotes")};
		if (!GitSourceControlUtils::RunCommand(TEXT("-c core.quotepath=off log"), InPathToGitBinary, InRepositoryRoot, LogParameters, FGitSourceControlModule::GetEmptyStringArray(), Results, OutErrorMessages))
		{
			return false;
		}
		AddPathsFromResults(Results, OutPaths);

		return true;
	}

	void ShowNotification(const FText& InText, SNotificationItem::ECompletionState InState)
	{
		FNotificationInfo Info(InText);
		Info.ExpireDuration = (InState == SNotificationItem::CS_Fail) ? 8.0f : 4.0f;
		Info.bUseSuccessFailIcons = true;
		const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(InState);
		}
	}

	void LogErrors(const FText& InSummary, const TArray<FString>& InErrorMessages)
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Error(InSummary);
		for (const FString& ErrorMessage : InErrorMessages)
		{
			SourceControlLog.Error(FText::FromString(ErrorMessage));
		}
	}

	void OnReleaseRequested(const TArray<FGitStaleLock>& InLocks, const FString& InPathToGitBinary, const FString& InRepositoryRoot)
	{
		if (InLocks.Num() == 0 || bOperationInProgress)
		{
			return;
		}
		bOperationInProgress = true;

		FNotificationInfo Info(FText::Format(LOCTEXT("StaleLocks_Releasing", "Releasing {0} {0}|plural(one=lock,other=locks)..."), InLocks.Num()));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		TWeakPtr<SNotificationItem> InProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (InProgressNotification.IsValid())
		{
			InProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}

		Async(EAsyncExecution::ThreadPool, [InLocks, InPathToGitBinary, InRepositoryRoot, InProgressNotification]()
		{
			TArray<FGitStaleLock> Released;
			TArray<FString> ErrorMessages;
			GitStaleLocks::ReleaseLocks(InPathToGitBinary, InRepositoryRoot, InLocks, Released, ErrorMessages);

			AsyncTask(ENamedThreads::GameThread, [NumRequested = InLocks.Num(), Released = MoveTemp(Released), ErrorMessages = MoveTemp(ErrorMessages), InRepositoryRoot, InProgressNotification]()
			{
				bOperationInProgress = false;
				if (InProgressNotification.IsValid())
				{
					InProgressNotification.Pin()->ExpireAndFadeout();
				}

				TArray<FString> ReleasedFiles;
				for (const FGitStaleLock& Lock : Released)
				{
					const FString AbsolutePath = FPaths::ConvertRelativePathToFull(InRepositoryRoot, Lock.Path);
					FGitLockedFilesCache::RemoveLockedFile(AbsolutePath);
					ReleasedFiles.Add(AbsolutePath);
				}

				FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
				if (GitSourceControl && ReleasedFiles.Num() > 0 && GitSourceControl->GetProvider().IsEnabled())
				{
					// Refresh the lock icons in the Content Browser
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 0, 0)
					GitSourceControl->GetProvider().Execute(ISourceControlOperation::Create<FUpdateStatus>(), FSourceControlChangelistPtr(), ReleasedFiles, EConcurrency::Asynchronous);
#else
					GitSourceControl->GetProvider().Execute(ISourceControlOperation::Create<FUpdateStatus>(), ReleasedFiles, EConcurrency::Asynchronous);
#endif
				}

				if (Released.Num() == NumRequested)
				{
					ShowNotification(FText::Format(LOCTEXT("StaleLocks_Released", "Released {0} stale {0}|plural(one=lock,other=locks)"), Released.Num()), SNotificationItem::CS_Success);
				}
				else
				{
					const FText Summary = FText::Format(LOCTEXT("StaleLocks_ReleasedPartially", "Released {0} of {1} stale locks, see the Revision Control log"), Released.Num(), NumRequested);
					LogErrors(Summary, ErrorMessages);
					ShowNotification(Summary, SNotificationItem::CS_Fail);
				}
			});
		});
	}

	void OpenDialog(const TArray<FGitStaleLock>& InStaleLocks, const FString& InPathToGitBinary, const FString& InRepositoryRoot)
	{
		if (const TSharedPtr<SWindow> ExistingWindow = DialogWindow.Pin())
		{
			ExistingWindow->RequestDestroyWindow();
		}

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("StaleLocks_Title", "Release Stale Git LFS Locks"))
			.ClientSize(FVector2D(820.0f, 520.0f))
			.SupportsMinimize(false)
			.SupportsMaximize(false);

		Window->SetContent(
			SNew(SGitStaleLocksDialog)
			.StaleLocks(InStaleLocks)
			.ParentWindow(Window)
			.OnRelease(FOnReleaseStaleLocks::CreateLambda([InPathToGitBinary, InRepositoryRoot](const TArray<FGitStaleLock>& Locks)
			{
				OnReleaseRequested(Locks, InPathToGitBinary, InRepositoryRoot);
			}))
		);

		const TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
		if (RootWindow.IsValid())
		{
			FSlateApplication::Get().AddWindowAsNativeChild(Window, RootWindow.ToSharedRef());
		}
		else
		{
			FSlateApplication::Get().AddWindow(Window);
		}
		DialogWindow = Window;
	}

	/** On Editor launch the check can complete before the main frame exists: wait for it so that the dialog isn't lost behind the splash screen */
	void OpenDialogWhenEditorIsReady(const TArray<FGitStaleLock>& InStaleLocks, const FString& InPathToGitBinary, const FString& InRepositoryRoot)
	{
		if (FGlobalTabmanager::Get()->GetRootWindow().IsValid())
		{
			OpenDialog(InStaleLocks, InPathToGitBinary, InRepositoryRoot);
			return;
		}

		const FTickerDelegate WaitForEditor = FTickerDelegate::CreateLambda([InStaleLocks, InPathToGitBinary, InRepositoryRoot](float)
		{
			if (!FGlobalTabmanager::Get()->GetRootWindow().IsValid())
			{
				return true; // keep waiting
			}
			OpenDialog(InStaleLocks, InPathToGitBinary, InRepositoryRoot);
			return false;
		});
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 0, 0)
		FTSTicker::GetCoreTicker().AddTicker(WaitForEditor, 1.0f);
#else
		FTicker::GetCoreTicker().AddTicker(WaitForEditor, 1.0f);
#endif
	}
}

namespace GitStaleLocks
{

bool FindStaleLocks(const FString& InPathToGitBinary, const FString& InRepositoryRoot, TArray<FGitStaleLock>& OutStaleLocks, TArray<FString>& OutErrorMessages)
{
	TArray<FGitStaleLock> OwnLocks;
	if (!QueryOwnLocks(InPathToGitBinary, InRepositoryRoot, OwnLocks, OutErrorMessages))
	{
		return false;
	}
	if (OwnLocks.Num() == 0)
	{
		return true;
	}

	TSet<FString> WorkInProgressPaths;
	if (!GetWorkInProgressPaths(InPathToGitBinary, InRepositoryRoot, WorkInProgressPaths, OutErrorMessages))
	{
		// Without knowing the work in progress, every lock could look stale: better not to offer anything
		return false;
	}

	for (FGitStaleLock& Lock : OwnLocks)
	{
		if (!WorkInProgressPaths.Contains(Lock.Path))
		{
			OutStaleLocks.Add(MoveTemp(Lock));
		}
	}
	OutStaleLocks.Sort([](const FGitStaleLock& A, const FGitStaleLock& B) { return A.Path < B.Path; });
	return true;
}

void ReleaseLocks(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FGitStaleLock>& InLocks, TArray<FGitStaleLock>& OutReleased, TArray<FString>& OutErrorMessages)
{
	TArray<FString> PathsOnDisk;
	TArray<const FGitStaleLock*> LocksWithoutFile;
	for (const FGitStaleLock& Lock : InLocks)
	{
		if (FPaths::FileExists(FPaths::ConvertRelativePathToFull(InRepositoryRoot, Lock.Path)))
		{
			PathsOnDisk.Add(Lock.Path);
		}
		else
		{
			LocksWithoutFile.Add(&Lock);
		}
	}

	TArray<FString> InfoMessages;
	bool bCommandsSucceeded = true;
	if (PathsOnDisk.Num() > 0)
	{
		// Unlock by path without --force: Git LFS refuses to unlock a modified file,
		// a last safeguard against edits made while the dialog was open
		bCommandsSucceeded &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InRepositoryRoot, InPathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), PathsOnDisk, InfoMessages, OutErrorMessages);
	}
	for (const FGitStaleLock* Lock : LocksWithoutFile)
	{
		// The file was moved or deleted since it was locked: Git LFS can't check its status, and requires --force.
		// These are only our own locks, so forcing doesn't take anyone else's lock.
		const TArray<FString> Parameters{TEXT("--force"), FString::Printf(TEXT("--id=%s"), *Lock->Id)};
		bCommandsSucceeded &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InRepositoryRoot, InPathToGitBinary, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, OutErrorMessages);
	}

	// A batch unlock doesn't say which files failed: ask the server which locks remain
	TArray<FGitStaleLock> RemainingLocks;
	if (QueryOwnLocks(InPathToGitBinary, InRepositoryRoot, RemainingLocks, OutErrorMessages))
	{
		TSet<FString> RemainingIds;
		for (const FGitStaleLock& Lock : RemainingLocks)
		{
			RemainingIds.Add(Lock.Id);
		}
		for (const FGitStaleLock& Lock : InLocks)
		{
			if (!RemainingIds.Contains(Lock.Id))
			{
				OutReleased.Add(Lock);
			}
		}
	}
	else if (bCommandsSucceeded)
	{
		OutReleased = InLocks;
	}
}

void CheckForStaleLocks(bool bIsStartupCheck)
{
	check(IsInGameThread());

	if (FApp::IsUnattended() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (bIsStartupCheck && !IsStartupCheckEnabled())
	{
		return;
	}
	if (bOperationInProgress)
	{
		if (!bIsStartupCheck)
		{
			ShowNotification(LOCTEXT("StaleLocks_InProgress", "Already checking for stale locks"), SNotificationItem::CS_None);
		}
		return;
	}

	FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return;
	}
	const FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
	if (!Provider.IsEnabled() || !Provider.UsesCheckout())
	{
		if (!bIsStartupCheck)
		{
			ShowNotification(LOCTEXT("StaleLocks_NoLocking", "Git LFS locking is not enabled"), SNotificationItem::CS_Fail);
		}
		return;
	}

	bOperationInProgress = true;
	const FString PathToGitBinary = Provider.GetGitBinaryPath();
	const FString RepositoryRoot = Provider.GetPathToGitRoot();

	Async(EAsyncExecution::ThreadPool, [bIsStartupCheck, PathToGitBinary, RepositoryRoot]()
	{
		TArray<FGitStaleLock> StaleLocks;
		TArray<FString> ErrorMessages;
		const bool bSuccess = FindStaleLocks(PathToGitBinary, RepositoryRoot, StaleLocks, ErrorMessages);

		AsyncTask(ENamedThreads::GameThread, [bIsStartupCheck, bSuccess, StaleLocks = MoveTemp(StaleLocks), ErrorMessages = MoveTemp(ErrorMessages), PathToGitBinary, RepositoryRoot]()
		{
			bOperationInProgress = false;

			if (!bSuccess)
			{
				// Offline, or no access to the LFS server: stay quiet on launch, the Editor already reports connection issues
				UE_LOG(LogSourceControl, Warning, TEXT("Unable to check for stale Git LFS locks: %s"), *FString::Join(ErrorMessages, TEXT(" ")));
				if (!bIsStartupCheck)
				{
					LogErrors(LOCTEXT("StaleLocks_CheckFailed", "Unable to check for stale Git LFS locks"), ErrorMessages);
					ShowNotification(LOCTEXT("StaleLocks_CheckFailedNotification", "Unable to check for stale locks, see the Revision Control log"), SNotificationItem::CS_Fail);
				}
				return;
			}

			UE_LOG(LogSourceControl, Log, TEXT("Found %d stale Git LFS lock(s)"), StaleLocks.Num());
			if (StaleLocks.Num() == 0)
			{
				if (!bIsStartupCheck)
				{
					ShowNotification(LOCTEXT("StaleLocks_None", "No stale locks found"), SNotificationItem::CS_Success);
				}
				return;
			}

			if (bIsStartupCheck)
			{
				OpenDialogWhenEditorIsReady(StaleLocks, PathToGitBinary, RepositoryRoot);
			}
			else
			{
				OpenDialog(StaleLocks, PathToGitBinary, RepositoryRoot);
			}
		});
	});
}

bool IsStartupCheckEnabled()
{
	bool bEnabled = true;
	GConfig->GetBool(StaleLocksConfigSection, StartupCheckConfigKey, bEnabled, GEditorPerProjectIni);
	return bEnabled;
}

void SetStartupCheckEnabled(bool bEnabled)
{
	GConfig->SetBool(StaleLocksConfigSection, StartupCheckConfigKey, bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

}

#undef LOCTEXT_NAMESPACE
