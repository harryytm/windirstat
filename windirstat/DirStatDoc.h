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

#pragma once

#include "pch.h"
#include "TreeListControl.h"

class CItem;
class CItemDupe;
class CItemTop;
class CItemSearch;
enum LOGICAL_FOCUS : uint8_t;

//
// Data stored for each extension.
//
struct alignas(std::hardware_destructive_interference_size) SExtensionRecord
{
    std::atomic<ULONGLONG> files = 0;
    std::atomic<ULONGLONG> bytes = 0;
    COLORREF color = 0;

    // Use relaxed memory ordering for simple accumulation operations
    void AddFile(const ULONGLONG size) noexcept
    {
        files.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);
    }

    void RemoveFile(const ULONGLONG size) noexcept
    {
        files.fetch_sub(1, std::memory_order_relaxed);
        bytes.fetch_sub(size, std::memory_order_relaxed);
    }

    ULONGLONG GetFiles() const noexcept { return files.load(std::memory_order_relaxed); }
    ULONGLONG GetBytes() const noexcept { return bytes.load(std::memory_order_relaxed); }
};

//
// Maps an extension to an SExtensionRecord.
//
using CExtensionData = std::unordered_map<std::wstring, SExtensionRecord>;

//
// Hints for UpdateAllViews()
//
using VIEW_HINT = enum VIEW_HINT : std::uint8_t
{
    HINT_NULL,                      // General update
    HINT_NEWROOT,                   // Root item has changed - clear everything.
    HINT_SELECTIONACTION,           // Inform central selection handler to update selection (uses pHint)
    HINT_SELECTIONREFRESH,          // Inform all views to redraw based on current selections
    HINT_SELECTIONSTYLECHANGED,     // Only update selection in TreeMapView
    HINT_EXTENSIONSELECTIONCHANGED, // Type list selected a new extension
    HINT_ZOOMCHANGED,               // Only zoom item has changed.
    HINT_LISTSTYLECHANGED,          // Options: List style (grid/stripes) or treelist colors changed
    HINT_TREEMAPSTYLECHANGED        // Options: Treemap style (grid, colors etc.) changed
};

//
// CDirStatDoc. The "Document" class.
// Owner of the root item and various other data (see data members).
//
class CDirStatDoc final : public CDocument
{
public:
    static CDirStatDoc* Get() { return s_singleton; }

protected:
    CDirStatDoc(); // Created by MFC only
    DECLARE_DYNCREATE(CDirStatDoc)

    ~CDirStatDoc() override;

    bool HasRootItem() const;
    bool IsReselectChildAvailable() const;
    bool IsRootDone() const;
    bool IsScanRunning() const;
    bool IsZoomed() const;
    bool m_selectionCacheValid = false;
    bool m_showFreeSpace; // Whether to show the <Free Space> item
    bool m_showUnknown;   // Whether to show the <Unknown> item
    BOOL OnNewDocument() override;
    BOOL OnOpenDocument(CItem* newroot);
    BOOL OnOpenDocument(LPCWSTR lpszPathName) override;
    bool UserDefinedCleanupWorksForItem(USERDEFINEDCLEANUP* udc, const CItem* item) const;
    CExtensionData m_extensionData;    // Base for the extension view and cushion colors
    CExtensionData* GetExtensionData();
    CItem* GetRootItem() const;
    CItem* GetZoomItem() const;
    CItem* m_rootItem = nullptr; // The very root item
    CItem* m_zoomItem = nullptr;   // Current "zoom root"
    CItem* PopReselectChild();
    COLORREF GetCushionColor(const std::wstring& ext);
    COLORREF GetZoomColor() const;
    enum StopReason : uint8_t { Default, Stop, Abort };
    LOGICAL_FOCUS m_cachedFocus{}; // Cache for GetAllSelected to avoid expensive queries
    SExtensionRecord* GetExtensionDataRecord(const std::wstring& ext);
    static bool DupeListHasFocus();
    static bool FileTreeHasFocus();
    static bool SearchListHasFocus();
    static bool TopListHasFocus();
    static bool WatcherListHasFocus();
    static CDirStatDoc* s_singleton;
    static constexpr CompressionAlgorithm CompressionIdToAlg(UINT id);
    static CTreeListControl* GetFocusControl();
    static std::wstring BuildUserDefinedCleanupCommandLine(const std::wstring& format, const std::wstring& rootPath, const std::wstring& currentPath);
    static void AskForConfirmation(USERDEFINEDCLEANUP* udc, const CItem* item);
    static void CallUserDefinedCleanup(bool isDirectory, const std::wstring& format, const std::wstring& rootPath, const std::wstring& currentPath, bool showConsoleWindow, bool wait);
    static void OpenItem(const CItem* item, const std::wstring& verb = {});
    std::mutex m_extensionMutex;
    std::optional<std::jthread> m_thread; // Wrapper thread so we do not occupy the UI thread
    std::unordered_map<std::wstring, BlockingQueue<CItem*>> m_queues; // The scanning and thread queue
    std::vector<CItem*> GetAllSelected();
    std::vector<CItem*> m_cachedSelection;
    std::vector<CItem*> m_reselectChildStack; // Stack for the "Re-select Child"-Feature
    std::wstring GetHighlightExtension() const;
    std::wstring m_highlightExtension; // Currently highlighted extension
    ULONGLONG GetRootSize() const;
    void ClearReselectChildStack();
    void DeleteContents() override;
    void DeletePhysicalItems(const std::vector<CItem*>& items, bool toTrashBin, bool emptyOnly = false) const;
    void InvalidateSelectionCache();
    void PerformUserDefinedCleanup(USERDEFINEDCLEANUP* udc, const CItem* item);
    void PushReselectChild(CItem* item);
    void RebuildExtensionData();
    void RecurseRefreshReparsePoints(CItem* items) const;
    void RecursiveUserDefinedCleanup(USERDEFINEDCLEANUP* udc, const std::wstring& rootPath, const std::wstring& currentPath);
    void RefreshAfterUserDefinedCleanup(const USERDEFINEDCLEANUP* udc, CItem* item, std::vector<CItem*> & refreshQueue) const;
    void RefreshItem(CItem* item) const { RefreshItem(std::vector{ item }); }
    void RefreshItem(const std::vector<CItem*>& item) const;
    void RefreshReparsePointItems();
    void SetHighlightExtension(const std::wstring& ext);
    void SetPathName(LPCWSTR lpszPathName, BOOL bAddToMRU) override;
    void SetTitlePrefix(const std::wstring& prefix) const;
    void SetZoomItem(CItem* item);
    void StartScanningEngine(std::vector<CItem*> items);
    void StopScanningEngine(StopReason stopReason = Stop);
    void UnlinkRoot();
    void UpdateAllViews(CView* pSender, VIEW_HINT hint = HINT_NULL, CItem* pHint = nullptr);

    DECLARE_MESSAGE_MAP()
    afx_msg void OnCleanupCompress(UINT id);
    afx_msg void OnCleanupDelete();
    afx_msg void OnCleanupDeleteToBin();
    afx_msg void OnCleanupEmptyFolder();
    afx_msg void OnCleanupEmptyRecycleBin();
    afx_msg void OnCleanupMoveTo();
    afx_msg void OnCleanupOpenTarget();
    afx_msg void OnCleanupOptimizeVhd();
    afx_msg void OnCleanupProperties();
    afx_msg void OnCleanupSparsifyFile();
    afx_msg void OnCommandPromptHere();
    afx_msg void OnComputeHash();
    afx_msg void OnContextMenuExplore(UINT nID);
    afx_msg void OnCreateHardlink();
    afx_msg void OnDisableHibernateFile();
    afx_msg void OnEditCopy();
    afx_msg void OnExecuteDiskCleanupUtility();
    afx_msg void OnExecuteDism();
    afx_msg void OnExecuteDismAnalyze();
    afx_msg void OnExecuteDismReset();
    afx_msg void OnExecuteProgramsFeatures();
    afx_msg void OnExplorerSelect();
    afx_msg void OnLoadResults();
    afx_msg void OnPowerShellHere();
    afx_msg void OnRefreshAll();
    afx_msg void OnRefreshSelected();
    afx_msg void OnRemoveLocalProfiles();
    afx_msg void OnRemoveMarkOfTheWebTags();
    afx_msg void OnRemoveRoamingProfiles();
    afx_msg void OnRemoveShadowCopies();
    afx_msg void OnSaveDuplicates();
    afx_msg void OnSaveResults();
    afx_msg void OnScanResume();
    afx_msg void OnScanStop();
    afx_msg void OnScanSuspend();
    afx_msg void OnSearch();
    afx_msg void OnTreeMapReselectChild();
    afx_msg void OnTreeMapSelectParent();
    afx_msg void OnTreeMapZoomIn();
    afx_msg void OnTreeMapZoomOut();
    afx_msg void OnUpdateCentralHandler(CCmdUI* pCmdUI);
    afx_msg void OnUpdateCompressionHandler(CCmdUI* pCmdUI);
    afx_msg void OnUpdateCreateHardlink(CCmdUI* pCmdUI);
    afx_msg void OnUpdateUserDefinedCleanup(CCmdUI* pCmdUI);
    afx_msg void OnUpdateViewShowFreeSpace(CCmdUI* pCmdUI);
    afx_msg void OnUpdateViewShowUnknown(CCmdUI* pCmdUI);
    afx_msg void OnUserDefinedCleanup(UINT id);
    afx_msg void OnViewShowFreeSpace();
    afx_msg void OnViewShowUnknown();
};
