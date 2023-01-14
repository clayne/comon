/*
   Copyright 2022 Sebastian Solnica

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>
#include <span>

#include <DbgEng.h>
#include <wil/com.h>

#include "comon.h"
#include "dbgsession.h"

using namespace comon_ext;

namespace fs = std::filesystem;

namespace {
dbgsession g_dbgsession{};

const wchar_t* monitor_not_enabled_error{ L"COM monitor not enabled for the current process." };

std::vector<std::string> split_args(std::string_view args) {
    char citation_char{ '\0' };
    std::vector<std::string> vargs{};
    std::string token{};

    for (auto c : args) {
        if (citation_char != '\0') {
            if (c == citation_char) {
                if (!token.empty()) {
                    vargs.push_back(token);
                    token.clear();
                }
                citation_char = '\0';
            } else {
                token.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            citation_char = c;
        } else if (std::isspace(c) || c == ',') {
            if (!token.empty()) {
                vargs.push_back(token);
                token.clear();
            }
        } else {
            token.push_back(c);
        }
    }

    if (!token.empty()) {
        vargs.push_back(token);
    }

    return vargs;
}
}

extern "C" HRESULT CALLBACK DebugExtensionInitialize(PULONG version, PULONG flags) {
    *version = DEBUG_EXTENSION_VERSION(EXT_MAJOR_VER, EXT_MINOR_VER);
    *flags = 0;
    return S_OK;
}

extern "C" void CALLBACK DebugExtensionNotify([[maybe_unused]] ULONG notify, [[maybe_unused]] ULONG64 argument) {}

extern "C" void CALLBACK DebugExtensionUninitialize(void) { g_dbgsession.detach(); }

extern "C" HRESULT CALLBACK cometa(IDebugClient * dbgclient, PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    auto vargs{ split_args(args) };

    if (vargs.size() == 0) {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
        return E_INVALIDARG;
    }

    auto& cometa{ g_dbgsession.get_metadata() };
    if (vargs[0] == "index") {
        return vargs.size() == 1 ? cometa.index() : cometa.index(widen(vargs[1]));
    } else if (vargs[0] == "save") {
        if (vargs.size() != 2) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
            return E_INVALIDARG;
        }
        return cometa.save(widen(vargs[1]));
    } else if (vargs[0] == "showi") {
        if (vargs.size() != 2) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
            return E_INVALIDARG;
        }
        if (IID iid{}; SUCCEEDED(try_parse_guid(widen(vargs[1]), iid))) {
            if (auto cotype{ cometa.resolve_type(iid) }; cotype) {
                dbgcontrol->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                    std::format(L"Found: {:b} ({})\n\n", iid, cotype->name).c_str());

                if (auto methods{ cometa.get_type_methods(iid) }; methods) {
                    dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"Methods:\n");
                    for (size_t i = 0; i < methods->size(); i++) {
                        dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, std::format(L"- [{}] {}\n", i, methods->at(i)).c_str());
                    }
                } else {
                    dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"No information about the interface methods :(\n");
                }
            } else {
                dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL,
                    std::format(L"Can't find any details on IID: {:b} in the metadata.\n", iid).c_str());
            }

            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"\nRegistered VTables for IID:\n");
            for (auto& [module_name, clsid, is_64bit, vtbl] : cometa.find_vtables_by_iid(iid)) {
                auto clsid_name{ cometa.resolve_class_name(clsid) };
                dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL,
                    std::format(L"- Module: {} ({}), CLSID: {:b} ({}), VTable offset: {:#x}\n", module_name,
                        is_64bit ? L"64-bit" : L"32-bit", clsid, clsid_name ? *clsid_name : L"N/A", vtbl)
                    .c_str());
            }
            return S_OK;
        } else {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: incorrect format of IID.\n");
            return E_INVALIDARG;
        }
    } else if (vargs[0] == "showc") {
        if (vargs.size() != 2) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
            return E_INVALIDARG;
        }
        if (CLSID clsid{}; SUCCEEDED(try_parse_guid(widen(vargs[1]), clsid))) {
            if (auto coclass{ cometa.resolve_class(clsid) }; coclass) {
                dbgcontrol->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL,
                    std::format(L"Found: {:b} ({})\n", clsid, coclass->name).c_str());
            } else {
                dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL,
                    std::format(L"Can't find any details on CLSID: {:b} in the metadata.\n", clsid).c_str());
            }

            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"\nRegistered VTables for CLSID:\n");
            for (auto& [module_name, iid, is_64bit, vtbl] : cometa.find_vtables_by_clsid(clsid)) {
                auto iid_name{ cometa.resolve_type_name(iid) };
                dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL,
                    std::format(L"- module: {} ({}), IID: {:b} ({}), VTable offset: {:#x}\n", module_name,
                        is_64bit ? L"64-bit" : L"32-bit", iid, iid_name ? *iid_name : L"N/A", vtbl)
                    .c_str());
            }
            return S_OK;
        } else {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: incorrect format of CLSID.\n");
            return E_INVALIDARG;
        }
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: unknown subcommand. Run !cohelp to check the syntax.\n");
        return E_INVALIDARG;
    }
}

extern "C" HRESULT CALLBACK cobp(IDebugClient * dbgclient, PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    auto vargs{ split_args(args) };

    if (vargs.size() < 3) {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
        return E_INVALIDARG;
    }

    CLSID clsid;
    RETURN_IF_FAILED(try_parse_guid(widen(vargs[0]), clsid));
    IID iid;
    RETURN_IF_FAILED(try_parse_guid(widen(vargs[1]), iid));

    if (auto monitor{ g_dbgsession.find_active_monitor() }; monitor) {
        try {
            DWORD method_num{ std::stoul(vargs[2]) };
            return monitor->create_cobreakpoint(clsid, iid, method_num);
        } catch (const std::invalid_argument&) {
            // we will try with a method name
            return monitor->create_cobreakpoint(clsid, iid, widen(vargs[2]));
        }
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, monitor_not_enabled_error);
        return E_FAIL;
    }
}

extern "C" HRESULT CALLBACK cobl([[maybe_unused]] IDebugClient * dbgclient, [[maybe_unused]] PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    if (auto monitor{ g_dbgsession.find_active_monitor() }; monitor) {
        for (const auto& [id, desc, addr] : monitor->list_breakpoints()) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, std::format(L"{}: {}, address: {:#x}\n", id, desc, addr).c_str());
        }
        return S_OK;
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, monitor_not_enabled_error);
        return E_FAIL;
    }
}

extern "C" HRESULT CALLBACK cobd([[maybe_unused]] IDebugClient * dbgclient, PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    if (auto monitor{ g_dbgsession.find_active_monitor() }; monitor) {
        return monitor->remove_cobreakpoint(std::stoul(args));
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, monitor_not_enabled_error);
        return E_FAIL;
    }
}

extern "C" HRESULT CALLBACK coreg(IDebugClient * dbgclient, PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    auto vargs{ split_args(args) };

    if (vargs.size() < 3) {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
        return E_INVALIDARG;
    }

    CLSID clsid;
    RETURN_IF_FAILED(try_parse_guid(widen(vargs[0]), clsid));
    IID iid;
    RETURN_IF_FAILED(try_parse_guid(widen(vargs[1]), iid));

    if (auto monitor{ g_dbgsession.find_active_monitor() }; monitor) {
        ULONG64 vtable_addr{ std::stoull(vargs[2]) };
        return monitor->register_vtable(clsid, iid, vtable_addr, false);
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, monitor_not_enabled_error);
        return E_FAIL;
    }
}

extern "C" HRESULT CALLBACK comon(IDebugClient * dbgclient, PCSTR args) {
    wil::com_ptr_t<IDebugControl4> dbgcontrol;
    RETURN_IF_FAILED(dbgclient->QueryInterface(__uuidof(IDebugControl4), dbgcontrol.put_void()));

    auto print_filter = [&dbgcontrol](const cofilter& filter) {
        auto print_clsids = [&dbgcontrol](const std::unordered_set<CLSID>& clsids) {
            for (auto& clsid : clsids) {
                dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, std::format(L"- {:b}\n", clsid).c_str());
            }
        };

        if (auto fltr = std::get_if<including_filter>(&filter); fltr) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"\nCLSIDs to monitor:\n");
            print_clsids(fltr->clsids);
            return;
        }
        if (auto fltr = std::get_if<excluding_filter>(&filter); fltr) {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"\nCLSIDs to EXCLUDE while monitoring:\n");
            print_clsids(fltr->clsids);
            return;
        }
        assert(std::holds_alternative<no_filter>(filter));
    };

    auto parse_filter = [](std::span<const std::string> args) -> cofilter {
        std::unordered_set<CLSID> clsids{};
        for (auto iter{ std::crbegin(args) }; iter != std::crend(args); iter++) {
            if (*iter == "-i") {
                return including_filter{ clsids };
            }
            if (*iter == "-e") {
                return excluding_filter{ clsids };
            }
            GUID clsid;
            if (SUCCEEDED(try_parse_guid(widen(*iter), clsid))) {
                clsids.insert(clsid);
            }
        }
        if (clsids.size() > 0) {
            return including_filter{ clsids };
        }
        return no_filter{};
    };

    auto vargs{ split_args(args) };
    if (vargs.size() < 1) {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
        return E_INVALIDARG;
    }

    if (auto monitor{ g_dbgsession.find_active_monitor() }; monitor) {
        if (vargs[0] == "attach") {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"COM monitor is already enabled for the current process.");
            return E_FAIL;
        } else if (vargs[0] == "pause") {
            monitor->pause();
        } else if (vargs[0] == "resume") {
            monitor->resume();
        } else if (vargs[0] == "detach") {
            g_dbgsession.detach();
        } else if (vargs[0] == "status") {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, std::format(L"COM monitor is {}\n",
                monitor->is_paused() ? L"PAUSED" : L"RUNNING").c_str());

            auto& cometa{ g_dbgsession.get_metadata() };
            dbgcontrol->OutputWide(DEBUG_OUTPUT_NORMAL, L"\nCOM types recorded for the current process:\n");
            for (auto& [clsid, vtables] : monitor->list_cotypes()) {
                auto clsid_name{ cometa.resolve_class_name(clsid) };
                dbgcontrol->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL, 
                    std::format(L"\n<col fg=\"srcannot\">CLSID: <b>{:b} ({})</b></col>\n", clsid, clsid_name ? *clsid_name : L"N/A").c_str());
                for (auto& [addr, iid] : vtables) {
                    auto iid_name{ cometa.resolve_type_name(iid) };
                    dbgcontrol->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL, 
                        std::format(L"  IID: <b>{:b} ({})</b>, address: {:#x}\n", iid, iid_name ? *iid_name : L"N/A", addr).c_str());
                }
            }
        } else {
            dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, L"ERROR: invalid arguments. Run !cohelp to check the syntax.\n");
            return E_INVALIDARG;
        }
        return S_OK;
    } else if (vargs[0] == "attach") {
        auto filter = parse_filter(std::span{ vargs }.subspan(1));
        g_dbgsession.attach(filter);
        dbgcontrol->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL, L"<b>COM monitor enabled for the current process.</b>\n");
        print_filter(filter);
        return S_OK;
    } else {
        dbgcontrol->OutputWide(DEBUG_OUTPUT_ERROR, monitor_not_enabled_error);
        return E_FAIL;
    }
}
