// WinDirStat - Directory Statistics
// Copyright © WinDirStat Team
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "pch.h"
#include "ItemSearch.h"
#include "FileTreeView.h"

CFileSearchControl::CFileSearchControl() : CTreeListControl(COptions::SearchViewColumnOrder.Ptr(), COptions::SearchViewColumnWidths.Ptr(), COptions::SearchViewColumnVisibility.Ptr(), LF_SEARCHLIST, false)
{
    m_singleton = this;
}

bool CFileSearchControl::GetAscendingDefault(const int column)
{
    return column == COL_ITEMSEARCH_NAME || column == COL_ITEMSEARCH_LAST_CHANGE;
}

BEGIN_MESSAGE_MAP(CFileSearchControl, CTreeListControl)
END_MESSAGE_MAP()

std::wregex CFileSearchControl::ComputeSearchRegex(const std::wstring & searchTerm, const bool searchCase, const bool useRegex)
{
    try
    {
        // Validate input is valid
        if (searchTerm.empty()) return {};

        // Decode regex flags based on settings
        auto searchFlags = std::regex_constants::optimize;
        if (!searchCase) searchFlags |= std::regex_constants::icase;

        // Precompile regex string
        return std::wregex(useRegex ?
            searchTerm : GlobToRegex(searchTerm, false), searchFlags);
    }
    catch (...)
    {
        return {};
    }
}

void CFileSearchControl::ProcessSearch(CItem* item,
    const std::wstring & searchTerm, const bool searchCase,
    const bool searchWholePhrase, const bool searchRegex, const bool onlyFiles)
{
    // Update tab visibility to show search tab if results exist
    CMainFrame::Get()->GetFileTabbedView()->SetSearchTabVisibility(true);

    // Process search request using progress dialog
    std::vector<CItem*> matchedItems;
    CProgressDlg(static_cast<size_t>(item->GetItemsCount()), CProgressDlg::Flags::None, AfxGetMainWnd(),
        [&](CProgressDlg* pdlg)
    {
        // Remove previous results
        SetRootItem();
        m_rootItem->SetLimitExceeded(false);

        // Precompile regex string
        const auto searchTermRegex = ComputeSearchRegex(searchTerm,
            searchCase, searchRegex);

        // Do search
        std::vector<CItem*> queue{ item };
        while (!queue.empty() && !pdlg->IsCancelled())
        {
            // Grab item from queue
            pdlg->Increment();
            CItem* qitem = queue.back();
            queue.pop_back();

            // Check for match
            if (!onlyFiles || qitem->IsTypeOrFlag(IT_FILE))
            {
                const auto nameView = qitem->GetNameView();
                const bool isMatch = searchWholePhrase ?
                    std::regex_match(nameView.begin(), nameView.end(), searchTermRegex) :
                    std::regex_search(nameView.begin(), nameView.end(), searchTermRegex);

                if (isMatch)
                {
                    matchedItems.push_back(qitem);
                }
            }

            // Descend into child items
            if (qitem->IsLeaf() || qitem->IsTypeOrFlag(IT_HLINKS)) continue;
            for (const auto& child : qitem->GetChildren())
            {
                queue.push_back(child);
            }
        }

        // Sort by physical size (largest first) and take top N results
        const size_t maxResults = COptions::SearchMaxResults;
        if (matchedItems.size() > maxResults)
        {
            // Partial sort to get the top N items by physical size
            std::ranges::partial_sort(matchedItems, matchedItems.begin() + maxResults,
                std::ranges::greater{}, &CItem::GetSizeLogical);

            // Keep only the top N results
            matchedItems.resize(maxResults);
            m_rootItem->SetLimitExceeded(true);
        }
    }).DoModal();

    PopulateSearchResults(matchedItems);
}

void CFileSearchControl::PopulateSearchResults(const std::vector<CItem*>& matchedItems)
{
    // Add found items to the interface
    CWaitCursor wait;
    CollapseItem(0);

    // Add to found items
    const CSetRedrawLock lock(this);
    m_itemTracker.reserve(matchedItems.size());
    for (CItem* matchedItem : matchedItems)
    {
        auto searchItem = new CItemSearch(matchedItem);
        m_itemTracker.emplace(matchedItem, searchItem);
        m_rootItem->AddSearchItemChild(searchItem);
    }

    SortItems();
    ExpandItem(0);
}

// Recursively checks whether a directory contains no files anywhere in its subtree. A
// nested empty subdirectory still counts as empty; an actual file, a directory
// symlink/reparse point (following it could hide real files a plain listing wouldn't
// reveal), or anything that can't be verified (e.g. access denied), does not.
//
// std::filesystem::is_empty() covers the common case (an actual leaf directory) in one
// call. A directory that isn't itself empty only needs one non-recursive listing of its
// own immediate entries, deferring to the memo for any subdirectory rather than
// rescanning it, so a chain of N nested empty directories costs O(N) total here instead
// of O(N^2) from each caller re-walking its own subtree.
inline bool CFileSearchControl::IsWhollyEmptyOnDisk(const std::wstring& path, std::unordered_map<std::wstring, bool>& memo)
{
    const std::unordered_map<std::wstring, bool>::const_iterator found = memo.find(path);
    if (found != memo.end()) return found->second;

    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) return memo.emplace(path, false).first->second;

    bool result = true;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        if (ec || it->symlink_status(ec).type() != std::filesystem::file_type::directory ||
            !IsWhollyEmptyOnDisk(it->path().native(), memo))
        {
            result = false;
            break;
        }
    }

    return memo.emplace(path, result && !ec).first->second;
}

void CFileSearchControl::SearchEmptyFolders(const std::vector<CItem*>& roots)
{
    // Update tab visibility to show search tab if results exist
    CMainFrame::Get()->GetFileTabbedView()->SetSearchTabVisibility(true);

    // If the user directly multi-selected both a folder and one of its own subfolders,
    // drop the subfolder from the seed set. Otherwise, if the subfolder happens to be
    // visited before its selected ancestor (stack order), it could be recorded as its
    // own topmost entry, and then the ancestor - also wholly empty - gets recorded as a
    // second, overlapping topmost entry that would delete the same branch again.
    std::vector<CItem*> seeds;
    seeds.reserve(roots.size());
    for (CItem* root : roots)
    {
        const bool hasSelectedAncestor = std::ranges::any_of(roots, [&](const CItem* other)
        {
            return other != root && other->IsAncestorOf(root);
        });
        if (!hasSelectedAncestor) seeds.push_back(root);
    }

    // Collect only the topmost directory of each empty branch: a directory whose entire
    // subtree contains no files (GetFilesCount() == 0) is wholly empty, so every folder
    // below it is empty too and does not need its own entry - removing the topmost one
    // (via the normal Delete / Delete to Recycle Bin path) takes the whole branch with it.
    // Listing descendants separately would make the result count misleading: picking N
    // entries to delete could remove more than N items once nested branches are involved.
    //
    // GetFilesCount() reflects the scanned model, which silently skips hidden/protected
    // files and directories, symlinks, and anything matching a user filter rule (Item.cpp,
    // ScanItems) - so a directory can read as "wholly empty" there while still holding
    // real files on disk. RemoveDirectory() used to catch this at deletion time since
    // Windows itself refuses to remove a directory that still has any entry, hidden or
    // not; that guarantee is gone once deletion goes through the generic recursive Delete
    // path, so IsWhollyEmptyOnDisk() re-verifies against the real filesystem instead.
    //
    // Wrapped in the same progress dialog ProcessSearch uses: IsWhollyEmptyOnDisk touches
    // the real filesystem for every candidate, which can take a while on a large or deeply
    // nested tree, so this keeps the UI responsive and lets the user cancel instead of the
    // app appearing to hang.
    std::vector<CItem*> emptyDirs;
    ULONGLONG totalItems = 0;
    for (const CItem* seed : seeds) totalItems += seed->GetItemsCount();
    CProgressDlg(static_cast<size_t>(totalItems), CProgressDlg::Flags::None, AfxGetMainWnd(), [&](CProgressDlg* pdlg)
    {
        // Remove previous results
        SetRootItem();
        m_rootItem->SetLimitExceeded(false);

        std::vector<CItem*> stack(seeds.begin(), seeds.end());
        std::unordered_set<CItem*> visited;
        std::unordered_map<std::wstring, bool> emptyOnDiskMemo;
        while (!stack.empty() && !pdlg->IsCancelled())
        {
            pdlg->Increment();
            CItem* item = stack.back();
            stack.pop_back();
            if (!visited.insert(item).second) continue;
            if (item->IsTypeOrFlag(IT_DIRECTORY) && !item->IsRootItem() && item->GetFilesCount() == 0
                && IsWhollyEmptyOnDisk(item->GetPathLong(), emptyOnDiskMemo))
            {
                emptyDirs.push_back(item);
                continue;
            }
            if (item->HasChildren())
            {
                stack.insert(stack.end(), item->GetChildren().begin(), item->GetChildren().end());
            }
        }
    }).DoModal();

    // The old code deleted every match directly, so an unbounded count never showed up
    // anywhere. Now that matches are shown in a list instead, a drive full of thousands
    // of empty folders (e.g. leftover build/cache directories) would otherwise dump all
    // of them into the result view at once. Capped the same way and using the same
    // options key (COptions::SearchMaxResults) as the existing text search, so behavior
    // is consistent and the largest folders (by logical size) are kept when the cap is
    // hit - SetLimitExceeded() below then lets the result view show its usual
    // "more results than shown" notice.
    if (const size_t maxResults = COptions::SearchMaxResults; emptyDirs.size() > maxResults)
    {
        std::ranges::partial_sort(emptyDirs, emptyDirs.begin() + maxResults,
            std::ranges::greater{}, &CItem::GetSizeLogical);
        emptyDirs.resize(maxResults);
        m_rootItem->SetLimitExceeded(true);
    }

    // Show the found empty folders in the search results view instead of deleting them
    // directly. From there, the user picks which ones to keep and removes the rest via
    // the normal Delete / Delete to Recycle Bin commands, same as any other search result.
    // Known limitation: a result stays in this list until the user acts on it, so a folder
    // that received a new file after this scan (e.g. an active cache directory) but before
    // the user deletes it would still be deleted along with that new file. This is the same
    // race any "browse a list, act later" UI has - including the file tree itself - and
    // fixing it here would mean re-verifying emptiness inside DeletePhysicalItems, which is
    // the shared delete path for every file and folder in the app, not just this feature.
    PopulateSearchResults(emptyDirs);
}

void CFileSearchControl::RemoveItem(CItem* item)
{
    const CSetRedrawLock lock(this);
    std::erase_if(m_itemTracker, [&](const auto& pair)
    {
        if (pair.first != item && !item->IsAncestorOf(pair.first)) return false;
        m_rootItem->RemoveSearchItemChild(pair.second);
        return true;
    });
}

void CFileSearchControl::AfterDeleteAllItems()
{
    // Delete previous search results
    m_itemTracker.clear();

    // Delete and recreate root item
    delete m_rootItem;
    m_rootItem = new CItemSearch();
    InsertItem(0, m_rootItem);
    m_rootItem->SetExpanded(true);
}
