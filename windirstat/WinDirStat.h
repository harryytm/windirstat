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
#include "IconHandler.h"

class CMainFrame;
class CDirStatApp;

// Frequently used "globals"
CIconHandler* GetIconHandler();

//
// CDirStatApp. The MFC application object.
// Knows about RAM Usage, Mount points, Help files and the CIconHandler.
//
class CDirStatApp final : public CWinAppEx
{
    friend class CWinDirStatCommandLineInfo;

public:

    CDirStatApp();
    static CDirStatApp* Get() { return &s_singleton; }
    BOOL InitInstance() override;

    static bool InPortableMode();

    static std::tuple<ULONGLONG, ULONGLONG> GetFreeDiskSpace(const std::wstring& pszRootPath);
    static std::wstring GetCurrentProcessMemoryInfo();

    static void LaunchHelp();
    static void LegacyUninstall();

    void RestartApplication(bool resetPreferences = false);

    bool SetPortableMode(bool enable, bool onlyOpen = false);
    bool IsFollowingAllowed(DWORD reparseTag = 0) const;

    BOOL LoadState(LPCTSTR, CFrameImpl*) override { return TRUE; }
    BOOL IsIdleMessage(MSG* pMsg) override;

    std::wstring GetSaveDupesToCsvPath() const { return m_saveDupesToCsvPath; }
    std::wstring GetSaveToCsvPath() const { return m_saveToCsvPath; }

    COLORREF AltColor() const;           // Coloring of compressed items
    COLORREF AltEncryptionColor() const; // Coloring of encrypted items

    CIconHandler* GetIconHandler();

protected:

    CSingleDocTemplate* m_pDocTemplate{nullptr}; // MFC voodoo.

    static CDirStatApp s_singleton;    // Singleton application instance

    std::wstring m_loadFromCsvPath;    // Path to load csv file from
    std::wstring m_saveDupesToCsvPath; // Path to save duplicates csv file to
    std::wstring m_saveToCsvPath;      // Path to save csv file to

    // Get the alternative color from Explorer configuration
    COLORREF GetAlternativeColor(COLORREF clrDefault, const std::wstring& which) const;
    COLORREF m_altColor;               // Coloring of compressed items
    COLORREF m_altEncryptionColor;     // Coloring of encrypted items

    CIconHandler m_iconList;           // Central icon list

#ifdef _DEBUG
    CWDSTracerConsole m_vtraceConsole;
#endif

    DECLARE_MESSAGE_MAP()
    afx_msg void OnAppAbout();
    afx_msg void OnFileOpen();
    afx_msg void OnFilter();
    afx_msg void OnHelpManual();
    afx_msg void OnReportBug();
    afx_msg void OnRunElevated();
    afx_msg void OnUpdateRunElevated(CCmdUI* pCmdUI);

};
