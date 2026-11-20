// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "Filtering.h"
#include "StorageSource.h"
#include "HelpersInterface.h"
#include "Options.h"

// --- Static member definitions ---

std::vector<std::wregex> CFiltering::ExcludeDirsRegex;
std::vector<std::wregex> CFiltering::ExcludeFilesRegex;
std::vector<std::wregex> CFiltering::IncludeDirsRegex;
std::vector<std::wregex> CFiltering::IncludeFilesRegex;
std::vector<std::wstring> CFiltering::IncludeDirsAnchors;
bool CFiltering::IncludeDirsMatchDescendants = true;
ULONGLONG CFiltering::SizeMinimumCalculated = 0;
FILETIME  CFiltering::MaxAgeFileTimeCutoff  = {};
bool      CFiltering::FilterActive          = false;

// --- Private helpers ---

static bool HasUnescapedTrailingDollar(const std::wstring_view pattern)
{
    if (pattern.empty() || pattern.back() != L'$') return false;

    size_t slashCount = 0;
    for (size_t i = pattern.size() - 1; i > 0 && pattern[i - 1] == L'\\'; --i)
    {
        ++slashCount;
    }
    return slashCount % 2 == 0;
}

struct FilterSyntaxError
{
    std::wstring_view detailKey;
};

static constexpr std::wstring_view RegexErrorDetail(const std::regex_constants::error_type code) noexcept
{
    using namespace std::regex_constants;
    switch (code)
    {
        case error_escape:    return IDS_PAGE_FILTERING_ERROR_INVALID_ESCAPE;
        case error_brack:     return IDS_PAGE_FILTERING_ERROR_MISMATCHED_BRACKETS;
        case error_paren:     return IDS_PAGE_FILTERING_ERROR_MISMATCHED_PARENS;
        case error_badrepeat: return IDS_PAGE_FILTERING_ERROR_UNEXPECTED_QUANTIFIER;
        default:              return IDS_PAGE_FILTERING_ERROR_INVALID_SYNTAX;
    }
}

static constexpr std::wstring ProcessDslAlternatives(const std::wstring_view val,
    const bool stripLeadingDots = false, const bool alwaysGroup = false)
{
    if (val.empty()) throw FilterSyntaxError{ IDS_PAGE_FILTERING_ERROR_EMPTY_VALUE };

    constexpr auto escapeLiteral = [](const std::wstring_view input) constexpr
    {
        std::wstring out;
        out.reserve(input.size() * 2);
        for (const wchar_t c : input)
        {
            if (std::wstring_view(LR"(\.^$|()[]{}+*?)").contains(c)) out += L'\\';
            out += c;
        }
        return out;
    };

    std::wstring result;
    const bool hasPipe = val.contains(L'|');
    for (size_t start = 0; start <= val.size(); )
    {
        const size_t end = val.find(L'|', start);
        auto item = (end == std::wstring_view::npos) ? val.substr(start) : val.substr(start, end - start);
        if (stripLeadingDots) while (item.starts_with(L'.')) item.remove_prefix(1);
        if (item.empty()) throw FilterSyntaxError{ IDS_PAGE_FILTERING_ERROR_EMPTY_VALUE };
        if (!result.empty()) result += L'|';
        result += escapeLiteral(item);
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return (hasPipe || alwaysGroup) ? (L"(?:" + result + L")") : result;
}

static std::optional<std::vector<std::wstring>> TokenizeDslLine(const std::wstring_view line)
{
    const auto readToken = [line](size_t& i) -> std::optional<std::wstring>
    {
        std::wstring token;
        bool inQuotes = false;
        for (; i < line.size(); ++i)
        {
            const wchar_t c = line[i];
            if (!inQuotes && iswspace(c)) break;
            if (c != L'"') { token += c; continue; }
            if (inQuotes && i + 1 < line.size() && line[i + 1] == L'"') { token += L'"'; ++i; continue; }
            inQuotes = !inQuotes;
        }
        if (inQuotes) return std::nullopt;
        return token;
    };

    std::vector<std::wstring> tokens;
    for (size_t i = 0; i < line.size(); )
    {
        while (i < line.size() && iswspace(line[i])) ++i;
        if (i >= line.size()) break;
        auto token = readToken(i);
        if (!token) return std::nullopt;
        tokens.push_back(std::move(*token));
    }
    return tokens;
}

static std::optional<std::wstring> TranslateDsl(const std::wstring_view line, const bool pathFilter)
{
    if (!line.contains(L"::")) return std::nullopt;

    const auto tokens = TokenizeDslLine(line);
    if (!tokens) throw FilterSyntaxError{ IDS_PAGE_FILTERING_ERROR_UNCLOSED_QUOTE };

    struct DslPolicy
    {
        std::wstring_view tag;
        bool stripDots;
        bool alwaysGroup;
        std::wstring_view fileBefore;
        std::wstring_view fileAfter;
        std::wstring_view pathBefore;
        std::wstring_view pathAfter;
    };

    static constexpr DslPolicy policies[] = {
        // tag             stripDots  group  fileBefore                  fileAfter                pathBefore                  pathAfter
        { L"ext::",        true,      true,  LR"([\s\S]*\.)",            L"$",                    LR"([\s\S]*\.)",            LR"((?:[\\/]|$))"        },
        { L"contains::",   false,     false, LR"([\s\S]*)",              LR"([\s\S]*)",           LR"([\s\S]*)",              LR"([\s\S]*)"            },
        { L"prefix::",     false,     false, LR"([\s\S]*(?:^|[\\/]))",   LR"([^\\/]*$)",          LR"([\s\S]*[\\/])",         LR"([^\\/]*(?:[\\/]|$))" },
        { L"suffix::",     false,     false, LR"([\s\S]*)",              LR"((?:\.[^\\/.]*)?$)",  LR"([\s\S]*)",              LR"((?:[\\/]|$))"        },
        { L"exact::",      false,     false, L"^",                       L"$",                    LR"([\s\S]*(?:^|[\\/]))",   LR"((?:[\\/]|$))"        },
    };

    const auto findPolicy = [](const std::wstring_view token) -> const DslPolicy*
    {
        const auto it = std::ranges::find_if(policies, [token](const DslPolicy& p)
        {
            return token.starts_with(p.tag);
        });
        return it != std::end(policies) ? &*it : nullptr;
    };

    if (std::ranges::none_of(*tokens, [&](const std::wstring_view t) { return findPolicy(t) != nullptr; }))
    {
        return std::nullopt;
    }

    std::wstring basePattern;
    std::wstring lookaheads;
    bool hasBase = false;

    for (const auto& token : *tokens)
    {
        if (const auto* policy = findPolicy(token))
        {
            const std::wstring value = ProcessDslAlternatives(
                std::wstring_view(token).substr(policy->tag.size()), policy->stripDots, policy->alwaysGroup);
            lookaheads.append(std::format(L"(?={}{}{})",
                pathFilter ? policy->pathBefore : policy->fileBefore,
                value,
                pathFilter ? policy->pathAfter : policy->fileAfter));
        }
        else
        {
            const std::wstring globRegex = GlobToRegex(token, false);
            if (!hasBase) { basePattern = globRegex; hasBase = true; }
            else lookaheads.append(std::format(L"(?=^{}$)", globRegex));
        }
    }

    if (!hasBase) basePattern = L"[\\s\\S]*";
    return lookaheads + basePattern;
}

static bool PrepareFilterToken(std::wstring& token, const bool defaultUseRegex, const bool pathFilter)
{
    static constexpr std::wstring_view regexPrefix = L"regex::";
    while (!token.empty() && token.back() == L'\r') token.pop_back();

    const bool explicitRegex = token.starts_with(regexPrefix);
    if (explicitRegex)
    {
        token.erase(0, regexPrefix.size());
        if (token.empty()) throw FilterSyntaxError{ IDS_PAGE_FILTERING_ERROR_EMPTY_VALUE };
    }
    else if (auto dsl = TranslateDsl(token, pathFilter))
    {
        token = std::move(*dsl);
        return true;
    }

    const bool useRegex = explicitRegex || defaultUseRegex;
    if (!useRegex && pathFilter)
    {
        while (!token.empty() && token.back() == L'\\') token.pop_back();
    }
    return useRegex;
}

static bool IsRemotePattern(const std::wstring_view pattern)
{
    return StorageSource::IsPath(pattern) || pattern.starts_with(L'^') && StorageSource::IsPath(pattern.substr(1));
}

static bool CompareThreshold(const ULONGLONG value, const ULONGLONG threshold, const int comparison)
{
    return comparison == 1 ? (value > threshold) : (value < threshold);
}

static bool CompareFileAge(const FILETIME& lastWriteTime, const FILETIME& cutoff, const int comparison)
{
    const int lastWriteCmp = CompareFileTime(&lastWriteTime, &cutoff);
    return comparison == 1 ? (lastWriteCmp < 0) : (lastWriteCmp > 0);
}

// Extracts the longest fixed-path prefix from an include-dir pattern that can
// be used as a scan anchor (i.e., the deepest directory that must exist for
// the pattern to ever match). Examples:
//   "C:\Windows\Sys*" -> "C:\Windows"
//   "*\foo" -> ""
std::wstring CFiltering::ExtractIncludeAnchor(const std::wstring_view pattern, const bool useRegex)
{
    constexpr std::wstring_view literalEscapes = LR"(\.+*?^$|()[]{}/)";
    constexpr std::wstring_view regexSpecials = LR"(.+*?^$|()[]{})";
    constexpr std::wstring_view globWildcards = L"[*?";

    std::wstring literal;
    bool truncated = false;
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        const wchar_t c = pattern[i];
        if (useRegex)
        {
            if (i == 0 && c == L'^') continue;
            if (c == L'\\' && i + 1 < pattern.size() &&
                (literalEscapes.contains(pattern[i + 1]) || iswspace(pattern[i + 1]) || iswpunct(pattern[i + 1])))
            {
                literal.push_back(pattern[++i]);
                continue;
            }
            if (c == L'$' && i + 1 == pattern.size()) break;
            if (c == L'\\' || regexSpecials.contains(c))
            {
                truncated = true;
                break;
            }
        }
        else if (globWildcards.contains(c))
        {
            truncated = true;
            break;
        }
        literal.push_back(c);
    }

    if (truncated)
    {
        // Trim wildcarded components; a partial remote authority provides no fixed directory anchor.
        const bool remote = StorageSource::IsPath(literal);
        const auto bs = literal.find_last_of(remote ? L'/' : L'\\');
        literal.resize(bs == std::wstring::npos || (remote && bs < literal.find(L"://") + 3) ? 0 : bs);
    }
    while (!literal.empty() && literal.back() == L'\\') literal.pop_back();
    return literal;
}

// In path filters, treat single backslashes as Windows separators even in regex
// mode, except for valid regex escapes, which retain their regex meaning.
std::wstring CFiltering::NormalizePathRegex(const std::wstring_view pattern)
{
    constexpr std::wstring_view preservedEscapes = LR"(\.+*?()[]{}^$|/bBdDsSwWfnrtv0cux123456789)";
    std::wstring result;
    result.reserve(pattern.size());
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == L'\\' &&
            (i + 1 >= pattern.size() || (!preservedEscapes.contains(pattern[i + 1]) &&
                !iswspace(pattern[i + 1]) && !iswpunct(pattern[i + 1]))))
        {
            result += L"\\\\";
        }
        else if (pattern[i] == L'\\')
        {
            result.push_back(pattern[i]);
            result.push_back(pattern[++i]);
        }
        else
        {
            result.push_back(pattern[i]);
        }
    }
    return result;
}

// --- Public methods ---

std::wregex CFiltering::CompilePattern(const std::wstring& pattern, const bool useRegex, const bool pathFilter)
{
    // In regex mode, normalize lone backslashes in path-based patterns so
    // users can type V:\Folder without having to escape the path separators.
    const std::wstring normalized = useRegex && pathFilter ? NormalizePathRegex(pattern) : pattern;

    // Directory filters apply to the directory itself and everything below it, whatever characters names contain.
    const bool remote = IsRemotePattern(pattern);
    std::wstring expression = useRegex ? normalized : GlobToRegex(normalized, false);
    if (pathFilter)
    {
        if (HasUnescapedTrailingDollar(expression)) expression.pop_back();
        expression = L"(?:" + expression + (remote ? L")(?:/[\\s\\S]*)?" : L")(?:\\\\[\\s\\S]*)?");
    }
    return std::wregex(expression, std::regex_constants::optimize |
        (remote ? std::regex_constants::ECMAScript : std::regex_constants::icase));
}

std::wstring CFiltering::FormatFilterError(
    std::wstring_view section, const size_t line, std::wstring_view token, const std::wstring_view detail)
{
    while (!section.empty() && (iswspace(section.back()) || section.back() == L':' || section.back() == L'：'))
    {
        section.remove_suffix(1);
    }
    while (!token.empty() && (token.back() == L'\r' || token.back() == L'\n'))
    {
        token.remove_suffix(1);
    }
    return detail.empty()
        ? std::format(L"{}({})\t{}", section, line, token)
        : std::format(L"{}({})\t{}\t{}", section, line, detail, token);
}

std::vector<CFiltering::FilterError> CFiltering::ValidateFilters(
    const std::wstring& text, const bool useRegex, const bool pathFilter)
{
    std::vector<FilterError> errors;
    size_t lineNumber = 0;
    for (auto& token : SplitString(text, L'\n'))
    {
        ++lineNumber;
        const std::wstring rawToken = token;
        try
        {
            const bool tokenUseRegex = PrepareFilterToken(token, useRegex, pathFilter);
            if (token.empty()) continue;
            CompilePattern(token, tokenUseRegex, pathFilter);
        }
        catch (const FilterSyntaxError& e) { errors.push_back({ lineNumber, rawToken, Localization::Lookup(e.detailKey) }); }
        catch (const std::regex_error& e) { errors.push_back({ lineNumber, rawToken, Localization::Lookup(RegexErrorDetail(e.code())) }); }
    }
    return errors;
}

void CFiltering::CompileFilters()
{
    ExcludeDirsRegex.clear();
    ExcludeFilesRegex.clear();
    IncludeDirsRegex.clear();
    IncludeFilesRegex.clear();
    IncludeDirsAnchors.clear();
    IncludeDirsMatchDescendants = true;
    std::vector<std::wstring> invalidFilters;

    for (const auto& [optionString, optionRegex, sectionId] : {
        std::tuple{COptions::FilteringExcludeDirs.Obj(), std::ref(ExcludeDirsRegex), IDS_PAGE_FILTERING_EXCLUDE_DIRS},
        std::tuple{COptions::FilteringExcludeFiles.Obj(), std::ref(ExcludeFilesRegex), IDS_PAGE_FILTERING_EXCLUDE_FILES},
        std::tuple{COptions::FilteringIncludeDirs.Obj(), std::ref(IncludeDirsRegex), IDS_PAGE_FILTERING_INCLUDE_DIRS},
        std::tuple{COptions::FilteringIncludeFiles.Obj(), std::ref(IncludeFilesRegex), IDS_PAGE_FILTERING_INCLUDE_FILES}})
    {
        const bool isIncludeDirs = &optionRegex.get() == &IncludeDirsRegex;
        const bool isExcludeDirs = &optionRegex.get() == &ExcludeDirsRegex;
        const bool isPathFilter = isIncludeDirs || isExcludeDirs;
        size_t lineNumber = 0;
        for (auto& token : SplitString(optionString, L'\n'))
        {
            ++lineNumber;
            const std::wstring rawToken = token;
            try
            {
                const bool useRegex = PrepareFilterToken(token, COptions::FilteringUseRegex, isPathFilter);
                if (token.empty()) continue;

                optionRegex.get().push_back(CompilePattern(token, useRegex, isPathFilter));

                if (isIncludeDirs)
                {
                    // Remote separators and end or lookahead assertions can depend on the appended filename.
                    if (useRegex)
                    {
                        std::wstring_view expression(token);
                        if (HasUnescapedTrailingDollar(expression)) expression.remove_suffix(1);
                        IncludeDirsMatchDescendants &= !IsRemotePattern(expression) && !expression.contains(L'$') &&
                            !expression.contains(L"(?=") && !expression.contains(L"(?!");
                    }
                    IncludeDirsAnchors.emplace_back(ExtractIncludeAnchor(useRegex
                        ? NormalizePathRegex(token) : token, useRegex));
                }
            }
            catch (const FilterSyntaxError& e)
            {
                invalidFilters.push_back(FormatFilterError(
                    Localization::Lookup(sectionId), lineNumber, rawToken, Localization::Lookup(e.detailKey)));
            }
            catch (const std::regex_error& e)
            {
                invalidFilters.push_back(FormatFilterError(
                    Localization::Lookup(sectionId), lineNumber, rawToken, Localization::Lookup(RegexErrorDetail(e.code()))));
            }
        }
    }

    if (!invalidFilters.empty())
    {
        CMessageBoxDlg::Show(Localization::Lookup(IDS_PAGE_FILTERING_INVALID_FILTER),
            invalidFilters, {}, false, MB_OK | MB_ICONERROR, nullptr, { 640, 380 });
    }

    // Calculate the total number of bytes to test as a scan minimum
    SizeMinimumCalculated = static_cast<ULONGLONG>(COptions::FilteringSizeMinimum) * (1ull << (10 * static_cast<int>(COptions::FilteringSizeUnits)));

    // Calculate the FILETIME cutoff for max-age filtering
    MaxAgeFileTimeCutoff = {};
    if (COptions::FilteringMaxAgeDays > 0)
    {
        uint64_t t = std::bit_cast<uint64_t>(CurrentSystemFileTime());
        t -= COptions::FilteringMaxAgeDays.Obj() * 864'000'000'000ULL; // 100ns ticks per day
        MaxAgeFileTimeCutoff = std::bit_cast<FILETIME>(t);
    }

    // Cache whether any filter is active so callers can short-circuit cheaply
    FilterActive = !ExcludeDirsRegex.empty() || !ExcludeFilesRegex.empty() ||
                   !IncludeDirsRegex.empty()  || !IncludeFilesRegex.empty() ||
                   SizeMinimumCalculated != 0  ||
                   std::bit_cast<ULONGLONG>(MaxAgeFileTimeCutoff) != 0;

    // Rebuild toolbar to reflect status
    if (auto* frame = CMainFrame::Get(); frame != nullptr) frame->RebuildToolBar();
}

std::wstring_view CFiltering::WithoutTrailingBackslashes(std::wstring_view path)
{
    const wchar_t separator = StorageSource::IsPath(path) ? L'/' : L'\\';
    if (separator == L'/') path = StorageSource::SplitOptions(path).first;
    while (!path.empty() && path.back() == separator) path.remove_suffix(1);
    return path;
}

bool CFiltering::MatchesAnyPath(const std::wstring& path, const std::vector<std::wregex>& patterns)
{
    if (patterns.empty()) return false;

    const auto matches = [&patterns](std::wstring_view candidate)
    {
        return std::ranges::any_of(patterns,
        [&candidate](const auto& pattern) { return std::regex_match(candidate.begin(), candidate.end(), pattern); });
    };

    if (matches(path)) return true;

    // Match remote ancestors at URI boundaries, preserving connection options after each candidate path.
    if (StorageSource::IsPath(path))
    {
        const auto [address, suffix] = StorageSource::SplitOptions(path);
        std::wstring candidate(address);
        const std::wstring options(suffix);
        const auto rootEnd = candidate.find(L'/', candidate.find(L"://") + 3);
        for (;;)
        {
            if (matches(candidate) || (!options.empty() && matches(candidate + options))) return true;
            if (candidate.ends_with(L'/')) { candidate.pop_back(); continue; }
            const auto slash = candidate.rfind(L'/');
            if (slash == candidate.npos || slash < rootEnd) return false;
            candidate.resize(slash + 1);
        }
    }

    const std::wstring_view trimmed = WithoutTrailingBackslashes(path);
    return trimmed != path && matches(trimmed);
}

bool CFiltering::IsFilteredOut(const std::wstring& directoryName, const bool scanRoot)
{
    if (!FilterActive) return false;
    if (MatchesAnyPath(directoryName, ExcludeDirsRegex)) return true;
    if (IncludeDirsRegex.empty()) return false;
    if (MatchesAnyPath(directoryName, IncludeDirsRegex)) return false;

    // Object scan roots may end within a key component rather than at a directory boundary.
    const bool objectPrefix = scanRoot &&
        (directoryName.starts_with(L"s3://") || directoryName.starts_with(L"azure://")) &&
        !StorageSource::SplitOptions(directoryName).first.ends_with(L'/');

    // Check if path is the same as or an ancestor of any include-anchor,
    // meaning we still need to descend into this directory to reach an included one.
    return !std::ranges::any_of(IncludeDirsAnchors, [&](const auto& anchor) -> bool
    {
        if (anchor.empty()) return true;
        const std::wstring_view a = WithoutTrailingBackslashes(anchor);
        const std::wstring_view d = WithoutTrailingBackslashes(directoryName);
        if (d.empty() || d.size() > a.size()) return false;
        const bool remote = StorageSource::IsPath(d);
        if (remote ? d != a.substr(0, d.size()) : _wcsnicmp(d.data(), a.data(), d.size()) != 0) return false;
        return d.size() == a.size() || objectPrefix || a[d.size()] == (remote ? L'/' : L'\\');
    });
}

bool CFiltering::CanReuseDirectoryMatch(const std::wstring_view childPrefix)
{
    // Only children filtered by this prefix plus their names can share the directory match.
    if (childPrefix.empty() || IncludeDirsRegex.empty() || !IncludeDirsMatchDescendants) return false;

    // Appending a separator to the trimmed path preserves word-boundary assertions.
    return MatchesAnyPath(std::wstring(WithoutTrailingBackslashes(childPrefix)), IncludeDirsRegex);
}

bool CFiltering::IsFilteredOut(const std::wstring_view fileName, const std::wstring& filePath,
    const ULONGLONG fileSizeLogical, const FILETIME& lastWriteTime, const bool directoryIncluded)
{
    if (!FilterActive) return false;

    // Exclude files beyond the size threshold based on comparison selection
    if (SizeMinimumCalculated > 0 &&
        CompareThreshold(fileSizeLogical, SizeMinimumCalculated, COptions::FilteringSizeComparison))
    {
        return true;
    }

    // Exclude files outside the max-age cutoff based on comparison selection
    if (std::bit_cast<ULONGLONG>(MaxAgeFileTimeCutoff) != 0)
    {
        if (CompareFileAge(lastWriteTime, MaxAgeFileTimeCutoff, COptions::FilteringMaxAgeComparison))
            return true;
    }

    // Exclude files matching name filter
    if (!ExcludeFilesRegex.empty() && std::ranges::any_of(ExcludeFilesRegex,
        [&fileName](const auto& pattern) { return std::regex_match(fileName.begin(), fileName.end(), pattern); }))
    {
        return true;
    }

    // Include only files whose path matches the include directory filter
    if (!directoryIncluded && !IncludeDirsRegex.empty() && !MatchesAnyPath(filePath, IncludeDirsRegex))
    {
        return true;
    }

    // Include only files whose name matches the include name filter
    if (!IncludeFilesRegex.empty() && std::ranges::none_of(IncludeFilesRegex,
        [&fileName](const auto& pattern) { return std::regex_match(fileName.begin(), fileName.end(), pattern); }))
    {
        return true;
    }

    return false;
}

bool CFiltering::IsFilteredOut(const CItem* item)
{
    if (!FilterActive) return false;
    if (item->IsTypeOrFlag(IT_FILE))
        return IsFilteredOut(item->GetName(), IncludeDirsRegex.empty() ? std::wstring() : item->GetPath(),
            item->GetSizeLogical(), item->GetLastChange());
    return IsFilteredOut(item->GetPath(), item->IsTypeOrFlag(ITF_REMOTE) && item->IsScanRoot());
}
