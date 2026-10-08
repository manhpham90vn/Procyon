// Windows services (Service Control Manager) and startup items (Run keys, Startup folders, with
// the enabled state Task Manager keeps under StartupApproved).
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <taskschd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#pragma comment(lib, "taskschd.lib")

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace procyon::platform {

using namespace win;

namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE handle) : h(handle) {}
    ~ScHandle() {
        if (h) CloseServiceHandle(h);
    }
    ScHandle(const ScHandle &) = delete;
    ScHandle &operator=(const ScHandle &) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// ---- startup items ----

constexpr const char *kRunPrefix = "run:";
constexpr const char *kFolderPrefix = "folder:";
const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *kRun32Key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";  // under WOW6432Node
const wchar_t *kApprovedRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
const wchar_t *kApprovedRun32 = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
const wchar_t *kApprovedFolder =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";

// Task Manager's record: 12 bytes, the first is 0x02 (enabled) or 0x03 (disabled), then the time
// it was disabled. Missing means enabled. Odd values are the disabled ones.
bool approved(HKEY root, const wchar_t *key, const std::wstring &name) {
    BYTE data[64];
    DWORD size = sizeof(data), type = 0;
    if (RegGetValueW(root, key, name.c_str(), RRF_RT_REG_BINARY, &type, data, &size) != ERROR_SUCCESS || size < 1)
        return true;
    return (data[0] & 1) == 0;
}

pc_result set_approved(HKEY root, const wchar_t *key, const std::wstring &name, bool enabled) {
    HKEY handle = nullptr;
    const LSTATUS opened =
        RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &handle, nullptr);
    if (opened != ERROR_SUCCESS) return error_result(static_cast<DWORD>(opened));
    BYTE data[12] = {};
    data[0] = enabled ? 0x02 : 0x03;
    if (!enabled) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        std::memcpy(data + 4, &now, sizeof(now));
    }
    const LSTATUS written = RegSetValueExW(handle, name.c_str(), 0, REG_BINARY, data, sizeof(data));
    RegCloseKey(handle);
    return error_result(static_cast<DWORD>(written));
}

struct RunSource {
    HKEY root;
    const wchar_t *key;
    const wchar_t *approved_key;
    REGSAM view;  // KEY_WOW64_64KEY or KEY_WOW64_32KEY
    const char *tag;
    int32_t scope;
};

const RunSource kRunSources[] = {
    {HKEY_CURRENT_USER, kRunKey, kApprovedRun, KEY_WOW64_64KEY, "hkcu", PC_STARTUP_USER_AGENT},
    {HKEY_LOCAL_MACHINE, kRunKey, kApprovedRun, KEY_WOW64_64KEY, "hklm", PC_STARTUP_GLOBAL_AGENT},
    {HKEY_LOCAL_MACHINE, kRun32Key, kApprovedRun32, KEY_WOW64_32KEY, "hklm32", PC_STARTUP_GLOBAL_AGENT},
};

void run_key_items(const RunSource &source, std::vector<pc_startup_item> &out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(source.root, source.key, 0, KEY_READ | source.view, &key) != ERROR_SUCCESS) return;
    for (DWORD index = 0;; ++index) {
        wchar_t name[16384];
        DWORD name_size = static_cast<DWORD>(std::size(name)), type = 0;
        BYTE data[8192];
        DWORD data_size = sizeof(data);
        const LSTATUS status = RegEnumValueW(key, index, name, &name_size, nullptr, &type, data, &data_size);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        const std::wstring value_name(name, name_size);
        auto text = reinterpret_cast<const wchar_t *>(data);
        const std::string command = utf8(text, wcsnlen(text, data_size / sizeof(wchar_t)));
        if (command.empty()) continue;

        pc_startup_item item{};
        copy_string(item.label, sizeof(item.label), std::string(kRunPrefix) + source.tag + ":" + utf8(value_name));
        const std::string executable = executable_of_command(command);
        // The program's own name ("Microsoft Edge"), as Task Manager and the macOS app show it; the
        // value name ("MicrosoftEdgeAutoLaunch_BF89…") only when the executable can't be read.
        std::string display = executable.empty() ? std::string() : product_name(executable);
        if (display.empty() || GetFileAttributesW(wide(executable).c_str()) == INVALID_FILE_ATTRIBUTES)
            display = utf8(value_name);
        copy_string(item.name, sizeof(item.name), display);
        // program is the executable (procyon.h), as on macOS and Linux; app_path is for app bundles.
        copy_string(item.program, sizeof(item.program), executable.empty() ? command : executable);
        copy_string(item.config_path, sizeof(item.config_path),
                    std::string(source.root == HKEY_CURRENT_USER ? "HKCU\\" : "HKLM\\") + utf8(source.key) +
                        (source.view == KEY_WOW64_32KEY ? " (32-bit)" : ""));
        item.scope = source.scope;
        item.pid = executable.empty() ? 0 : pid_of_executable(executable);
        item.enabled = approved(source.root, source.approved_key, value_name);
        item.run_at_load = true;
        out.push_back(item);
    }
    RegCloseKey(key);
}

std::string known_folder(const KNOWNFOLDERID &id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path)) || !path) return {};
    std::string out = utf8(path, wcslen(path));
    CoTaskMemFree(path);
    return out;
}

// Target of a .lnk shortcut, empty when it can't be resolved.
std::string shortcut_target(const std::wstring &link) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string target;
    IShellLinkW *shell_link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
        IPersistFile *file = nullptr;
        if (SUCCEEDED(shell_link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
                wchar_t path[MAX_PATH] = {};
                if (SUCCEEDED(shell_link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                    wchar_t expanded[MAX_PATH] = {};
                    if (ExpandEnvironmentStringsW(path, expanded, MAX_PATH) > 0)
                        target = utf8(expanded, wcslen(expanded));
                    else
                        target = utf8(path, wcslen(path));
                }
            }
            file->Release();
        }
        shell_link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return target;
}

void folder_items(const std::string &folder, HKEY root, const char *tag, int32_t scope,
                  std::vector<pc_startup_item> &out) {
    if (folder.empty()) return;
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = wide(folder) + L"\\*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring file_name = found.cFileName;
        const std::string name = utf8(file_name);
        if (lower_ascii(name) == "desktop.ini") continue;
        const std::string full = folder + "\\" + name;
        std::string target = full;
        if (const std::string lowered = lower_ascii(name);
            lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, ".lnk") == 0)
            target = shortcut_target(wide(full));

        pc_startup_item item{};
        copy_string(item.label, sizeof(item.label), std::string(kFolderPrefix) + tag + ":" + name);
        std::string display = name;
        if (const auto dot = display.find_last_of('.'); dot != std::string::npos && dot > 0) display.resize(dot);
        copy_string(item.name, sizeof(item.name), display);
        copy_string(item.program, sizeof(item.program), target);
        copy_string(item.config_path, sizeof(item.config_path), full);
        item.scope = scope;
        item.pid = target.empty() ? 0 : pid_of_executable(target);
        item.enabled = approved(root, kApprovedFolder, file_name);
        item.run_at_load = true;
        out.push_back(item);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// "run:hkcu:Name" / "folder:user:Name.lnk" -> where its enabled flag lives.
bool parse_startup_label(const std::string &label, HKEY &root, const wchar_t *&key, std::wstring &name) {
    if (label.rfind(kRunPrefix, 0) == 0) {
        const std::string rest = label.substr(std::strlen(kRunPrefix));
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) return false;
        const std::string tag = rest.substr(0, colon);
        name = wide(rest.substr(colon + 1));
        for (const RunSource &source : kRunSources) {
            if (tag != source.tag) continue;
            root = source.root;
            key = source.approved_key;
            return !name.empty();
        }
        return false;
    }
    if (label.rfind(kFolderPrefix, 0) == 0) {
        const std::string rest = label.substr(std::strlen(kFolderPrefix));
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) return false;
        const std::string tag = rest.substr(0, colon);
        name = wide(rest.substr(colon + 1));
        if (tag == "user")
            root = HKEY_CURRENT_USER;
        else if (tag == "common")
            root = HKEY_LOCAL_MACHINE;
        else
            return false;
        key = kApprovedFolder;
        return !name.empty();
    }
    return false;
}

// ---- Task Scheduler: tasks that run at logon or at boot ----
//
// The third place Windows starts programs from. Microsoft's own tasks live under \Microsoft and are
// the OS (like /System/Library/LaunchAgents on macOS): they are left out. A task's label is
// "task:" plus its path with '|' for '\' (labels may not contain path separators).

constexpr const char *kTaskPrefix = "task:";

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

template <typename T>
struct Com {
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string bstr_utf8(BSTR text) {
    std::string out = text ? utf8(text, SysStringLen(text)) : std::string();
    SysFreeString(text);
    return out;
}

std::string task_label(const std::string &path) {
    std::string label = std::string(kTaskPrefix) + path;
    for (char &c : label)
        if (c == '\\') c = '|';
    return label;
}

std::string task_path_of_label(const std::string &label) {
    std::string path = label.substr(std::strlen(kTaskPrefix));
    for (char &c : path)
        if (c == '|') c = '\\';
    return path;
}

bool task_service(Com<ITaskService> &service) {
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                reinterpret_cast<void **>(&service))) ||
        !service)
        return false;
    VARIANT empty;
    VariantInit(&empty);
    return SUCCEEDED(service->Connect(empty, empty, empty, empty));
}

// The startup scope of a task from its triggers: boot (as SYSTEM, like a daemon), logon of every
// user, or logon of one user; -1 when it has no such trigger.
int32_t task_scope(ITaskDefinition *definition) {
    Com<ITriggerCollection> triggers;
    if (FAILED(definition->get_Triggers(&triggers)) || !triggers) return -1;
    LONG count = 0;
    triggers->get_Count(&count);
    int32_t scope = -1;
    for (LONG i = 1; i <= count; ++i) {
        Com<ITrigger> trigger;
        if (FAILED(triggers->get_Item(i, &trigger)) || !trigger) continue;
        TASK_TRIGGER_TYPE2 type{};
        trigger->get_Type(&type);
        VARIANT_BOOL enabled = VARIANT_TRUE;
        trigger->get_Enabled(&enabled);
        if (!enabled) continue;
        if (type == TASK_TRIGGER_BOOT) return PC_STARTUP_DAEMON;
        if (type != TASK_TRIGGER_LOGON) continue;
        Com<ILogonTrigger> logon;
        BSTR user = nullptr;
        if (SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void **>(&logon))) && logon)
            logon->get_UserId(&user);
        const std::string who = bstr_utf8(user);
        scope = who.empty() ? PC_STARTUP_GLOBAL_AGENT : std::max<int32_t>(scope, PC_STARTUP_USER_AGENT);
    }
    return scope;
}

// The first program the task runs ("C:\...\app.exe" and its arguments), empty when it runs none.
std::string task_command(ITaskDefinition *definition, std::string &executable) {
    executable.clear();
    Com<IActionCollection> actions;
    if (FAILED(definition->get_Actions(&actions)) || !actions) return {};
    LONG count = 0;
    actions->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        Com<IAction> action;
        if (FAILED(actions->get_Item(i, &action)) || !action) continue;
        TASK_ACTION_TYPE type{};
        action->get_Type(&type);
        if (type != TASK_ACTION_EXEC) continue;
        Com<IExecAction> exec;
        if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void **>(&exec))) || !exec) continue;
        BSTR path = nullptr, arguments = nullptr;
        exec->get_Path(&path);
        exec->get_Arguments(&arguments);
        std::string command = bstr_utf8(path);
        const std::string args = bstr_utf8(arguments);
        if (command.empty()) continue;
        executable = executable_of_command(command);
        if (!args.empty()) command += " " + args;
        return command;
    }
    return {};
}

void add_task(IRegisteredTask *task, std::vector<pc_startup_item> &out) {
    Com<ITaskDefinition> definition;
    if (FAILED(task->get_Definition(&definition)) || !definition) return;
    const int32_t scope = task_scope(definition.p);
    if (scope < 0) return;
    std::string executable;
    const std::string command = task_command(definition.p, executable);
    if (command.empty()) return;  // COM handlers and e-mail actions aren't startup programs
    BSTR name = nullptr, path = nullptr;
    task->get_Name(&name);
    task->get_Path(&path);
    const std::string task_name = bstr_utf8(name);
    const std::string task_path = bstr_utf8(path);
    VARIANT_BOOL enabled = VARIANT_TRUE;
    task->get_Enabled(&enabled);

    pc_startup_item item{};
    copy_string(item.label, sizeof(item.label), task_label(task_path));
    std::string display = executable.empty() ? std::string() : product_name(executable);
    if (display.empty() || GetFileAttributesW(wide(executable).c_str()) == INVALID_FILE_ATTRIBUTES) display = task_name;
    copy_string(item.name, sizeof(item.name), display);
    copy_string(item.program, sizeof(item.program), executable.empty() ? command : executable);
    copy_string(item.config_path, sizeof(item.config_path), "Task Scheduler " + task_path);
    item.scope = scope;
    item.pid = executable.empty() ? 0 : pid_of_executable(executable);
    item.enabled = enabled != VARIANT_FALSE;
    item.run_at_load = true;
    out.push_back(item);
}

void task_items_in(ITaskFolder *folder, std::vector<pc_startup_item> &out, int depth) {
    if (depth > 8) return;
    Com<IRegisteredTaskCollection> tasks;
    if (SUCCEEDED(folder->GetTasks(0, &tasks)) && tasks) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            Com<IRegisteredTask> task;
            if (FAILED(tasks->get_Item(index, &task)) || !task) continue;
            add_task(task.p, out);
        }
    }
    Com<ITaskFolderCollection> folders;
    if (SUCCEEDED(folder->GetFolders(0, &folders)) && folders) {
        LONG count = 0;
        folders->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            Com<ITaskFolder> child;
            if (FAILED(folders->get_Item(index, &child)) || !child) continue;
            BSTR path = nullptr;
            child->get_Path(&path);
            if (lower_ascii(bstr_utf8(path)) == "\\microsoft") continue;  // Windows' own tasks
            task_items_in(child.p, out, depth + 1);
        }
    }
}

void task_items(std::vector<pc_startup_item> &out) {
    ComScope com;
    Com<ITaskService> service;
    if (!task_service(service)) return;
    Com<ITaskFolder> root;
    BSTR root_path = SysAllocString(L"\\");
    const HRESULT got = service->GetFolder(root_path, &root);
    SysFreeString(root_path);
    if (FAILED(got) || !root) return;
    task_items_in(root.p, out, 0);
}

pc_result hresult_result(HRESULT hr) {
    if (SUCCEEDED(hr)) return PC_OK;
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return PC_ERR_PERMISSION;
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return PC_ERR_NOT_FOUND;
    return PC_ERR_FAILED;
}

pc_result set_task_enabled(const std::string &path, bool enabled) {
    ComScope com;
    Com<ITaskService> service;
    if (!task_service(service)) return PC_ERR_FAILED;
    Com<ITaskFolder> root;
    BSTR root_path = SysAllocString(L"\\");
    const HRESULT got = service->GetFolder(root_path, &root);
    SysFreeString(root_path);
    if (FAILED(got) || !root) return hresult_result(got);
    Com<IRegisteredTask> task;
    BSTR task_path = SysAllocString(wide(path).c_str());
    const HRESULT found = root->GetTask(task_path, &task);
    SysFreeString(task_path);
    if (FAILED(found) || !task) return hresult_result(found);
    return hresult_result(task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE));
}

bool is_startup_label(const std::string &label) {
    return label.rfind(kRunPrefix, 0) == 0 || label.rfind(kFolderPrefix, 0) == 0 || label.rfind(kTaskPrefix, 0) == 0;
}

// ---- services ----

pc_result wait_for_state(SC_HANDLE service, DWORD state, int seconds) {
    for (int i = 0; i < seconds * 4; ++i) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status),
                                  &needed))
            return last_error_result();
        if (status.dwCurrentState == state) return PC_OK;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return PC_ERR_FAILED;
}

pc_result start_service(SC_HANDLE manager, const std::wstring &name) {
    ScHandle service(OpenServiceW(manager, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) return last_error_result();
    if (StartServiceW(service.h, 0, nullptr)) return wait_for_state(service.h, SERVICE_RUNNING, 15);
    const DWORD error = GetLastError();
    if (error == ERROR_SERVICE_ALREADY_RUNNING) return PC_OK;
    if (error == ERROR_SERVICE_DISABLED) return PC_ERR_FAILED;
    return error_result(error);
}

pc_result stop_service(SC_HANDLE manager, const std::wstring &name) {
    ScHandle service(OpenServiceW(manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) return last_error_result();
    SERVICE_STATUS status{};
    if (ControlService(service.h, SERVICE_CONTROL_STOP, &status)) return wait_for_state(service.h, SERVICE_STOPPED, 15);
    const DWORD error = GetLastError();
    if (error == ERROR_SERVICE_NOT_ACTIVE) return PC_OK;
    return error_result(error);
}

// The service's configuration; false when it can't be read. `buffer` holds the strings it points to.
bool query_config(SC_HANDLE service, std::vector<BYTE> &buffer, const QUERY_SERVICE_CONFIGW *&config) {
    config = nullptr;
    if (buffer.size() < 8192) buffer.resize(8192);
    DWORD size = 0;
    if (!QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data()),
                             static_cast<DWORD>(buffer.size()), &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
        buffer.resize(size);
        if (!QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data()),
                                 static_cast<DWORD>(buffer.size()), &size))
            return false;
    }
    config = reinterpret_cast<const QUERY_SERVICE_CONFIGW *>(buffer.data());
    return true;
}

// Disabling a service forgets how it used to start (Automatic, Automatic (Delayed), Manual).
// launchctl enable restores the job's own definition; the SCM has no such memory, so the start
// type is kept here (per user, like the rest of the settings) and Enable puts it back.
const wchar_t *kStartTypesKey = L"Software\\Procyon\\ServiceStartTypes";

void remember_start_type(const std::wstring &name, DWORD start_type) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kStartTypesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&start_type), sizeof(start_type));
    RegCloseKey(key);
}

DWORD remembered_start_type(const std::wstring &name) {
    const uint32_t type = registry_dword(HKEY_CURRENT_USER, kStartTypesKey, name.c_str(), SERVICE_AUTO_START);
    return type == SERVICE_DEMAND_START || type == SERVICE_AUTO_START ? type : SERVICE_AUTO_START;
}

pc_result set_start_type(SC_HANDLE manager, const std::wstring &name, DWORD start_type) {
    ScHandle service(OpenServiceW(manager, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG));
    if (!service) return last_error_result();
    std::vector<BYTE> buffer;
    const QUERY_SERVICE_CONFIGW *config = nullptr;
    const bool known = query_config(service.h, buffer, config);
    if (start_type == SERVICE_DISABLED) {
        if (known && config->dwStartType != SERVICE_DISABLED) remember_start_type(name, config->dwStartType);
    } else if (known && config->dwStartType != SERVICE_DISABLED) {
        return PC_OK;  // already enabled: keep its start type as it is
    } else {
        start_type = remembered_start_type(name);
    }
    return ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, start_type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, nullptr)
               ? PC_OK
               : last_error_result();
}

}  // namespace

std::vector<pc_service> services() {
    std::vector<pc_service> result;
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!manager) return result;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed,
                          &count, &resume, nullptr);
    if (GetLastError() != ERROR_MORE_DATA || needed == 0) return result;
    std::vector<BYTE> buffer(needed + 4096);
    resume = 0;
    if (!EnumServicesStatusExW(manager.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(),
                               static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr))
        return result;
    auto entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW *>(buffer.data());
    result.reserve(count);
    std::vector<BYTE> config(8192);
    for (DWORD i = 0; i < count; ++i) {
        const ENUM_SERVICE_STATUS_PROCESSW &entry = entries[i];
        pc_service service{};
        const std::string name = utf8(entry.lpServiceName, wcslen(entry.lpServiceName));
        copy_string(service.label, sizeof(service.label), name);
        copy_string(service.name, sizeof(service.name), utf8(entry.lpDisplayName, wcslen(entry.lpDisplayName)));
        copy_string(service.config_path, sizeof(service.config_path),
                    "HKLM\\SYSTEM\\CurrentControlSet\\Services\\" + name);
        // Per-user services carry the session's id as a suffix ("CDPUserSvc_4a1b2").
        service.domain =
            (entry.ServiceStatusProcess.dwServiceType & SERVICE_USER_SERVICE) ? PC_DOMAIN_USER : PC_DOMAIN_SYSTEM;
        service.pid = entry.ServiceStatusProcess.dwCurrentState == SERVICE_STOPPED
                          ? 0
                          : static_cast<int32_t>(entry.ServiceStatusProcess.dwProcessId);
        // A service that has never run reports ERROR_SERVICE_NEVER_STARTED: not a failure.
        const DWORD exit_code = entry.ServiceStatusProcess.dwWin32ExitCode;
        service.last_exit = exit_code == ERROR_SERVICE_SPECIFIC_ERROR
                                ? static_cast<int32_t>(entry.ServiceStatusProcess.dwServiceSpecificExitCode)
                            : exit_code == ERROR_SERVICE_NEVER_STARTED ? 0
                                                                       : static_cast<int32_t>(exit_code);
        service.enabled = true;

        ScHandle handle(OpenServiceW(manager.h, entry.lpServiceName, SERVICE_QUERY_CONFIG));
        const QUERY_SERVICE_CONFIGW *cfg = nullptr;
        if (handle && query_config(handle.h, config, cfg)) {
            service.enabled = cfg->dwStartType != SERVICE_DISABLED;
            if (cfg->lpBinaryPathName) {
                const std::string command = utf8(cfg->lpBinaryPathName, wcslen(cfg->lpBinaryPathName));
                const std::string executable = executable_of_command(command);
                copy_string(service.program, sizeof(service.program), executable.empty() ? command : executable);
                service.apple = under_windows_directory(executable);
            }
        }
        result.push_back(service);
    }
    return result;
}

std::vector<pc_startup_item> startup_items() {
    std::vector<pc_startup_item> result;
    for (const RunSource &source : kRunSources) run_key_items(source, result);
    folder_items(known_folder(FOLDERID_Startup), HKEY_CURRENT_USER, "user", PC_STARTUP_USER_AGENT, result);
    folder_items(known_folder(FOLDERID_CommonStartup), HKEY_LOCAL_MACHINE, "common", PC_STARTUP_GLOBAL_AGENT, result);
    task_items(result);
    return result;
}

// Windows keeps no separate administrator-only list: everything is in startup_items.
std::vector<pc_startup_item> managed_startup_items(uint32_t) { return {}; }

bool valid_service_label(const std::string &label) {
    // A service name (up to 256 characters, no path separators) or one of our startup labels.
    if (label.empty() || label.size() >= 256 || label[0] == '-' || label[0] == '/') return false;
    for (unsigned char c : label)
        if (c < 0x20 || c == '\\' || c == '/') return false;
    return true;
}

pc_result service_control(int32_t domain, const std::string &label, int32_t action) {
    if (!valid_service_label(label)) return PC_ERR_INVALID;
    (void)domain;
    if (is_startup_label(label)) {
        if (action != PC_SERVICE_ENABLE && action != PC_SERVICE_DISABLE) return PC_ERR_UNSUPPORTED;
        if (label.rfind(kTaskPrefix, 0) == 0) {
            const std::string path = task_path_of_label(label);
            if (path.size() < 2 || path[0] != '\\') return PC_ERR_INVALID;
            return set_task_enabled(path, action == PC_SERVICE_ENABLE);
        }
        HKEY root = nullptr;
        const wchar_t *key = nullptr;
        std::wstring name;
        if (!parse_startup_label(label, root, key, name)) return PC_ERR_INVALID;
        return set_approved(root, key, name, action == PC_SERVICE_ENABLE);
    }
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) return last_error_result();
    const std::wstring name = wide(label);
    switch (action) {
        case PC_SERVICE_START: return start_service(manager.h, name);
        case PC_SERVICE_STOP: return stop_service(manager.h, name);
        case PC_SERVICE_RESTART: {
            const pc_result stopped = stop_service(manager.h, name);
            if (stopped != PC_OK) return stopped;
            return start_service(manager.h, name);
        }
        case PC_SERVICE_ENABLE: return set_start_type(manager.h, name, SERVICE_AUTO_START);
        case PC_SERVICE_DISABLE: return set_start_type(manager.h, name, SERVICE_DISABLED);
        default: return PC_ERR_INVALID;
    }
}

// The launchctl parsers are macOS-only; they exist here so the shared declarations link.
LoadedServices parse_launchctl_print(const std::string &) { return {}; }
std::unordered_map<std::string, bool> parse_launchctl_disabled(const std::string &) { return {}; }
std::vector<pc_startup_item> parse_managed_startup_items(std::string, uint32_t, const LoadedServices &,
                                                         const LoadedServices &) {
    return {};
}

}  // namespace procyon::platform

#endif  // _WIN32
