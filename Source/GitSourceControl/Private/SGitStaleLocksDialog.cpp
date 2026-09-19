// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "SGitStaleLocksDialog.h"

#include "Misc/EngineVersionComparison.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 1, 0)
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace
{
	const FName CheckColumn(TEXT("Check"));
	const FName PathColumn(TEXT("Path"));
	const FName LockedAtColumn(TEXT("LockedAt"));

	const FSlateBrush* GetStyleBrush(const FName InName)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 1, 0)
		return FAppStyle::GetBrush(InName);
#else
		return FEditorStyle::GetBrush(InName);
#endif
	}

	FText FormatLockedAt(const FDateTime& InLockedAtUtc)
	{
		if (InLockedAtUtc == FDateTime())
		{
			return FText::GetEmpty();
		}
		// Git LFS reports UTC times
		const FDateTime LocalTime = InLockedAtUtc + (FDateTime::Now() - FDateTime::UtcNow());
		return FText::AsDateTime(LocalTime, EDateTimeStyle::Short, EDateTimeStyle::Short, FText::GetInvariantTimeZone());
	}

	class SGitStaleLockRow : public SMultiColumnTableRow<TSharedPtr<FGitStaleLockItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SGitStaleLockRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable, const TSharedPtr<FGitStaleLockItem>& InItem)
		{
			Item = InItem;
			SMultiColumnTableRow<TSharedPtr<FGitStaleLockItem>>::Construct(FSuperRowType::FArguments(), InOwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnName) override
		{
			if (InColumnName == CheckColumn)
			{
				return SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return Item->bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState InNewState) { Item->bChecked = (InNewState == ECheckBoxState::Checked); })
					];
			}
			if (InColumnName == PathColumn)
			{
				return SNew(SBox)
					.VAlign(VAlign_Center)
					.Padding(FMargin(4.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(Item->Lock.Path))
						.ToolTipText(FText::Format(LOCTEXT("StaleLocks_RowTooltip", "{0}\nLock ID: {1}"), FText::FromString(Item->Lock.Path), FText::FromString(Item->Lock.Id)))
					];
			}
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(FormatLockedAt(Item->Lock.LockedAt))
				];
		}

	private:
		TSharedPtr<FGitStaleLockItem> Item;
	};
}

void SGitStaleLocksDialog::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;
	OnRelease = InArgs._OnRelease;
	for (const FGitStaleLock& Lock : InArgs._StaleLocks)
	{
		TSharedPtr<FGitStaleLockItem> Item = MakeShared<FGitStaleLockItem>();
		Item->Lock = Lock;
		Items.Add(MoveTemp(Item));
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(GetStyleBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("StaleLocks_Explanation",
					"You hold {0} Git LFS {0}|plural(one=lock,other=locks) on files without any local change, committed or not. "
					"This usually happens when changes are submitted outside of the Editor (Rider, command line...), "
					"which doesn't release the locks, and prevents others from editing these files.\n\n"
					"Release the selected locks?"), Items.Num()))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(GetStyleBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(0.0f))
				[
					SAssignNew(ListView, SListView<TSharedPtr<FGitStaleLockItem>>)
					.ListItemsSource(&Items)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SGitStaleLocksDialog::OnGenerateRow)
					.HeaderRow
					(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(CheckColumn)
						.FixedWidth(28.0f)
						.HAlignHeader(HAlign_Center)
						.VAlignHeader(VAlign_Center)
						[
							SNew(SCheckBox)
							.ToolTipText(LOCTEXT("StaleLocks_SelectAll", "Select all / none"))
							.IsChecked(this, &SGitStaleLocksDialog::GetAllCheckedState)
							.OnCheckStateChanged(this, &SGitStaleLocksDialog::OnAllCheckedStateChanged)
						]
						+ SHeaderRow::Column(PathColumn)
						.DefaultLabel(LOCTEXT("StaleLocks_PathColumn", "File"))
						.FillWidth(1.0f)
						+ SHeaderRow::Column(LockedAtColumn)
						.DefaultLabel(LOCTEXT("StaleLocks_LockedAtColumn", "Locked"))
						.FixedWidth(150.0f)
					)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([]() { return GitStaleLocks::IsStartupCheckEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState InNewState) { GitStaleLocks::SetStartupCheckEnabled(InNewState == ECheckBoxState::Checked); })
					.ToolTipText(LOCTEXT("StaleLocks_StartupTooltip", "You can always check manually from the Revision Control menu: Git > Release Stale Locks..."))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("StaleLocks_StartupCheck", "Check for stale locks when the Editor starts"))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 1, 0)
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
#endif
					.Text(this, &SGitStaleLocksDialog::GetReleaseButtonText)
					.IsEnabled(this, &SGitStaleLocksDialog::IsReleaseEnabled)
					.OnClicked(this, &SGitStaleLocksDialog::OnReleaseClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("StaleLocks_Keep", "Keep All"))
					.ToolTipText(LOCTEXT("StaleLocks_KeepTooltip", "Close without releasing any lock"))
					.OnClicked(this, &SGitStaleLocksDialog::OnKeepClicked)
				]
			]
		]
	];
}

TSharedRef<ITableRow> SGitStaleLocksDialog::OnGenerateRow(TSharedPtr<FGitStaleLockItem> InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SGitStaleLockRow, OwnerTable, InItem);
}

ECheckBoxState SGitStaleLocksDialog::GetAllCheckedState() const
{
	const int32 NumChecked = GetNumChecked();
	if (NumChecked == 0)
	{
		return ECheckBoxState::Unchecked;
	}
	return (NumChecked == Items.Num()) ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

void SGitStaleLocksDialog::OnAllCheckedStateChanged(ECheckBoxState InNewState)
{
	const bool bChecked = (InNewState == ECheckBoxState::Checked);
	for (const TSharedPtr<FGitStaleLockItem>& Item : Items)
	{
		Item->bChecked = bChecked;
	}
}

int32 SGitStaleLocksDialog::GetNumChecked() const
{
	int32 NumChecked = 0;
	for (const TSharedPtr<FGitStaleLockItem>& Item : Items)
	{
		NumChecked += Item->bChecked ? 1 : 0;
	}
	return NumChecked;
}

FText SGitStaleLocksDialog::GetReleaseButtonText() const
{
	return FText::Format(LOCTEXT("StaleLocks_Release", "Release {0} {0}|plural(one=Lock,other=Locks)"), GetNumChecked());
}

bool SGitStaleLocksDialog::IsReleaseEnabled() const
{
	return GetNumChecked() > 0;
}

FReply SGitStaleLocksDialog::OnReleaseClicked()
{
	TArray<FGitStaleLock> Locks;
	for (const TSharedPtr<FGitStaleLockItem>& Item : Items)
	{
		if (Item->bChecked)
		{
			Locks.Add(Item->Lock);
		}
	}
	OnRelease.ExecuteIfBound(Locks);
	return OnKeepClicked();
}

FReply SGitStaleLocksDialog::OnKeepClicked()
{
	if (const TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
