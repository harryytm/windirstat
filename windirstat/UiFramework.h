// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "UiPanes.h"

// -----------------------------------------------------------------------------
//  Application command and resource IDs
// -----------------------------------------------------------------------------
#include "resource.h"

inline constexpr UINT ID_SEPARATOR = 0;

// CWinApp's routes are defined here because ID_APP_EXIT must be visible first.
// It is inline because this header is included by multiple translation units.
inline std::span<const RouteEntry> CWinApp::Routes()
{
    static constexpr std::array entries
    {
        Route::Command<&OnAppExit>(ID_APP_EXIT),
    };
    return entries;
}

// CFrameWnd's routes handle ID_VIEW_TOOLBAR and ID_VIEW_STATUS_BAR.
// It is inline for the same reason as CWinApp's table above.
inline std::span<const RouteEntry> CFrameWnd::Routes()
{
    static constexpr std::array entries
    {
        Route::Command<&OnToggleViewBar>(ID_VIEW_TOOLBAR, ID_VIEW_STATUS_BAR),
        Route::Update<&OnUpdateViewBarMenu>(ID_VIEW_TOOLBAR, ID_VIEW_STATUS_BAR),
    };
    return entries;
}

inline void CFrameWnd::OnToggleViewBar(const UINT nID)
{
    CWnd* bar = nID == ID_VIEW_TOOLBAR ? m_topBar : nID == ID_VIEW_STATUS_BAR ? m_bottomBar : nullptr;
    if (bar == nullptr) return;
    bar->ShowWindow(bar->IsWindowVisible() ? SW_HIDE : SW_SHOW);
    UpdateLayout();
}

inline void CFrameWnd::OnUpdateViewBarMenu(CCmdUI* pCmdUI) const
{
    const CWnd* bar = pCmdUI->m_nID == ID_VIEW_TOOLBAR ? m_topBar :
        pCmdUI->m_nID == ID_VIEW_STATUS_BAR ? m_bottomBar : nullptr;
    pCmdUI->Enable(bar != nullptr);
    if (bar != nullptr) pCmdUI->SetCheck(bar->IsWindowVisible() ? 1 : 0);
}

constexpr UINT_PTR NumericSubclassId = 0x4E554D;

struct NumericSubclassData
{
    ULONGLONG min = 0;
    ULONGLONG max = 0;
    size_t maxLen = 0;
    UINT delay = 0;
    UINT_PTR timerId = 0;
    bool allowEmpty = false;
};

inline constexpr auto powersOf10Lut = []() constexpr
{
    std::array<ULONGLONG, 20> values{};
    ULONGLONG current = 1;
    for (size_t i = 0; i < 20; ++i)
    {
        values[i] = current;
        if (i + 1 < 20) current *= 10;
    }
    return values;
}();

inline LRESULT CALLBACK NumericInputSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto* const data = reinterpret_cast<NumericSubclassData*>(dwRefData);
    if (!data) return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);

    const WindowRef wnd(hWnd);

    auto triggerClamp = [&](const ULONGLONG targetVal)
    {
        MessageBeep(MB_OK);
        wnd.SetText(std::to_wstring(targetVal));
        wnd.SendMessage(EM_SETSEL, 0, -1);
    };

    auto validateAndEnforce = [&]()
        {
            if (const auto id = std::exchange(data->timerId, 0)) KillTimer(hWnd, id);

            const std::wstring string = wnd.GetText();
            if (string.empty() && data->allowEmpty) return;

            if (!string.empty())
            {
                const ULONGLONG value = std::wcstoull(string.c_str(), nullptr, 10);

                if (value > data->max) return triggerClamp(data->max);
                if (string.front() == L'0' && (data->min > 0 || string.length() > 1))
                    return triggerClamp(std::max(data->min, value));
                if (value >= data->min) return;

                const size_t currentLen = string.length();
                if (currentLen < data->maxLen)
                {
                    const ULONGLONG multiplier = powersOf10Lut[data->maxLen - currentLen];
                    if ((value * multiplier) + (multiplier - 1) >= data->min)
                    {
                        data->timerId = reinterpret_cast<UINT_PTR>(data);
                        SetTimer(hWnd, data->timerId, data->delay, nullptr);
                        return;
                    }
                }
            }

            triggerClamp(data->min);
        };

    if (uMsg == WM_NCDESTROY)
    {
        if (const auto id = std::exchange(data->timerId, 0)) KillTimer(hWnd, id);
        RemoveWindowSubclass(hWnd, &NumericInputSubclassProc, uIdSubclass);
        delete data;
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_TIMER && wParam == data->timerId)
    {
        KillTimer(hWnd, std::exchange(data->timerId, 0));
        const std::wstring string = wnd.GetText();
        if (!string.empty() && std::wcstoull(string.c_str(), nullptr, 10) < data->min)
            triggerClamp(data->min);
        return 0;
    }

    if (uMsg == WM_KILLFOCUS)
    {
        if (const auto id = std::exchange(data->timerId, 0)) KillTimer(hWnd, id);
        const std::wstring string = wnd.GetText();
        if (string.empty() ? !data->allowEmpty : std::wcstoull(string.c_str(), nullptr, 10) < data->min)
            triggerClamp(data->min);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    auto isAnyOf = [](const auto val, const auto... set) noexcept { return ((std::cmp_equal(val, set)) || ...); };

    auto forwardAndValidate = [&]()
        {
            const LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            validateAndEnforce();
            return result;
        };

    if (isAnyOf(uMsg, WM_CHAR, WM_PASTE, WM_CUT, WM_CLEAR)) return forwardAndValidate();
    if (uMsg == WM_KEYDOWN && wParam == VK_DELETE) return forwardAndValidate();
    if (uMsg == WM_NULL && wParam == NumericSubclassId) return forwardAndValidate();

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Helper function to limit numeric input in an edit control to a specified range.
void LimitNumericInput(const WindowRef wnd, const ULONGLONG min, const ULONGLONG max, const bool allowEmpty = false, const std::optional<UINT> delay = std::nullopt);
