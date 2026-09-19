// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "GitStaleLocks.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SWindow;

DECLARE_DELEGATE_OneParam(FOnReleaseStaleLocks, const TArray<FGitStaleLock>& /*Locks*/);

/** One row of the stale locks list */
struct FGitStaleLockItem
{
	FGitStaleLock Lock;
	bool bChecked = true;
};

/** Checkable list of stale Git LFS locks, to release the selected ones */
class SGitStaleLocksDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitStaleLocksDialog) {}
		SLATE_ARGUMENT(TArray<FGitStaleLock>, StaleLocks)
		SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
		SLATE_EVENT(FOnReleaseStaleLocks, OnRelease)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FGitStaleLockItem> InItem, const TSharedRef<STableViewBase>& OwnerTable);

	ECheckBoxState GetAllCheckedState() const;
	void OnAllCheckedStateChanged(ECheckBoxState InNewState);

	int32 GetNumChecked() const;
	FText GetReleaseButtonText() const;
	bool IsReleaseEnabled() const;

	FReply OnReleaseClicked();
	FReply OnKeepClicked();

	TArray<TSharedPtr<FGitStaleLockItem>> Items;
	TSharedPtr<SListView<TSharedPtr<FGitStaleLockItem>>> ListView;
	TWeakPtr<SWindow> ParentWindow;
	FOnReleaseStaleLocks OnRelease;
};
