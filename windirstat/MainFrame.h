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
#include "PacMan.h"
#include "FileTabbedView.h"
#include "Dialogs/ProgressDlg.h"
#include "LayoutPopup.h"

class CWdsSplitterWnd;
class CMainFrame;

class CFileTreeView;
class CExtensionView;
class CVisualizationPane;

//
// The "logical focus" can be
// - on the Directory List
// - on the Extension List
// Although these windows can lose the real focus, for instance
// when a dialog box is opened, the logical focus will not be lost.
//
enum LOGICAL_FOCUS : uint8_t
{
    LF_NONE = 0,
    LF_FILETREE,
    LF_DUPELIST,
    LF_TOPLIST,
    LF_SEARCHLIST,
    LF_WATCHERLIST,
    LF_PERMSLIST,
    LF_EXTLIST,
    LF_STORAGEANALYTICS,
};

//
// CSettingsSheet.
//
class CSettingsSheet final : public MessageTarget<CSettingsSheet, CPropertySheet>
{
public:
    CSettingsSheet();
    void SetRestartRequired(const bool changed) { m_restartRequest = changed; }
    bool OnInitDialog() override;
    static bool ShowSettings(int initialPage = -1, bool refreshOnFilteringChange = true);

    bool m_restartApplication = false; // [out]
    int m_initialPage = -1;

protected:
    bool OnCommand(WPARAM wParam, LPARAM lParam) override;
    HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    bool OnEraseBkgnd(CDC* pDC) const;

    bool m_restartRequest = false;
    bool m_alreadyAsked = false;

    DECLARE_ROUTE_MAP()
};

//
// CWdsSplitterWnd. A CSplitterWnd with 2 columns or rows, which
// knows about the current split ratio and retains it even when resized.
//
class CWdsSplitterWnd final : public MessageTarget<CWdsSplitterWnd, CSplitterWnd>
{
public:
    CWdsSplitterWnd(double * splitterPos);
    void StopTracking(bool bAccept) override;
    void SetSplitterPos(double pos);
    void RestoreSplitterPos(double posIfVirgin);
    void ResetUserPosition() { m_wasTrackedByUser = false; }
    void SetStorage(double* ptr) { m_userSplitterPos = ptr; m_wasTrackedByUser = (*ptr > 0.0 && *ptr < 1.0); }
    void ClearPaneTracking();
    void TrackPane(int pane, std::function<void(bool)> onToggle, std::function<void()> onMinimize);

protected:
    bool PreCreateWindow(CREATESTRUCT& cs) override;

    struct PaneTracking
    {
        std::function<void(bool)> onToggle;
        std::function<void()> onMinimize;
    };

    double m_splitterPos{0};    // Current split ratio
    bool m_wasTrackedByUser;    // True as soon as user has modified the splitter position
    double * m_userSplitterPos; // Split ratio as set by the user
    PaneTracking m_paneTracking[2];

    void PostNcDestroy() override;

protected:
    void OnSize(UINT nType, int cx, int cy);

    DECLARE_ROUTE_MAP()
};

//
// CPacmanControl. Pacman on the status bar.
//
class CPacmanControl final : public MessageTarget<CPacmanControl, CWnd>
{
public:
    CPacmanControl() = default;
    void Drive();
    void Start();
    void Stop();

protected:
    CPacman m_pacman;

protected:
    void OnPaint();
    int OnCreate(LPCREATESTRUCT lpCreateStruct);
    bool OnEraseBkgnd(CDC* pDC);

    DECLARE_ROUTE_MAP()
};

//
// CMainFrame. The main application window.
//
class CMainFrame final : public MessageTarget<CMainFrame, CFrameWnd>
{
protected:
    static constexpr DWORD WM_CALLBACKUI = WM_APP + 1;
    static UINT s_TaskBarMessage;
    inline static CMainFrame* s_Singleton = nullptr;

public:
    CMainFrame();
    ~CMainFrame() override;

    void InitialShowWindow();
    void InvokeInMessageThread(std::function<void()> callback) const;

    void RestoreVisualizationPane(bool force = false);
    void MinimizeVisualizationPane();
    void MinimizeExtensionView();
    void ExpandFileTabbedView();
    void RestoreSplitterPositions();
    void ApplyPaneVisibility(bool restoreDuringScan = false);

    // Used for storing and retrieving the main panes
    CFileTabbedView* m_fileTabbedView = nullptr;
    CExtensionView* m_extensionView = nullptr;
    CVisualizationPane* m_visualizationPane = nullptr;
    CFileTreeView* GetFileTreeView() const { return m_fileTabbedView->GetFileTreeView(); }
    CFileTopView* GetFileTopView() const { return m_fileTabbedView->GetFileTopView(); }
    CFileDupeView* GetFileDupeView() const { return m_fileTabbedView->GetFileDupeView(); }
    CFileSearchView* GetFileSearchView() const { return m_fileTabbedView->GetFileSearchView(); }
    CFileWatcherView* GetFileWatcherView() const { return m_fileTabbedView->GetFileWatcherView(); }
    CFilePermsView* GetFilePermsView() const { return m_fileTabbedView->GetFilePermsView(); }
    CFileTabbedView* GetFileTabbedView() const { return m_fileTabbedView; }
    CExtensionView* GetExtensionView() const { return m_extensionView; }
    GraphPane GetGraphPaneType() const;
    void SelectGraphPane(GraphPane pane);
    void ShowVisualization(bool show) const;
    bool IsVisualizationShown() const;
    CWinDirStatPane* GetVisualizationPane() const;
    CWinDirStatPane* GetActiveVisualization() const;

    void CreateProgress(ULONGLONG range);
    void UpdateProgressRange(ULONGLONG range);
    void SetProgressComplete();
    void SuspendState(bool suspend);
    bool IsScanSuspended() const { return m_scanSuspend; }

    void UpdateProgress();
    void UpdateDynamicMenuItems(CMenu* menu, CMenu* menuHeader = nullptr) const;
    std::pair<CMenu*, int> LocateNamedMenu(const CMenu* menu, const std::wstring& subMenuText, bool removeItems = true) const;

    void SetLogicalFocus(LOGICAL_FOCUS lf);
    LOGICAL_FOCUS GetLogicalFocus() const { return m_logicalFocus; }
    void MoveFocus(LOGICAL_FOCUS logicalFocus);
    void UpdatePaneText();

    static void QueryRecycleBin(ULONGLONG& items, ULONGLONG& bytes);

    bool OnCreateClient() override;
    bool PreCreateWindow(CREATESTRUCT& cs) override;

    void CreateStatusProgress();
    void CreatePacmanProgress();
    void LayoutProgress();
    void DestroyProgress();

    void SetStatusPaneText(const CDC& cdc, CStatusBar::PaneId pane, const std::wstring& text, int minWidth = 0);
    void UpdateCleanupMenu(CMenu* menu, bool triggerAsync = true);

    UINT_PTR m_timer = 0;           // Timer for updating the display
    bool m_progressVisible = false; // True while progress must be shown (either pacman or progress bar)
    bool m_scanSuspend = false;     // True if the scan has been suspended
    bool m_shuttingDown = false;    // Marks the process is shutting down so we can exit timers
    ULONGLONG m_progressRange = 0;  // Progress range. A range of 0 means Pacman should be used.
    ULONGLONG m_progressPos = 0;    // Progress position (<= progressRange, or an item count in case of m_progressRang == 0)
    CItem* m_workingItem = nullptr;

    CWdsSplitterWnd m_subSplitter{ COptions::SubSplitterPos.Ptr() }; // Contains the two upper views
    CWdsSplitterWnd m_splitter{ COptions::MainSplitterPos.Ptr() };    // Contains (a) m_wndSubSplitter and (b) the graph view.
    CLayoutPopup m_layoutPopup;                                        // Floating layout-picker popup

    CStatusBar m_wndStatusBar; // Status bar
    CToolBar m_wndToolBar;     // Toolbar
    CSize m_defaultButtonSize;  // Toolbar button size at creation, before DPI and size scaling
    int m_watcherAutoScrollOnImage = -1;
    int m_watcherAutoScrollOffImage = -1;
    CWdsProgressCtrl m_progress;  // Progress control. Is Create()ed and Destroy()ed again every time.
    CPacmanControl m_pacman;      // Static control for Pacman
    LOGICAL_FOCUS m_logicalFocus = LF_NONE; // Which view has the logical focus

    CComPtr<ITaskbarList3> m_taskbarList;
    TBPFLAG m_taskbarButtonState = TBPF_INDETERMINATE;
    TBPFLAG m_taskbarButtonPreviousState = TBPF_INDETERMINATE;

    // Cached values for cleanup menu queries (updated asynchronously)
    ULONGLONG m_recycleBinItems = 0;
    ULONGLONG m_recycleBinBytes = 0;
    ULONGLONG m_shadowCopyCount = 0;
    ULONGLONG m_shadowCopyBytes = 0;

protected:
    CCmdTarget* GetCommandTarget() const override { return CWinDirStatModel::Get(); }
    int OnCreate(LPCREATESTRUCT lpCreateStruct);
    void OnSetFocus(CWnd* pOldWnd);
    void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    LRESULT OnEnterSizeMove(WPARAM, LPARAM) const;
    LRESULT OnExitSizeMove(WPARAM, LPARAM) const;
    LRESULT OnCallbackRequest(WPARAM, LPARAM lParam);
    void OnTimer(UINT_PTR nIDEvent);
    void OnClose();
    void OnInitMenuPopup(CMenu* pPopupMenu, UINT nIndex, bool bSysMenu);
    LRESULT OnMenuCommand(WPARAM position, LPARAM menuHandle);
    void OnUpdateEnableControl(CCmdUI* pCmdUI);
    void OnSize(UINT nType, int cx, int cy);
    void OnUpdateViewShowVisualization(CCmdUI* pCmdUI) const;
    void OnUpdateTreeMapUseLogical(CCmdUI* pCmdUI);
    void OnUpdateTreeMapUsePhysical(CCmdUI* pCmdUI);
    void OnUpdateViewAbsolutePercentages(CCmdUI* pCmdUI);
    void OnUpdateViewShowFileTypes(CCmdUI* pCmdUI) const;
    void OnUpdateViewGroupUnregisteredTypes(CCmdUI* pCmdUI) const;
    void OnUpdateViewShowWatcher(CCmdUI* pCmdUI) const;
    void OnViewShowVisualization();
    void OnViewTreeMapStyle(UINT commandId);
    void OnUpdateViewTreeMapStyle(CCmdUI* pCmdUI) const;
    void OnViewFlameGraph();
    void OnUpdateViewFlameGraph(CCmdUI* pCmdUI) const;
    void OnViewSunburst();
    void OnUpdateViewSunburst(CCmdUI* pCmdUI) const;
    void OnViewTreeMapUseLogical();
    void OnViewTreeMapUsePhysical();
    void OnViewAbsolutePercentages() const;
    void OnViewShowFileTypes();
    void OnViewGroupUnregisteredTypes() const;
    void OnViewShowExtensionsOnTreeMap() const;
    void OnUpdateViewShowExtensionsOnTreeMap(CCmdUI* pCmdUI) const;
    void OnViewShowFolderFramesOnTreeMap() const;
    void OnUpdateViewShowFolderFramesOnTreeMap(CCmdUI* pCmdUI) const;
    void OnViewAllFiles() const { GetFileTabbedView()->SetActiveFileTreeView(); }
    void OnViewLargestFiles() const { GetFileTabbedView()->SetActiveTopView(); }
    void OnViewDuplicateFiles() const { GetFileTabbedView()->SetActiveDupeView(); }
    void OnViewSearchResults() const { GetFileTabbedView()->SetActiveSearchView(); }
    void OnViewToolBarSize(UINT commandId);
    void OnUpdateViewToolBarSize(CCmdUI* pCmdUI) const;
    void OnViewFontSize(UINT commandId);
    void OnUpdateViewFontSize(CCmdUI* pCmdUI) const;
    void OnAdvancedShadowCopy(UINT nID);
    void OnAdvancedDefrag(UINT nID);
    void OnAdvancedChkdsk(UINT nID);
    void OnToolsWatcher() const;
    void OnWatcherStart();
    void OnUpdateWatcherStart(CCmdUI* pCmdUI);
    void OnWatcherPause();
    void OnUpdateWatcherPause(CCmdUI* pCmdUI);
    void OnWatcherAutoScroll();
    void OnUpdateWatcherAutoScroll(CCmdUI* pCmdUI);
    void OnWatcherClear();
    void OnUpdateWatcherClear(CCmdUI* pCmdUI);
    void OnToolsPermissions() const;
    void OnUpdateToolsPermissions(CCmdUI* pCmdUI) const;
    void OnToolsStorageAnalytics() const;
    void OnUpdateToolsStorageAnalytics(CCmdUI* pCmdUI) const;
    void UpdateToolsMenu(CMenu* menu) const;
    void OnViewWindowLayout();
    void OnConfigure();
    void OnDestroy();
    LRESULT OnTaskButtonCreated(WPARAM, LPARAM);
    UINT OnPowerBroadcast(UINT, LPARAM);
    void OnSysColorChange();
    void OnSettingChange(UINT, LPCTSTR);
    LRESULT OnUahDrawMenu(WPARAM wParam, LPARAM lParam) const;
    void OnNcPaint();
    bool OnNcActivate(bool bActive);
    bool OnEraseBkgnd(CDC*) { return true; }
public:
    static CMainFrame* Get() { return s_Singleton; }
    void UpdateFrameTitleForScan(LPCWSTR scanName);
    void UpdateAllPanes(CWnd* sender, MODEL_CHANGE change, CItem* item) const;
    void RebuildToolBar(bool rebuildButtons = true);
    void SetWatcherToolBarButtons(bool visible, bool updateLayout = true);
    void RebuildLayout(bool resetPositions = false);
    bool CreateFromResource(UINT nIDResource) override;

private:
    void ApplyFontSize(int percent, bool rebuildToolBar = false);
    void ApplyWindowsTextScale();
    void BuildSplitterLayout(int topo, int perm, HWND hFTV, HWND hExtV, HWND hVisualization);
    void ConfigureSplitterCallbacks(int topo, int perm);

    DECLARE_ROUTE_MAP()
};

BEGIN_ROUTE_MAP(CSettingsSheet)
    ON_WINDOW(OnCtlColor, WM_CTLCOLOR)
    ON_WINDOW(OnEraseBkgnd, WM_ERASEBKGND)
END_ROUTE_MAP()

BEGIN_ROUTE_MAP(CWdsSplitterWnd)
    ON_WINDOW(OnSize, WM_SIZE)
END_ROUTE_MAP()

BEGIN_ROUTE_MAP(CPacmanControl)
    ON_WINDOW(OnPaint, WM_PAINT)
    ON_WINDOW(OnCreate, WM_CREATE)
    ON_WINDOW(OnEraseBkgnd, WM_ERASEBKGND)
END_ROUTE_MAP()

BEGIN_ROUTE_MAP(CMainFrame)
    ON_COMMAND(OnConfigure, ID_CONFIGURE)
    ON_COMMAND(OnViewShowFileTypes, ID_VIEW_SHOWFILETYPES)
    ON_COMMAND(OnViewGroupUnregisteredTypes, ID_VIEW_GROUP_TYPES)
    ON_COMMAND(OnViewShowVisualization, ID_VIEW_SHOWVISUALIZATION)
    ON_COMMAND(OnViewTreeMapStyle, ID_VIEW_TREEMAP_ROWS, ID_VIEW_TREEMAP_MOORE)
    ON_COMMAND(OnViewFlameGraph, ID_VIEW_FLAMEGRAPH)
    ON_COMMAND(OnViewSunburst, ID_VIEW_SUNBURST)
    ON_COMMAND(OnViewTreeMapUseLogical, ID_TREEMAP_LOGICAL_SIZE)
    ON_COMMAND(OnViewTreeMapUsePhysical, ID_TREEMAP_PHYSICAL_SIZE)
    ON_COMMAND(OnViewAbsolutePercentages, ID_VIEW_ABSOLUTE_PERCENTAGES)
    ON_WINDOW(OnEnterSizeMove, WM_ENTERSIZEMOVE)
    ON_WINDOW(OnExitSizeMove, WM_EXITSIZEMOVE)
    ON_WINDOW(OnCallbackRequest, WM_CALLBACKUI)
    ON_WINDOW(OnUahDrawMenu, DarkMode::WM_UAHDRAWMENU)
    ON_WINDOW(OnUahDrawMenu, DarkMode::WM_UAHDRAWMENUITEM)
    ON_REGISTERED(OnTaskButtonCreated, s_TaskBarMessage)
    ON_UPDATE(OnUpdateViewShowVisualization, ID_VIEW_SHOWVISUALIZATION)
    ON_UPDATE(OnUpdateViewTreeMapStyle, ID_VIEW_TREEMAP_ROWS, ID_VIEW_TREEMAP_MOORE)
    ON_UPDATE(OnUpdateViewFlameGraph, ID_VIEW_FLAMEGRAPH)
    ON_UPDATE(OnUpdateViewSunburst, ID_VIEW_SUNBURST)
    ON_UPDATE(OnUpdateViewShowFileTypes, ID_VIEW_SHOWFILETYPES)
    ON_UPDATE(OnUpdateViewGroupUnregisteredTypes, ID_VIEW_GROUP_TYPES)
    ON_UPDATE(OnUpdateTreeMapUseLogical, ID_TREEMAP_LOGICAL_SIZE)
    ON_UPDATE(OnUpdateTreeMapUsePhysical, ID_TREEMAP_PHYSICAL_SIZE)
    ON_UPDATE(OnUpdateViewAbsolutePercentages, ID_VIEW_ABSOLUTE_PERCENTAGES)
    ON_COMMAND(OnViewShowExtensionsOnTreeMap, ID_TREEMAP_SHOW_EXTENSIONS)
    ON_UPDATE(OnUpdateViewShowExtensionsOnTreeMap, ID_TREEMAP_SHOW_EXTENSIONS)
    ON_COMMAND(OnViewShowFolderFramesOnTreeMap, ID_TREEMAP_SHOW_FOLDER_FRAMES)
    ON_UPDATE(OnUpdateViewShowFolderFramesOnTreeMap, ID_TREEMAP_SHOW_FOLDER_FRAMES)
    ON_UPDATE(OnUpdateViewShowWatcher, ID_TOOLS_WATCHER)
    ON_WINDOW(OnClose, WM_CLOSE)
    ON_WINDOW(OnCreate, WM_CREATE)
    ON_WINDOW(OnDestroy, WM_DESTROY)
    ON_WINDOW(OnInitMenuPopup, WM_INITMENUPOPUP)
    ON_WINDOW(OnMenuCommand, WM_MENUCOMMAND)
    ON_WINDOW(OnSize, WM_SIZE)
    ON_WINDOW(OnSysColorChange, WM_SYSCOLORCHANGE)
    ON_WINDOW(OnSettingChange, WM_SETTINGCHANGE)
    ON_WINDOW(OnPowerBroadcast, WM_POWERBROADCAST)
    ON_WINDOW(OnTimer, WM_TIMER)
    ON_WINDOW(OnNcPaint, WM_NCPAINT)
    ON_WINDOW(OnNcActivate, WM_NCACTIVATE)
    ON_WINDOW(OnEraseBkgnd, WM_ERASEBKGND)
    ON_WINDOW(OnSetFocus, WM_SETFOCUS)
    ON_WINDOW(OnKeyDown, WM_KEYDOWN)
    ON_COMMAND(OnViewAllFiles, ID_VIEW_ALL_FILES)
    ON_COMMAND(OnViewLargestFiles, ID_VIEW_LARGEST_FILES)
    ON_COMMAND(OnViewDuplicateFiles, ID_VIEW_DUPLICATE_FILES)
    ON_COMMAND(OnViewSearchResults, ID_VIEW_SEARCH_RESULTS)
    ON_COMMAND(OnViewToolBarSize, ID_VIEW_TOOLBAR_SIZE_100, ID_VIEW_TOOLBAR_SIZE_USE_WINDOWS)
    ON_UPDATE(OnUpdateViewToolBarSize, ID_VIEW_TOOLBAR_SIZE_100, ID_VIEW_TOOLBAR_SIZE_USE_WINDOWS)
    ON_COMMAND(OnViewFontSize, ID_VIEW_FONT_SIZE_100, ID_VIEW_FONT_SIZE_USE_WINDOWS)
    ON_UPDATE(OnUpdateViewFontSize, ID_VIEW_FONT_SIZE_100, ID_VIEW_FONT_SIZE_USE_WINDOWS)
    ON_COMMAND(OnAdvancedShadowCopy, ID_TOOLS_SHADOW_COPY_BASE, ID_TOOLS_SHADOW_COPY_BASE + wds::alphaSize)
    ON_COMMAND(OnAdvancedDefrag, ID_TOOLS_DEFRAG_BASE, ID_TOOLS_DEFRAG_BASE + wds::alphaSize)
    ON_COMMAND(OnAdvancedChkdsk, ID_TOOLS_CHKDSK_BASE, ID_TOOLS_CHKDSK_BASE + wds::alphaSize)
    ON_COMMAND(OnToolsWatcher, ID_TOOLS_WATCHER)
    ON_COMMAND(OnWatcherStart, ID_WATCHER_START)
    ON_UPDATE(OnUpdateWatcherStart, ID_WATCHER_START)
    ON_COMMAND(OnWatcherPause, ID_WATCHER_PAUSE)
    ON_UPDATE(OnUpdateWatcherPause, ID_WATCHER_PAUSE)
    ON_COMMAND(OnWatcherAutoScroll, ID_WATCHER_AUTOSCROLL)
    ON_UPDATE(OnUpdateWatcherAutoScroll, ID_WATCHER_AUTOSCROLL)
    ON_COMMAND(OnWatcherClear, ID_WATCHER_CLEAR)
    ON_UPDATE(OnUpdateWatcherClear, ID_WATCHER_CLEAR)
    ON_COMMAND(OnToolsPermissions, ID_TOOLS_PERMISSIONS)
    ON_UPDATE(OnUpdateToolsPermissions, ID_TOOLS_PERMISSIONS)
    ON_COMMAND(OnToolsStorageAnalytics, ID_TOOLS_STORAGE_ANALYTICS)
    ON_UPDATE(OnUpdateToolsStorageAnalytics, ID_TOOLS_STORAGE_ANALYTICS)
    ON_COMMAND(OnViewWindowLayout, ID_VIEW_WINDOW_LAYOUT)
END_ROUTE_MAP()
