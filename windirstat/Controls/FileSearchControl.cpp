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
#include "FinderBasic.h"

CFileSearchControl::CFileSearchControl() : CTreeListControl(COptions::SearchViewColumnOrder.Ptr(), COptions::SearchViewColumnWidths.Ptr(), COptions::SearchViewColumnVisibility.Ptr(), LF_SEARCHLIST, false)
{
    m_singleton = this;
}

bool CFileSearchControl::GetAscendingDefault(const int column)
{
    return column == COL_ITEMSEARCH_NAME || column == COL_ITEMSEARCH_LAST_CHANGE;
}

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
    CProgressDlg(static_cast<size_t>(item->GetItemsCount()), CProgressDlg::Flags::None, GetMainWindow(),
        [&](CProgressDlg* pdlg)
    {
        // Remove previous results
        SetRootItem();
        m_rootItem->SetLimitExceeded(false);

        // Precompile regex string
        const auto searchTermRegex = ComputeSearchRegex(searchTerm,
            searchCase, searchRegex);

        // Do search
        std::vector queue{ item };
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
    }).ShowModal();

    // Add found items to the interface
    CWaitCursor wait;
    CollapseItem(0);

    // Add to found items
    const ScopedRedrawPause lock(this);
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

void CFileSearchControl::SearchEmptyFolders(const std::vector<CItem*>& items)
{
    // Update tab visibility to show search tab if results exist
    CMainFrame::Get()->GetFileTabbedView()->SetSearchTabVisibility(true);

    // Drop items that are descendants of another selected item, so a nested pair can't
    // end up as two overlapping topmost matches - no duplicate-tracking set is needed below.
    std::vector<CItem*> stack;
    stack.reserve(items.size());
    ULONGLONG totalItems = 0;
    for (CItem* item : items)
    {
        const bool hasSelectedAncestor = std::ranges::any_of(items, [&](const CItem* other)
        {
            return other != item && other->IsAncestorOf(item);
        });
        if (!hasSelectedAncestor)
        {
            totalItems += item->GetItemsCount();
            stack.push_back(item);
        }
    }

    // Only the topmost directory of each empty branch is kept - removing it (via Delete /
    // Delete to Recycle Bin) takes the whole branch with it, so descendants don't need entries.
    std::vector<CItem*> results;
    CProgressDlg(static_cast<size_t>(totalItems), CProgressDlg::Flags::None, GetMainWindow(), [&](CProgressDlg* pdlg)
    {
        // Remove previous results
        SetRootItem();
        m_rootItem->SetLimitExceeded(false);

        std::unordered_map<std::wstring, bool> checkedFolders;

        while (!stack.empty() && !pdlg->IsCancelled())
        {
            pdlg->Increment();
            CItem* item = stack.back();
            stack.pop_back();
            if (FinderBasic::IsEmptyFolderOnDisk(item, checkedFolders))
            {
                // Cap like the existing text search, so a drive full of leftover empty
                // folders doesn't dump everything into the result view at once; stop the
                // scan itself once hit instead of collecting everything first.
                if (results.size() >= COptions::SearchMaxResults)
                {
                    m_rootItem->SetLimitExceeded(true);
                    break;
                }
                results.push_back(item);
                continue;
            }
            // Don't descend into followed links: their children live outside the selected
            // physical tree, so an empty folder found there isn't one the user asked about.
            if (item->HasChildren() && !item->IsTypeOrFlag(ITRP_MASK))
            {
                stack.insert(stack.end(), item->GetChildren().begin(), item->GetChildren().end());
            }
        }
    }).ShowModal();

    // Add found items to the interface - a snapshot, like every other scan result: a folder
    // listed here can still gain a file before the user gets around to deleting it.
    CWaitCursor wait;
    CollapseItem(0);

    const ScopedRedrawPause lock(this);
    m_itemTracker.reserve(results.size());
    for (CItem* result : results)
    {
        auto searchItem = new CItemSearch(result);
        m_itemTracker.emplace(result, searchItem);
        m_rootItem->AddSearchItemChild(searchItem);
    }

    SortItems();
    ExpandItem(0);
}

void CFileSearchControl::RemoveItem(CItem* item)
{
    const ScopedRedrawPause lock(this);
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
