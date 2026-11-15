        {
            const auto program = sandbox + L"\\安装 路径\\DesktopBox";
            const auto chosen = sandbox + L"\\自选 收纳位置";
            EnsureDirectory(program);
            expect(box::InstalledDataDirectory(program).empty(), "an unconfigured executable retains the normal per-user fallback");
            expect(WriteBytes(program + L"\\portable.flag", "", 0), "create legacy portable installation fixture");
            expect(box::InstalledDataDirectory(program) == program + L"\\data", "existing portable installations keep their original data folder");
            box::SaveDataDirectory(program, chosen);
            expect(box::InstalledDataDirectory(program) == chosen &&
                   box::InstalledDataDirectory(program, sandbox + L"\\explicit") == sandbox + L"\\explicit",
                   "installer-selected Unicode data folder persists and explicit data-dir still takes precedence");
            const auto original = sandbox + L"\\保存位置验证.txt";
            WriteBytes(original, "chosen-location", 15);
            Storage selected(box::InstalledDataDirectory(program), program + L"\\ico");
            expect(selected.MoveInto({original}, nullptr, true).complete &&
                   ReadSmallFile(chosen + L"\\items\\保存位置验证.txt") == "chosen-location" &&
                   GetFileAttributesW((program + L"\\data\\items").c_str()) == INVALID_FILE_ATTRIBUTES,
                   "actual file bytes are moved into the selected folder rather than beside the executable");
            Storage reopened(box::InstalledDataDirectory(program), program + L"\\ico");
            expect(reopened.OriginalPath(chosen + L"\\items\\保存位置验证.txt") == original &&
                   reopened.RestoreAll(nullptr, L"", true).complete && ReadSmallFile(original) == "chosen-location",
                   "fresh storage instances keep origin records and restore files from the selected folder");
            const auto config = program + L"\\storage.ini";
            HANDLE locked = CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            expect(locked != INVALID_HANDLE_VALUE, "lock existing location configuration for a failed-save fixture");
            bool failed = false;
            try { box::SaveDataDirectory(program, sandbox + L"\\replacement"); } catch (HRESULT) { failed = true; }
            CloseHandle(locked);
            expect(failed && box::InstalledDataDirectory(program) == chosen &&
                   GetFileAttributesW((config + L".new").c_str()) == INVALID_FILE_ATTRIBUTES,
                   "failed configuration replacement preserves the previously selected folder");
            const std::wstring invalid = L"\xFEFF[DesktopBox]\r\nDataDirectory=relative\\folder\r\n";
            WriteBytes(config, invalid.data(), static_cast<DWORD>(invalid.size() * sizeof(wchar_t)));
            bool rejected = false;
            try { box::InstalledDataDirectory(program); } catch (HRESULT) { rejected = true; }
            expect(rejected && box::InstalledDataDirectory(program, chosen) == chosen,
                   "invalid saved paths fail visibly instead of silently opening an empty portable folder");
        }
