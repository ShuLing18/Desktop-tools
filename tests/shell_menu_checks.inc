// Exercise command routing without changing the real user's Start menu or opening Explorer.
{
    struct MenuCommand : IContextMenu {
        std::wstring verb = L"PinToStartScreen";
        HRESULT result = E_ACCESSDENIED;
        bool ansiOnly = false, hasVerb = true;
        unsigned invokes = 0;
        CMINVOKECOMMANDINFOEX seen{};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (iid != IID_IUnknown && iid != IID_IContextMenu) return E_NOINTERFACE;
            *object = static_cast<IContextMenu*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* command) override {
            ++invokes;
            if (!command || command->cbSize != sizeof(seen)) return E_INVALIDARG;
            seen = *reinterpret_cast<CMINVOKECOMMANDINFOEX*>(command);
            return result;
        }
        HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR offset, UINT flags, UINT*, LPSTR text, UINT count) override {
            if (offset != 7 || !hasVerb) return E_NOTIMPL;
            if (flags == GCS_VERBW && !ansiOnly) {
                lstrcpynW(reinterpret_cast<LPWSTR>(text), verb.c_str(), count); return S_OK;
            }
            if (flags == GCS_VERBA) {
                const std::string ascii(verb.begin(), verb.end()); lstrcpynA(text, ascii.c_str(), count); return S_OK;
            }
            return E_NOTIMPL;
        }
    } menu;
    const HWND owner = GetDesktopWindow(); const POINT point{123, 456};
    unsigned fallbacks = 0, renames = 0;
    const auto continueInExplorer = [&]() -> HRESULT { ++fallbacks; return S_OK; };
    auto invoke = [&] { return box::ShellMenu::Invoke(owner, &menu, 7, point, {}, continueInExplorer); };
    expect(invoke() == S_OK && menu.invokes == 1 && fallbacks == 1,
           "a denied Start pin offers Explorer continuation after exactly one native invocation");
    expect(menu.seen.hwnd == owner && reinterpret_cast<UINT_PTR>(menu.seen.lpVerb) == 7 &&
           reinterpret_cast<UINT_PTR>(menu.seen.lpVerbW) == 7 && menu.seen.ptInvoke.x == point.x && menu.seen.ptInvoke.y == point.y &&
           (menu.seen.fMask & (CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_FLAG_LOG_USAGE)) ==
               (CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_FLAG_LOG_USAGE),
           "Shell commands retain the selected command offset owner coordinates and user-action flags");
    menu.result = S_OK;
    expect(invoke() == S_OK && menu.invokes == 2 && fallbacks == 1,
           "a successful pin toggle is never retried or routed through Explorer");
    menu.result = E_ACCESSDENIED; menu.verb = L"delete";
    expect(invoke() == E_ACCESSDENIED && fallbacks == 1,
           "access denied from deleting a file remains an error and never triggers a pin continuation");
    menu.verb = L"PinToStartScreen"; menu.result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    expect(invoke() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && fallbacks == 1,
           "a missing pin target is reported instead of being treated as a Windows pin restriction");
    menu.result = E_ACCESSDENIED; menu.hasVerb = false;
    expect(invoke() == E_ACCESSDENIED && fallbacks == 1,
           "a menu handler without a canonical verb cannot redirect an unknown operation");
    menu.hasVerb = true; menu.ansiOnly = true; menu.verb = L"pInToStArTsCrEeN";
    expect(invoke() == S_OK && fallbacks == 2,
           "ANSI-only Shell handlers and mixed-case Start pin verbs use the same continuation");
    menu.ansiOnly = false; menu.verb = L"startunpin";
    expect(invoke() == S_OK && fallbacks == 3,
           "a denied unpin command can also continue with the original item in Explorer");
    expect(box::ShellMenu::Invoke(owner, &menu, 7, point, {}, [] { return E_FAIL; }) == E_FAIL,
           "failure to show the Explorer continuation is not reported as a successful pin");
    menu.verb = L"rename"; const auto priorInvokes = menu.invokes;
    expect(box::ShellMenu::Invoke(owner, &menu, 7, point, [&] { ++renames; }, continueInExplorer) == S_OK &&
           renames == 1 && menu.invokes == priorInvokes && fallbacks == 3,
           "the existing in-panel rename path still bypasses native command invocation");
    expect(box::ShellMenu::Invoke(owner, nullptr, 7, point) == E_POINTER &&
           box::ShellMenu::Invoke(owner, &menu, 65536, point) == E_INVALIDARG && menu.invokes == priorInvokes,
           "invalid Shell command inputs cannot invoke an unrelated command");
    expect(box::ShowShellSelectionInExplorer({}) == E_INVALIDARG,
           "an empty Explorer continuation cannot open or select an unrelated item");
}
