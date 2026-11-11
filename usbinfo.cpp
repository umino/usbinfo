// usbinfo - USB デバイス / ストレージ情報の調査用ダンプツール (Windows 10 以降)
//
// 取得元:
//   - Configuration Manager (CM_*)      : devnode の全プロパティ (DEVPKEY)、親子関係
//   - ストレージ IOCTL                  : IOCTL_STORAGE_QUERY_PROPERTY / HOTPLUG_INFO / DEVICE_NUMBER ほか
//   - ボリューム API                    : GetDriveType / ボリューム → ディスク対応
//   - USB ハブ IOCTL                    : デバイス/コンフィグ/文字列ディスクリプタ、ポート属性
//   - WMI                               : Win32_DiskDrive / MSFT_Disk / MSFT_PhysicalDisk
//
// 管理者権限は不要 (デバイスはアクセス権 0 でオープンし、FILE_ANY_ACCESS の IOCTL のみ使用)。

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define NOMINMAX
#include <windows.h>
#include <initguid.h>   // 以降のヘッダーの GUID / DEVPROPKEY を実体化する
#include <devpropdef.h>
#include <devpkey.h>
#include <winioctl.h>   // DEFINE_DEVPROPKEY 定義後に取り込むと DEVPKEY_Storage_* も定義される
#include <ntddscsi.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// 汎用ヘルパー
// ---------------------------------------------------------------------------

static std::wstring fmt(const wchar_t* f, ...)
{
    wchar_t b[4096];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, ap);
    va_end(ap);
    return b;
}

static std::wstring trim(std::wstring s)
{
    const wchar_t* ws = L" \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static std::wstring winErr(DWORD e)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = fmt(L"Win32 error %lu", e);
    if (msg) { s += L" (" + trim(msg) + L")"; LocalFree(msg); }
    return s;
}

static std::wstring hexDump(const BYTE* p, size_t n, size_t limit = 512)
{
    std::wstring s;
    size_t m = std::min(n, limit);
    s.reserve(m * 3 + 16);
    for (size_t i = 0; i < m; ++i) { if (i) s += L' '; s += fmt(L"%02X", p[i]); }
    if (n > limit) s += fmt(L" ... (%zu bytes total)", n);
    return s;
}

static std::wstring guidStr(const GUID& g)
{
    wchar_t b[64] = L"";
    StringFromGUID2(g, b, _countof(b));
    return b;
}

static std::wstring fileTimeStr(const FILETIME& ft)
{
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return L"(invalid FILETIME)";
    return fmt(L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static std::wstring a2w(const char* s, size_t maxLen)
{
    std::wstring w;
    for (size_t i = 0; i < maxLen && s[i]; ++i) w += (wchar_t)(unsigned char)s[i];
    return w;
}

static std::string toUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool startsWithI(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

static std::vector<std::wstring> splitMultiSz(const wchar_t* p, size_t maxChars)
{
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < maxChars && p[i]) {
        size_t len = wcsnlen(p + i, maxChars - i);
        v.emplace_back(p + i, len);
        i += len + 1;
    }
    return v;
}

static std::wstring join(const std::vector<std::wstring>& v, const wchar_t* sep)
{
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
    return s;
}

// ---------------------------------------------------------------------------
// 出力ツリー (テキスト / JSON / CSV 共通の中間表現)
// ---------------------------------------------------------------------------

struct Node {
    enum Kind { Str, Raw, Obj, Arr };
    Kind kind = Obj;
    std::wstring key, val;
    std::list<Node> kids;   // list なので子への参照は追加後も有効

    Node& push(Kind k, const std::wstring& name, const std::wstring& value = L"")
    {
        kids.emplace_back();
        Node& n = kids.back();
        n.kind = k; n.key = name; n.val = value;
        return n;
    }
    Node& str(const std::wstring& name, const std::wstring& value) { return push(Str, name, value); }
    Node& num(const std::wstring& name, unsigned long long v) { return push(Raw, name, fmt(L"%llu", v)); }
    Node& boolean(const std::wstring& name, bool v) { return push(Raw, name, v ? L"true" : L"false"); }
    Node& hex(const std::wstring& name, unsigned long long v, int digits) { return push(Str, name, fmt(L"0x%0*llX", digits, v)); }
    Node& obj(const std::wstring& name) { return push(Obj, name); }
    Node& arr(const std::wstring& name) { return push(Arr, name); }
    Node& item() { return push(Obj, L""); }
    void err(const std::wstring& what, DWORD e) { str(what + L"_error", winErr(e)); }

    const Node* find(const std::wstring& name) const
    {
        for (const Node& k : kids) if (k.key == name) return &k;
        return nullptr;
    }
};

static std::wstring oneLine(std::wstring s)
{
    for (wchar_t& c : s) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return s;
}

static void emitText(const Node& n, std::wstring& o, int ind, bool inArr, size_t idx)
{
    std::wstring pad((size_t)ind * 2, L' ');
    if (n.kind == Node::Str || n.kind == Node::Raw) {
        o += pad + (inArr ? L"- " : n.key + L": ") + oneLine(n.val) + L"\n";
        return;
    }
    std::wstring head = inArr ? fmt(L"[%zu]", idx) : n.key;
    if (n.kids.empty()) { o += pad + head + (n.kind == Node::Arr ? L": []\n" : L": {}\n"); return; }
    o += pad + head + L":\n";
    size_t i = 0;
    for (const Node& k : n.kids) emitText(k, o, ind + 1, n.kind == Node::Arr, i++);
}

static std::wstring jsonEsc(const std::wstring& s)
{
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) o += fmt(L"\\u%04x", (unsigned)c);
            else o += c;
        }
    }
    return o + L"\"";
}

static void emitJson(const Node& n, std::wstring& o, int ind)
{
    std::wstring pad((size_t)ind * 2, L' ');
    if (n.kind == Node::Str) { o += jsonEsc(n.val); return; }
    if (n.kind == Node::Raw) { o += n.val; return; }
    bool isArr = n.kind == Node::Arr;
    if (n.kids.empty()) { o += isArr ? L"[]" : L"{}"; return; }
    o += isArr ? L"[\n" : L"{\n";
    size_t i = 0;
    for (const Node& k : n.kids) {
        o += pad + L"  ";
        if (!isArr) o += jsonEsc(k.key) + L": ";
        emitJson(k, o, ind + 1);
        if (++i < n.kids.size()) o += L",";
        o += L"\n";
    }
    o += pad + (isArr ? L"]" : L"}");
}

static std::wstring csvEsc(const std::wstring& s)
{
    std::wstring o = L"\"";
    for (wchar_t c : oneLine(s)) { if (c == L'"') o += L'"'; o += c; }
    return o + L"\"";
}

static void emitCsv(const Node& table, std::wstring& o)
{
    bool header = true;
    for (const Node& row : table.kids) {
        if (header) {
            size_t i = 0;
            for (const Node& c : row.kids) { if (i++) o += L","; o += csvEsc(c.key); }
            o += L"\r\n";
            header = false;
        }
        size_t i = 0;
        for (const Node& c : row.kids) { if (i++) o += L","; o += csvEsc(c.val); }
        o += L"\r\n";
    }
}

// ---------------------------------------------------------------------------
// 名前テーブル
// ---------------------------------------------------------------------------

struct KeyName { const DEVPROPKEY* key; const wchar_t* name; };
#define K(x) { &x, L#x }
static const KeyName g_keyNames[] = {
    K(DEVPKEY_NAME),
    K(DEVPKEY_Device_DeviceDesc), K(DEVPKEY_Device_HardwareIds), K(DEVPKEY_Device_CompatibleIds),
    K(DEVPKEY_Device_Service), K(DEVPKEY_Device_Class), K(DEVPKEY_Device_ClassGuid), K(DEVPKEY_Device_Driver),
    K(DEVPKEY_Device_ConfigFlags), K(DEVPKEY_Device_Manufacturer), K(DEVPKEY_Device_FriendlyName),
    K(DEVPKEY_Device_LocationInfo), K(DEVPKEY_Device_PDOName), K(DEVPKEY_Device_Capabilities),
    K(DEVPKEY_Device_UINumber), K(DEVPKEY_Device_UpperFilters), K(DEVPKEY_Device_LowerFilters),
    K(DEVPKEY_Device_BusTypeGuid), K(DEVPKEY_Device_LegacyBusType), K(DEVPKEY_Device_BusNumber),
    K(DEVPKEY_Device_EnumeratorName), K(DEVPKEY_Device_Security), K(DEVPKEY_Device_SecuritySDS),
    K(DEVPKEY_Device_DevType), K(DEVPKEY_Device_Exclusive), K(DEVPKEY_Device_Characteristics),
    K(DEVPKEY_Device_Address), K(DEVPKEY_Device_UINumberDescFormat), K(DEVPKEY_Device_PowerData),
    K(DEVPKEY_Device_RemovalPolicy), K(DEVPKEY_Device_RemovalPolicyDefault), K(DEVPKEY_Device_RemovalPolicyOverride),
    K(DEVPKEY_Device_InstallState), K(DEVPKEY_Device_LocationPaths), K(DEVPKEY_Device_BaseContainerId),
    K(DEVPKEY_Device_InstanceId), K(DEVPKEY_Device_DevNodeStatus), K(DEVPKEY_Device_ProblemCode),
    K(DEVPKEY_Device_EjectionRelations), K(DEVPKEY_Device_RemovalRelations), K(DEVPKEY_Device_PowerRelations),
    K(DEVPKEY_Device_BusRelations), K(DEVPKEY_Device_Parent), K(DEVPKEY_Device_Children),
    K(DEVPKEY_Device_Siblings), K(DEVPKEY_Device_TransportRelations), K(DEVPKEY_Device_ProblemStatus),
    K(DEVPKEY_Device_Reported), K(DEVPKEY_Device_Legacy), K(DEVPKEY_Device_ContainerId),
    K(DEVPKEY_Device_InLocalMachineContainer), K(DEVPKEY_Device_Model), K(DEVPKEY_Device_ModelId),
    K(DEVPKEY_Device_FriendlyNameAttributes), K(DEVPKEY_Device_ManufacturerAttributes),
    K(DEVPKEY_Device_PresenceNotForDevice), K(DEVPKEY_Device_SignalStrength),
    K(DEVPKEY_Device_IsAssociateableByUserAction), K(DEVPKEY_Device_ShowInUninstallUI),
    K(DEVPKEY_Device_Numa_Proximity_Domain), K(DEVPKEY_Device_DHP_Rebalance_Policy), K(DEVPKEY_Device_Numa_Node),
    K(DEVPKEY_Device_BusReportedDeviceDesc), K(DEVPKEY_Device_IsPresent), K(DEVPKEY_Device_HasProblem),
    K(DEVPKEY_Device_ConfigurationId), K(DEVPKEY_Device_ReportedDeviceIdsHash),
    K(DEVPKEY_Device_PhysicalDeviceLocation), K(DEVPKEY_Device_BiosDeviceName), K(DEVPKEY_Device_DriverProblemDesc),
    K(DEVPKEY_Device_DebuggerSafe), K(DEVPKEY_Device_PostInstallInProgress), K(DEVPKEY_Device_Stack),
    K(DEVPKEY_Device_ExtendedConfigurationIds), K(DEVPKEY_Device_IsRebootRequired),
    K(DEVPKEY_Device_FirmwareDate), K(DEVPKEY_Device_FirmwareVersion), K(DEVPKEY_Device_FirmwareRevision),
    K(DEVPKEY_Device_DependencyProviders), K(DEVPKEY_Device_DependencyDependents),
    K(DEVPKEY_Device_SoftRestartSupported), K(DEVPKEY_Device_ExtendedAddress), K(DEVPKEY_Device_SessionId),
    K(DEVPKEY_Device_InstallDate), K(DEVPKEY_Device_FirstInstallDate), K(DEVPKEY_Device_LastArrivalDate),
    K(DEVPKEY_Device_LastRemovalDate), K(DEVPKEY_Device_DriverDate), K(DEVPKEY_Device_DriverVersion),
    K(DEVPKEY_Device_DriverDesc), K(DEVPKEY_Device_DriverInfPath), K(DEVPKEY_Device_DriverInfSection),
    K(DEVPKEY_Device_DriverInfSectionExt), K(DEVPKEY_Device_MatchingDeviceId), K(DEVPKEY_Device_DriverProvider),
    K(DEVPKEY_Device_DriverPropPageProvider), K(DEVPKEY_Device_DriverCoInstallers),
    K(DEVPKEY_Device_ResourcePickerTags), K(DEVPKEY_Device_ResourcePickerExceptions),
    K(DEVPKEY_Device_DriverRank), K(DEVPKEY_Device_DriverLogoLevel), K(DEVPKEY_Device_NoConnectSound),
    K(DEVPKEY_Device_GenericDriverInstalled), K(DEVPKEY_Device_AdditionalSoftwareRequested),
    K(DEVPKEY_Device_SafeRemovalRequired), K(DEVPKEY_Device_SafeRemovalRequiredOverride),
    K(DEVPKEY_Storage_Portable), K(DEVPKEY_Storage_Removable_Media), K(DEVPKEY_Storage_System_Critical),
    K(DEVPKEY_Storage_Disk_Number), K(DEVPKEY_Storage_Partition_Number),
};
#undef K

static std::wstring keyName(const DEVPROPKEY& k)
{
    for (const KeyName& kn : g_keyNames)
        if (kn.key->pid == k.pid && IsEqualGUID(kn.key->fmtid, k.fmtid)) return kn.name;
    return fmt(L"%s[%lu]", guidStr(k.fmtid).c_str(), k.pid);
}

static const wchar_t* busTypeName(DWORD b)
{
    static const wchar_t* names[] = {
        L"Unknown", L"Scsi", L"Atapi", L"Ata", L"1394", L"Ssa", L"Fibre", L"Usb", L"RAID", L"iScsi", L"Sas",
        L"Sata", L"Sd", L"Mmc", L"Virtual", L"FileBackedVirtual", L"Spaces", L"Nvme", L"SCM", L"Ufs" };
    return b < _countof(names) ? names[b] : L"(other)";
}

static const wchar_t* driveTypeName(UINT t)
{
    switch (t) {
    case DRIVE_UNKNOWN: return L"DRIVE_UNKNOWN";
    case DRIVE_NO_ROOT_DIR: return L"DRIVE_NO_ROOT_DIR";
    case DRIVE_REMOVABLE: return L"DRIVE_REMOVABLE";
    case DRIVE_FIXED: return L"DRIVE_FIXED";
    case DRIVE_REMOTE: return L"DRIVE_REMOTE";
    case DRIVE_CDROM: return L"DRIVE_CDROM";
    case DRIVE_RAMDISK: return L"DRIVE_RAMDISK";
    }
    return L"(other)";
}

static const wchar_t* removalPolicyName(DWORD p)
{
    switch (p) {
    case CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL: return L"ExpectNoRemoval";
    case CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL: return L"ExpectOrderlyRemoval";
    case CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL: return L"ExpectSurpriseRemoval";
    }
    return L"(unknown)";
}

static std::wstring capsStr(DWORD c)
{
    static const struct { DWORD bit; const wchar_t* name; } flags[] = {
        { CM_DEVCAP_LOCKSUPPORTED, L"LockSupported" }, { CM_DEVCAP_EJECTSUPPORTED, L"EjectSupported" },
        { CM_DEVCAP_REMOVABLE, L"Removable" }, { CM_DEVCAP_DOCKDEVICE, L"DockDevice" },
        { CM_DEVCAP_UNIQUEID, L"UniqueID" }, { CM_DEVCAP_SILENTINSTALL, L"SilentInstall" },
        { CM_DEVCAP_RAWDEVICEOK, L"RawDeviceOK" }, { CM_DEVCAP_SURPRISEREMOVALOK, L"SurpriseRemovalOK" },
        { CM_DEVCAP_HARDWAREDISABLED, L"HardwareDisabled" }, { CM_DEVCAP_NONDYNAMIC, L"NonDynamic" },
        { CM_DEVCAP_SECUREDEVICE, L"SecureDevice" } };
    std::wstring s = fmt(L"0x%08lX", c);
    std::wstring names;
    for (auto& f : flags) if (c & f.bit) { if (!names.empty()) names += L"|"; names += f.name; }
    return s + L" (" + names + L")";
}

static const wchar_t* usbClassName(BYTE c)
{
    switch (c) {
    case 0x00: return L"(per-interface)";
    case 0x01: return L"Audio";
    case 0x02: return L"CDC";
    case 0x03: return L"HID";
    case 0x05: return L"Physical";
    case 0x06: return L"Image(PTP)";
    case 0x07: return L"Printer";
    case 0x08: return L"MassStorage";
    case 0x09: return L"Hub";
    case 0x0A: return L"CDC-Data";
    case 0x0B: return L"SmartCard";
    case 0x0D: return L"ContentSecurity";
    case 0x0E: return L"Video";
    case 0x0F: return L"PersonalHealthcare";
    case 0x10: return L"AudioVideo";
    case 0x11: return L"Billboard";
    case 0xDC: return L"Diagnostic";
    case 0xE0: return L"Wireless";
    case 0xEF: return L"Miscellaneous";
    case 0xFE: return L"ApplicationSpecific";
    case 0xFF: return L"VendorSpecific";
    }
    return L"(unknown)";
}

static std::wstring massStorageDetail(BYTE sub, BYTE proto)
{
    const wchar_t* s = L"?";
    switch (sub) {
    case 0x00: s = L"SCSI-not-reported"; break;
    case 0x01: s = L"RBC"; break;
    case 0x02: s = L"ATAPI(MMC-5)"; break;
    case 0x04: s = L"UFI"; break;
    case 0x06: s = L"SCSI-transparent"; break;
    case 0xFF: s = L"VendorSpecific"; break;
    }
    const wchar_t* p = L"?";
    switch (proto) {
    case 0x00: p = L"CBI(with-interrupt)"; break;
    case 0x01: p = L"CBI"; break;
    case 0x50: p = L"BOT(Bulk-Only)"; break;
    case 0x62: p = L"UAS"; break;
    case 0xFF: p = L"VendorSpecific"; break;
    }
    return std::wstring(s) + L"/" + p;
}

// ---------------------------------------------------------------------------
// Configuration Manager ヘルパー
// ---------------------------------------------------------------------------

static std::wstring devInstId(DEVINST di)
{
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = L"";
    if (CM_Get_Device_IDW(di, buf, _countof(buf), 0) != CR_SUCCESS) return L"";
    return buf;
}

static std::vector<std::wstring> listInterfaces(const GUID& g, const wchar_t* devId = nullptr)
{
    for (int retry = 0; retry < 3; ++retry) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<LPGUID>(&g), const_cast<DEVINSTID_W>(devId),
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) break;
        std::vector<wchar_t> buf((size_t)len + 1);
        CONFIGRET cr = CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&g), const_cast<DEVINSTID_W>(devId),
                                                     buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL) continue;
        if (cr != CR_SUCCESS) break;
        return splitMultiSz(buf.data(), len);
    }
    return {};
}

static std::wstring interfaceInstanceId(const std::wstring& ifPath)
{
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = L"";
    ULONG size = sizeof(buf);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(ifPath.c_str(), &DEVPKEY_Device_InstanceId, &type, (PBYTE)buf, &size, 0) != CR_SUCCESS)
        return L"";
    return buf;
}

static bool getPropRaw(DEVINST di, const DEVPROPKEY& k, DEVPROPTYPE& t, std::vector<BYTE>& buf)
{
    ULONG sz = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(di, &k, &t, nullptr, &sz, 0);
    if (cr == CR_SUCCESS) { buf.clear(); return true; }
    if (cr != CR_BUFFER_SMALL) return false;
    buf.assign(sz, 0);
    return CM_Get_DevNode_PropertyW(di, &k, &t, buf.data(), &sz, 0) == CR_SUCCESS;
}

static bool getStrProp(DEVINST di, const DEVPROPKEY& k, std::wstring& out)
{
    DEVPROPTYPE t; std::vector<BYTE> b;
    if (!getPropRaw(di, k, t, b) || t != DEVPROP_TYPE_STRING || b.size() < sizeof(wchar_t)) return false;
    out.assign((const wchar_t*)b.data(), wcsnlen((const wchar_t*)b.data(), b.size() / sizeof(wchar_t)));
    return true;
}

static bool getStrListProp(DEVINST di, const DEVPROPKEY& k, std::vector<std::wstring>& out)
{
    DEVPROPTYPE t; std::vector<BYTE> b;
    if (!getPropRaw(di, k, t, b) || t != DEVPROP_TYPE_STRING_LIST) return false;
    out = splitMultiSz((const wchar_t*)b.data(), b.size() / sizeof(wchar_t));
    return true;
}

static bool getU32Prop(DEVINST di, const DEVPROPKEY& k, DWORD& out)
{
    DEVPROPTYPE t; std::vector<BYTE> b;
    if (!getPropRaw(di, k, t, b) || b.size() < 4 || (t != DEVPROP_TYPE_UINT32 && t != DEVPROP_TYPE_INT32)) return false;
    memcpy(&out, b.data(), 4);
    return true;
}

static bool getBoolProp(DEVINST di, const DEVPROPKEY& k, bool& out)
{
    DEVPROPTYPE t; std::vector<BYTE> b;
    if (!getPropRaw(di, k, t, b) || t != DEVPROP_TYPE_BOOLEAN || b.empty()) return false;
    out = b[0] != 0;
    return true;
}

static bool getGuidProp(DEVINST di, const DEVPROPKEY& k, GUID& out)
{
    DEVPROPTYPE t; std::vector<BYTE> b;
    if (!getPropRaw(di, k, t, b) || t != DEVPROP_TYPE_GUID || b.size() < sizeof(GUID)) return false;
    memcpy(&out, b.data(), sizeof(GUID));
    return true;
}

static void addProp(Node& o, const std::wstring& name, DEVPROPTYPE t, const std::vector<BYTE>& b)
{
    const BYTE* p = b.data();
    size_t n = b.size();
    auto wstr = [&]() { return std::wstring((const wchar_t*)p, wcsnlen((const wchar_t*)p, n / sizeof(wchar_t))); };
    switch (t) {
    case DEVPROP_TYPE_STRING:
    case DEVPROP_TYPE_STRING_INDIRECT:
    case DEVPROP_TYPE_SECURITY_DESCRIPTOR_STRING:
        o.str(name, wstr());
        return;
    case DEVPROP_TYPE_STRING_LIST: {
        Node& a = o.arr(name);
        for (auto& s : splitMultiSz((const wchar_t*)p, n / sizeof(wchar_t))) a.str(L"", s);
        return;
    }
    case DEVPROP_TYPE_BOOLEAN:
        if (n >= 1) { o.boolean(name, p[0] != 0); return; }
        break;
    case DEVPROP_TYPE_BYTE:
    case DEVPROP_TYPE_SBYTE:
        if (n >= 1) { o.str(name, fmt(L"0x%02X (%u)", p[0], p[0])); return; }
        break;
    case DEVPROP_TYPE_UINT16:
    case DEVPROP_TYPE_INT16:
        if (n >= 2) { USHORT v; memcpy(&v, p, 2); o.str(name, fmt(L"0x%04X (%u)", v, v)); return; }
        break;
    case DEVPROP_TYPE_UINT32:
    case DEVPROP_TYPE_INT32:
    case DEVPROP_TYPE_ERROR:
    case DEVPROP_TYPE_NTSTATUS:
        if (n >= 4) { DWORD v; memcpy(&v, p, 4); o.str(name, fmt(L"0x%08lX (%lu)", v, v)); return; }
        break;
    case DEVPROP_TYPE_UINT64:
    case DEVPROP_TYPE_INT64:
        if (n >= 8) { ULONGLONG v; memcpy(&v, p, 8); o.str(name, fmt(L"0x%016llX (%llu)", v, v)); return; }
        break;
    case DEVPROP_TYPE_GUID:
        if (n >= sizeof(GUID)) { GUID g; memcpy(&g, p, sizeof(g)); o.str(name, guidStr(g)); return; }
        break;
    case DEVPROP_TYPE_FILETIME:
        if (n >= sizeof(FILETIME)) { FILETIME ft; memcpy(&ft, p, sizeof(ft)); o.str(name, fileTimeStr(ft)); return; }
        break;
    case DEVPROP_TYPE_EMPTY:
    case DEVPROP_TYPE_NULL:
        o.str(name, L"(empty)");
        return;
    }
    o.str(name, fmt(L"(type 0x%lX, %zu bytes) ", t, n) + hexDump(p, n, 128));
}

// devnode の全プロパティをダンプ (キー名でソートして比較しやすくする)
static void dumpAllProps(DEVINST di, Node& out)
{
    ULONG count = 0;
    CONFIGRET cr = CM_Get_DevNode_Property_Keys(di, nullptr, &count, 0);
    if (cr != CR_BUFFER_SMALL && cr != CR_SUCCESS) { out.str(L"error", fmt(L"CM_Get_DevNode_Property_Keys CONFIGRET %lu", cr)); return; }
    std::vector<DEVPROPKEY> keys(count);
    if (count && CM_Get_DevNode_Property_Keys(di, keys.data(), &count, 0) != CR_SUCCESS) return;
    keys.resize(count);

    std::vector<std::pair<std::wstring, DEVPROPKEY>> named;
    for (auto& k : keys) named.emplace_back(keyName(k), k);
    std::sort(named.begin(), named.end(), [](auto& a, auto& b) {
        bool ka = a.first[0] == L'D', kb = b.first[0] == L'D';   // 既知の DEVPKEY_* を先に
        return ka != kb ? ka : a.first < b.first;
    });
    for (auto& nk : named) {
        DEVPROPTYPE t = 0; std::vector<BYTE> b;
        if (getPropRaw(di, nk.second, t, b)) addProp(out, nk.first, t, b);
    }
}

// 判定に効く主要項目だけを抜き出した要約
static void briefNode(DEVINST di, Node& o)
{
    o.str(L"instanceId", devInstId(di));
    std::wstring s; DWORD d; bool b; GUID g; std::vector<std::wstring> l;
    if (getStrProp(di, DEVPKEY_NAME, s)) o.str(L"name", s);
    if (getStrProp(di, DEVPKEY_Device_DeviceDesc, s)) o.str(L"deviceDesc", s);
    if (getStrProp(di, DEVPKEY_Device_BusReportedDeviceDesc, s)) o.str(L"busReportedDeviceDesc", s);
    if (getStrProp(di, DEVPKEY_Device_Manufacturer, s)) o.str(L"manufacturer", s);
    if (getStrProp(di, DEVPKEY_Device_Class, s)) o.str(L"class", s);
    if (getStrProp(di, DEVPKEY_Device_Service, s)) o.str(L"service", s);
    if (getStrProp(di, DEVPKEY_Device_EnumeratorName, s)) o.str(L"enumerator", s);
    if (getStrProp(di, DEVPKEY_Device_LocationInfo, s)) o.str(L"locationInfo", s);
    if (getStrListProp(di, DEVPKEY_Device_LocationPaths, l)) o.str(L"locationPaths", join(l, L" | "));
    if (getU32Prop(di, DEVPKEY_Device_Address, d)) o.num(L"address", d);
    if (getU32Prop(di, DEVPKEY_Device_Capabilities, d)) {
        o.str(L"capabilities", capsStr(d));
        o.boolean(L"capRemovable", (d & CM_DEVCAP_REMOVABLE) != 0);
        o.boolean(L"capSurpriseRemovalOK", (d & CM_DEVCAP_SURPRISEREMOVALOK) != 0);
        o.boolean(L"capUniqueID", (d & CM_DEVCAP_UNIQUEID) != 0);
    }
    if (getU32Prop(di, DEVPKEY_Device_RemovalPolicy, d)) o.str(L"removalPolicy", fmt(L"%lu (%s)", d, removalPolicyName(d)));
    if (getGuidProp(di, DEVPKEY_Device_ContainerId, g)) o.str(L"containerId", guidStr(g));
    if (getBoolProp(di, DEVPKEY_Device_InLocalMachineContainer, b)) o.boolean(L"inLocalMachineContainer", b);
    if (getBoolProp(di, DEVPKEY_Device_SafeRemovalRequired, b)) o.boolean(L"safeRemovalRequired", b);
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, di, 0) == CR_SUCCESS) {
        o.hex(L"devNodeStatus", status, 8);
        o.boolean(L"statusRemovable(DN_REMOVABLE)", (status & DN_REMOVABLE) != 0);
        if (problem) o.num(L"problemCode", problem);
    }
}

static bool isUsbHub(DEVINST di)
{
    std::wstring id = devInstId(di);
    return !id.empty() && !listInterfaces(GUID_DEVINTERFACE_USB_HUB, id.c_str()).empty();
}

// ---------------------------------------------------------------------------
// USB (ハブ IOCTL 経由)
// ---------------------------------------------------------------------------

struct UsbBrief {
    bool valid = false;
    std::wstring instanceId, vidPid, serial, manufacturer, product, speed, interfaces, bcdUSB, bcdDevice;
    std::wstring hubId, removalPolicy, containerId, service;
    ULONG port = 0;
    int capRemovable = -1, capUniqueId = -1, inLocalMachineContainer = -1;
    int portUserConnectable = -1, portTypeC = -1, hubNonRemovableBit = -1;
};

static std::wstring tri(int v) { return v < 0 ? L"(n/a)" : (v ? L"true" : L"false"); }

static bool usbGetDescriptor(HANDLE hub, ULONG port, BYTE type, BYTE index, USHORT lang, USHORT len,
                             std::vector<BYTE>& out, DWORD* err = nullptr)
{
    const size_t hdr = offsetof(USB_DESCRIPTOR_REQUEST, Data);
    std::vector<BYTE> buf(hdr + len, 0);
    auto* r = (PUSB_DESCRIPTOR_REQUEST)buf.data();
    r->ConnectionIndex = port;
    r->SetupPacket.wValue = (USHORT)((type << 8) | index);
    r->SetupPacket.wIndex = lang;
    r->SetupPacket.wLength = len;
    DWORD ret = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf.data(), (DWORD)buf.size(),
                         buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
        if (err) *err = GetLastError();
        return false;
    }
    if (ret < hdr + 2) { if (err) *err = ERROR_INVALID_DATA; return false; }
    out.assign(buf.begin() + hdr, buf.begin() + ret);
    return true;
}

static std::wstring usbGetString(HANDLE hub, ULONG port, BYTE index, USHORT lang)
{
    if (!index) return L"";
    std::vector<BYTE> d;
    if (!usbGetDescriptor(hub, port, USB_STRING_DESCRIPTOR_TYPE, index, lang, 255, d)) return L"(read failed)";
    if (d.size() < 2 || d[1] != USB_STRING_DESCRIPTOR_TYPE) return L"(invalid)";
    size_t bytes = std::min<size_t>(d[0], d.size());
    if (bytes < 2) return L"";
    return std::wstring((const wchar_t*)(d.data() + 2), (bytes - 2) / 2);
}

static void parseConfigDescriptor(const std::vector<BYTE>& cfg, HANDLE hub, ULONG port, USHORT lang,
                                  Node& out, std::wstring& ifaceSummary)
{
    Node& list = out.arr(L"descriptors");
    size_t pos = 0;
    while (pos + 2 <= cfg.size()) {
        BYTE len = cfg[pos], type = cfg[pos + 1];
        if (len < 2 || pos + len > cfg.size()) break;
        const BYTE* d = cfg.data() + pos;
        Node& e = list.item();
        switch (type) {
        case USB_CONFIGURATION_DESCRIPTOR_TYPE:
            e.str(L"type", L"CONFIGURATION");
            if (len >= 9) {
                e.num(L"wTotalLength", d[2] | (d[3] << 8));
                e.num(L"bNumInterfaces", d[4]);
                e.num(L"bConfigurationValue", d[5]);
                e.hex(L"bmAttributes", d[7], 2);
                e.boolean(L"selfPowered", (d[7] & 0x40) != 0);
                e.boolean(L"remoteWakeup", (d[7] & 0x20) != 0);
                e.str(L"bMaxPower", fmt(L"%u (x2mA or x8mA for SS)", d[8]));
                if (d[6]) e.str(L"iConfiguration", usbGetString(hub, port, d[6], lang));
            }
            break;
        case USB_INTERFACE_DESCRIPTOR_TYPE:
            e.str(L"type", L"INTERFACE");
            if (len >= 9) {
                e.num(L"bInterfaceNumber", d[2]);
                e.num(L"bAlternateSetting", d[3]);
                e.num(L"bNumEndpoints", d[4]);
                e.hex(L"bInterfaceClass", d[5], 2);
                e.hex(L"bInterfaceSubClass", d[6], 2);
                e.hex(L"bInterfaceProtocol", d[7], 2);
                std::wstring desc = usbClassName(d[5]);
                if (d[5] == 0x08) desc += L" " + massStorageDetail(d[6], d[7]);
                e.str(L"classDescription", desc);
                if (d[8]) e.str(L"iInterface", usbGetString(hub, port, d[8], lang));
                if (!ifaceSummary.empty()) ifaceSummary += L"; ";
                ifaceSummary += fmt(L"if%u.alt%u=%02X/%02X/%02X %s", d[2], d[3], d[5], d[6], d[7], desc.c_str());
            }
            break;
        case USB_ENDPOINT_DESCRIPTOR_TYPE:
            e.str(L"type", L"ENDPOINT");
            if (len >= 7) {
                static const wchar_t* xfer[] = { L"Control", L"Isochronous", L"Bulk", L"Interrupt" };
                e.str(L"bEndpointAddress", fmt(L"0x%02X (%s)", d[2], (d[2] & 0x80) ? L"IN" : L"OUT"));
                e.str(L"bmAttributes", fmt(L"0x%02X (%s)", d[3], xfer[d[3] & 3]));
                e.num(L"wMaxPacketSize", d[4] | (d[5] << 8));
                e.num(L"bInterval", d[6]);
            }
            break;
        default:
            e.str(L"type", fmt(L"0x%02X", type));
            break;
        }
        e.str(L"raw", hexDump(d, len));
        pos += len;
    }
}

// dev: ハブの直下にぶら下がる USB デバイスの devnode
static void usbInfo(DEVINST dev, Node& out, UsbBrief& brief)
{
    brief.instanceId = devInstId(dev);
    DWORD d; bool b; GUID g; std::wstring s;
    if (getU32Prop(dev, DEVPKEY_Device_Capabilities, d)) {
        brief.capRemovable = (d & CM_DEVCAP_REMOVABLE) ? 1 : 0;
        brief.capUniqueId = (d & CM_DEVCAP_UNIQUEID) ? 1 : 0;
    }
    if (getU32Prop(dev, DEVPKEY_Device_RemovalPolicy, d)) brief.removalPolicy = removalPolicyName(d);
    if (getBoolProp(dev, DEVPKEY_Device_InLocalMachineContainer, b)) brief.inLocalMachineContainer = b ? 1 : 0;
    if (getGuidProp(dev, DEVPKEY_Device_ContainerId, g)) brief.containerId = guidStr(g);
    if (getStrProp(dev, DEVPKEY_Device_Service, s)) brief.service = s;

    DEVINST hubInst = 0;
    if (CM_Get_Parent(&hubInst, dev, 0) != CR_SUCCESS) { out.str(L"error", L"no parent devnode"); return; }
    brief.hubId = devInstId(hubInst);
    out.str(L"hubInstanceId", brief.hubId);
    auto hubPaths = listInterfaces(GUID_DEVINTERFACE_USB_HUB, brief.hubId.c_str());
    if (hubPaths.empty()) { out.str(L"error", L"parent is not a USB hub"); return; }
    out.str(L"hubInterfacePath", hubPaths[0]);

    DWORD port = 0;
    if (!getU32Prop(dev, DEVPKEY_Device_Address, port) || port == 0) { out.str(L"error", L"port number (DEVPKEY_Device_Address) unavailable"); return; }
    brief.port = port;
    out.num(L"portNumber", port);

    HANDLE hub = CreateFileW(hubPaths[0].c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hub == INVALID_HANDLE_VALUE) { out.err(L"openHub", GetLastError()); return; }
    DWORD ret = 0;

    // --- ハブ自体の情報 ---
    {
        Node& h = out.obj(L"hub");
        USB_HUB_INFORMATION_EX hi = {};
        if (DeviceIoControl(hub, IOCTL_USB_GET_HUB_INFORMATION_EX, &hi, sizeof(hi), &hi, sizeof(hi), &ret, nullptr)) {
            static const wchar_t* types[] = { L"?", L"UsbRootHub", L"Usb20Hub", L"Usb30Hub" };
            h.str(L"hubType", hi.HubType < _countof(types) ? types[hi.HubType] : L"?");
            h.num(L"highestPortNumber", hi.HighestPortNumber);
            // ハブディスクリプタの DeviceRemovable ビット (1 = そのポートのデバイスは取り外し不可)
            if (hi.HubType == Usb30Hub) {
                if (port < 16) brief.hubNonRemovableBit = (hi.u.Usb30HubDescriptor.DeviceRemovable >> port) & 1;
                h.hex(L"usb30.DeviceRemovable", hi.u.Usb30HubDescriptor.DeviceRemovable, 4);
            } else {
                if (port < 64) brief.hubNonRemovableBit = (hi.u.UsbHubDescriptor.bRemoveAndPowerMask[port / 8] >> (port % 8)) & 1;
                h.str(L"usb20.DeviceRemovable(first 4 bytes)", hexDump(hi.u.UsbHubDescriptor.bRemoveAndPowerMask, 4));
            }
            h.str(L"deviceRemovableBitForThisPort(1=non-removable)", tri(brief.hubNonRemovableBit));
        } else h.err(L"IOCTL_USB_GET_HUB_INFORMATION_EX", GetLastError());
    }

    // --- ポートのコネクタ属性 ---
    {
        Node& p = out.obj(L"portConnectorProperties");
        BYTE buf[sizeof(USB_PORT_CONNECTOR_PROPERTIES) + 512] = {};
        auto* pc = (PUSB_PORT_CONNECTOR_PROPERTIES)buf;
        pc->ConnectionIndex = port;
        if (DeviceIoControl(hub, IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES, pc, sizeof(buf), pc, sizeof(buf), &ret, nullptr)) {
            brief.portUserConnectable = pc->UsbPortProperties.PortIsUserConnectable ? 1 : 0;
            brief.portTypeC = pc->UsbPortProperties.PortConnectorIsTypeC ? 1 : 0;
            p.hex(L"raw", pc->UsbPortProperties.ul, 8);
            p.boolean(L"PortIsUserConnectable", pc->UsbPortProperties.PortIsUserConnectable);
            p.boolean(L"PortIsDebugCapable", pc->UsbPortProperties.PortIsDebugCapable);
            p.boolean(L"PortHasMultipleCompanions", pc->UsbPortProperties.PortHasMultipleCompanions);
            p.boolean(L"PortConnectorIsTypeC", pc->UsbPortProperties.PortConnectorIsTypeC);
            p.num(L"CompanionPortNumber", pc->CompanionPortNumber);
        } else p.err(L"IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES", GetLastError());
    }

    // --- 接続情報 + デバイスディスクリプタ ---
    BYTE ciBuf[sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + sizeof(USB_PIPE_INFO) * 32] = {};
    auto* ci = (PUSB_NODE_CONNECTION_INFORMATION_EX)ciBuf;
    ci->ConnectionIndex = port;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, ci, sizeof(ciBuf), ci, sizeof(ciBuf), &ret, nullptr)) {
        out.err(L"IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX", GetLastError());
        CloseHandle(hub);
        return;
    }
    brief.valid = true;
    static const wchar_t* speeds[] = { L"Low", L"Full", L"High", L"Super" };
    std::wstring speed = ci->Speed < _countof(speeds) ? speeds[ci->Speed] : L"?";
    {
        Node& c = out.obj(L"connection");
        static const wchar_t* st[] = { L"NoDeviceConnected", L"DeviceConnected", L"DeviceFailedEnumeration", L"DeviceGeneralFailure",
            L"DeviceCausedOvercurrent", L"DeviceNotEnoughPower", L"DeviceNotEnoughBandwidth", L"DeviceHubNestedTooDeeply",
            L"DeviceInLegacyHub", L"DeviceEnumerating", L"DeviceReset" };
        c.str(L"connectionStatus", (unsigned)ci->ConnectionStatus < _countof(st) ? st[ci->ConnectionStatus] : L"?");
        c.num(L"deviceAddress", ci->DeviceAddress);
        c.num(L"currentConfigurationValue", ci->CurrentConfigurationValue);
        c.boolean(L"deviceIsHub", ci->DeviceIsHub != 0);
        c.num(L"numberOfOpenPipes", ci->NumberOfOpenPipes);

        USB_NODE_CONNECTION_INFORMATION_EX_V2 v2 = {};
        v2.ConnectionIndex = port;
        v2.Length = sizeof(v2);
        v2.SupportedUsbProtocols.Usb300 = 1;
        if (DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &v2, sizeof(v2), &v2, sizeof(v2), &ret, nullptr)) {
            c.boolean(L"portSupportsUsb110", v2.SupportedUsbProtocols.Usb110);
            c.boolean(L"portSupportsUsb200", v2.SupportedUsbProtocols.Usb200);
            c.boolean(L"portSupportsUsb300", v2.SupportedUsbProtocols.Usb300);
            c.boolean(L"deviceIsOperatingAtSuperSpeedOrHigher", v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher);
            c.boolean(L"deviceIsSuperSpeedCapableOrHigher", v2.Flags.DeviceIsSuperSpeedCapableOrHigher);
            c.boolean(L"deviceIsOperatingAtSuperSpeedPlusOrHigher", v2.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher);
            c.boolean(L"deviceIsSuperSpeedPlusCapableOrHigher", v2.Flags.DeviceIsSuperSpeedPlusCapableOrHigher);
            if (v2.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher) speed = L"SuperPlus";
            else if (v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) speed = L"Super";
        } else c.err(L"IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2", GetLastError());
        c.str(L"speed", speed);

        BYTE dk[sizeof(USB_NODE_CONNECTION_DRIVERKEY_NAME) + 1024] = {};
        auto* dkn = (PUSB_NODE_CONNECTION_DRIVERKEY_NAME)dk;
        dkn->ConnectionIndex = port;
        if (DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, dkn, sizeof(dk), dkn, sizeof(dk), &ret, nullptr)) {
            c.str(L"driverKeyName", dkn->DriverKeyName);
            // ポート番号の取り違えがないことの確認 (devnode 側の Driver キーと一致するはず)
            if (getStrProp(dev, DEVPKEY_Device_Driver, s)) c.boolean(L"driverKeyMatchesDevnode", _wcsicmp(s.c_str(), dkn->DriverKeyName) == 0);
        }
    }
    brief.speed = speed;

    const USB_DEVICE_DESCRIPTOR& dd = ci->DeviceDescriptor;
    USHORT lang = 0x0409;
    {
        std::vector<BYTE> l;
        if (usbGetDescriptor(hub, port, USB_STRING_DESCRIPTOR_TYPE, 0, 0, 255, l) && l.size() >= 4) lang = (USHORT)(l[2] | (l[3] << 8));
    }
    {
        Node& n = out.obj(L"deviceDescriptor");
        n.hex(L"bcdUSB", dd.bcdUSB, 4);
        n.str(L"bDeviceClass", fmt(L"0x%02X %s", dd.bDeviceClass, usbClassName(dd.bDeviceClass)));
        n.hex(L"bDeviceSubClass", dd.bDeviceSubClass, 2);
        n.hex(L"bDeviceProtocol", dd.bDeviceProtocol, 2);
        n.num(L"bMaxPacketSize0", dd.bMaxPacketSize0);
        n.hex(L"idVendor", dd.idVendor, 4);
        n.hex(L"idProduct", dd.idProduct, 4);
        n.hex(L"bcdDevice", dd.bcdDevice, 4);
        n.num(L"iManufacturer", dd.iManufacturer);
        n.num(L"iProduct", dd.iProduct);
        n.num(L"iSerialNumber", dd.iSerialNumber);
        n.num(L"bNumConfigurations", dd.bNumConfigurations);
        n.hex(L"stringLangId", lang, 4);
        brief.manufacturer = usbGetString(hub, port, dd.iManufacturer, lang);
        brief.product = usbGetString(hub, port, dd.iProduct, lang);
        brief.serial = dd.iSerialNumber ? usbGetString(hub, port, dd.iSerialNumber, lang) : L"(none)";
        n.str(L"manufacturerString", brief.manufacturer);
        n.str(L"productString", brief.product);
        n.str(L"serialNumberString", brief.serial);
        brief.vidPid = fmt(L"%04X:%04X", dd.idVendor, dd.idProduct);
        brief.bcdUSB = fmt(L"%04X", dd.bcdUSB);
        brief.bcdDevice = fmt(L"%04X", dd.bcdDevice);
    }

    // --- コンフィグレーションディスクリプタ ---
    {
        Node& n = out.obj(L"configurationDescriptor");
        std::vector<BYTE> head, cfg;
        DWORD e = 0;
        if (usbGetDescriptor(hub, port, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, 9, head, &e) && head.size() >= 4) {
            USHORT total = (USHORT)(head[2] | (head[3] << 8));
            if (usbGetDescriptor(hub, port, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, total, cfg, &e))
                parseConfigDescriptor(cfg, hub, port, lang, n, brief.interfaces);
            else n.err(L"getConfigDescriptor", e);
        } else n.err(L"getConfigDescriptor", e);
    }

    // --- BOS ディスクリプタ (USB 2.01 以降) ---
    if (dd.bcdUSB > 0x0200) {
        std::vector<BYTE> head, bos;
        if (usbGetDescriptor(hub, port, 0x0F, 0, 0, 5, head) && head.size() >= 4) {
            USHORT total = (USHORT)(head[2] | (head[3] << 8));
            if (usbGetDescriptor(hub, port, 0x0F, 0, 0, total, bos)) out.str(L"bosDescriptorRaw", hexDump(bos.data(), bos.size()));
        }
    }
    CloseHandle(hub);
}

static void addUsbBrief(Node& o, const UsbBrief& u)
{
    o.str(L"usbDeviceInstanceId", u.instanceId);
    o.str(L"vidPid", u.vidPid);
    o.str(L"bcdDevice", u.bcdDevice);
    o.str(L"bcdUSB", u.bcdUSB);
    o.str(L"manufacturerString", u.manufacturer);
    o.str(L"productString", u.product);
    o.str(L"serialNumberString", u.serial);
    o.str(L"speed", u.speed);
    o.str(L"interfaces", u.interfaces);
    o.str(L"service", u.service);
    o.str(L"hubInstanceId", u.hubId);
    o.num(L"portNumber", u.port);
    o.str(L"capRemovable", tri(u.capRemovable));
    o.str(L"capUniqueID", tri(u.capUniqueId));
    o.str(L"removalPolicy", u.removalPolicy);
    o.str(L"containerId", u.containerId);
    o.str(L"inLocalMachineContainer", tri(u.inLocalMachineContainer));
    o.str(L"portIsUserConnectable", tri(u.portUserConnectable));
    o.str(L"portConnectorIsTypeC", tri(u.portTypeC));
    o.str(L"hubDeviceRemovableBit(1=non-removable)", tri(u.hubNonRemovableBit));
}

static void dumpDescendants(DEVINST parent, Node& arr, int depth)
{
    if (depth > 8) return;
    DEVINST child = 0;
    if (CM_Get_Child(&child, parent, 0) != CR_SUCCESS) return;
    do {
        Node& n = arr.item();
        std::wstring s;
        n.str(L"instanceId", devInstId(child));
        if (getStrProp(child, DEVPKEY_NAME, s)) n.str(L"name", s);
        if (getStrProp(child, DEVPKEY_Device_Class, s)) n.str(L"class", s);
        if (getStrProp(child, DEVPKEY_Device_Service, s)) n.str(L"service", s);
        Node tmp;
        dumpDescendants(child, tmp, depth + 1);
        if (!tmp.kids.empty()) { Node& c = n.arr(L"children"); c.kids = std::move(tmp.kids); }
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
}

// ---------------------------------------------------------------------------
// ボリューム
// ---------------------------------------------------------------------------

struct VolumeInfo {
    std::wstring guidPath, dosDevice, fsName, label;
    std::vector<std::wstring> mounts;
    UINT driveType = DRIVE_UNKNOWN;
    bool hasDevNum = false;
    STORAGE_DEVICE_NUMBER devNum = {};
    std::vector<DISK_EXTENT> extents;
    DWORD serial = 0, fsFlags = 0, volInfoErr = 0;
    ULARGE_INTEGER total = {}, freeBytes = {};
    bool hasSpace = false;
};

static std::vector<VolumeInfo> collectVolumes()
{
    std::vector<VolumeInfo> vols;
    wchar_t name[MAX_PATH] = L"";
    HANDLE f = FindFirstVolumeW(name, _countof(name));
    if (f == INVALID_HANDLE_VALUE) return vols;
    do {
        VolumeInfo v;
        v.guidPath = name;                       // "\\?\Volume{GUID}\"
        v.driveType = GetDriveTypeW(name);

        std::vector<wchar_t> mp(4096);
        DWORD need = 0;
        if (GetVolumePathNamesForVolumeNameW(name, mp.data(), (DWORD)mp.size(), &need))
            v.mounts = splitMultiSz(mp.data(), mp.size());

        std::wstring noSlash = v.guidPath.substr(0, v.guidPath.size() - 1);
        wchar_t dos[512] = L"";
        if (noSlash.size() > 4 && QueryDosDeviceW(noSlash.c_str() + 4, dos, _countof(dos))) v.dosDevice = dos;

        HANDLE h = CreateFileW(noSlash.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD ret = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &v.devNum, sizeof(v.devNum), &ret, nullptr)) v.hasDevNum = true;
            BYTE eb[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 63];
            if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, eb, sizeof(eb), &ret, nullptr)) {
                auto* ve = (PVOLUME_DISK_EXTENTS)eb;
                for (DWORD i = 0; i < ve->NumberOfDiskExtents; ++i) v.extents.push_back(ve->Extents[i]);
            }
            CloseHandle(h);
        }

        wchar_t label[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
        DWORD maxComp = 0;
        if (GetVolumeInformationW(name, label, _countof(label), &v.serial, &maxComp, &v.fsFlags, fs, _countof(fs))) {
            v.label = label; v.fsName = fs;
            ULARGE_INTEGER avail;
            if (GetDiskFreeSpaceExW(name, &avail, &v.total, &v.freeBytes)) v.hasSpace = true;
        } else v.volInfoErr = GetLastError();
        vols.push_back(std::move(v));
    } while (FindNextVolumeW(f, name, _countof(name)));
    FindVolumeClose(f);
    return vols;
}

// ---------------------------------------------------------------------------
// WMI
// ---------------------------------------------------------------------------

typedef std::vector<std::pair<std::wstring, std::wstring>> WmiObject;

static std::wstring variantToStr(VARIANT& v)
{
    if (v.vt == VT_NULL || v.vt == VT_EMPTY) return L"";
    if (v.vt & VT_ARRAY) {
        SAFEARRAY* sa = v.parray;
        VARTYPE et = (VARTYPE)(v.vt & VT_TYPEMASK);
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        std::wstring s;
        for (LONG i = lb; i <= ub; ++i) {
            if (i > lb) s += L", ";
            if (et == VT_BSTR) {
                BSTR b = nullptr;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &b)) && b) { s += b; SysFreeString(b); }
            } else {
                LONGLONG raw = 0;
                if (FAILED(SafeArrayGetElement(sa, &i, &raw))) continue;
                switch (et) {
                case VT_UI1: case VT_I1: s += fmt(L"%u", (unsigned)(raw & 0xFF)); break;
                case VT_I2: case VT_UI2: case VT_BOOL: s += fmt(L"%u", (unsigned)(raw & 0xFFFF)); break;
                default: s += fmt(L"%lu", (unsigned long)(raw & 0xFFFFFFFF)); break;
                }
            }
        }
        return s;
    }
    VARIANT d;
    VariantInit(&d);
    std::wstring s;
    if (SUCCEEDED(VariantChangeType(&d, &v, VARIANT_ALPHABOOL, VT_BSTR)) && d.bstrVal) s = d.bstrVal;
    VariantClear(&d);
    return s;
}

static std::vector<WmiObject> wmiQuery(const wchar_t* ns, const wchar_t* wql, std::wstring& err)
{
    std::vector<WmiObject> result;
    IWbemLocator* loc = nullptr;
    IWbemServices* svc = nullptr;
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc);
    if (SUCCEEDED(hr)) {
        BSTR b = SysAllocString(ns);
        hr = loc->ConnectServer(b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
        SysFreeString(b);
    }
    if (SUCCEEDED(hr)) {
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(wql);
        hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(q);
    }
    while (SUCCEEDED(hr) && en) {
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        hr = en->Next(WBEM_INFINITE, 1, &obj, &got);
        if (FAILED(hr) || got == 0) break;
        WmiObject o;
        if (SUCCEEDED(obj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY))) {
            BSTR name = nullptr;
            VARIANT v;
            VariantInit(&v);
            while (obj->Next(0, &name, &v, nullptr, nullptr) == WBEM_S_NO_ERROR) {
                if (v.vt != VT_NULL && v.vt != VT_EMPTY) o.emplace_back(name, variantToStr(v));
                SysFreeString(name);
                VariantClear(&v);
            }
            obj->EndEnumeration();
        }
        std::sort(o.begin(), o.end());
        result.push_back(std::move(o));
        obj->Release();
    }
    if (FAILED(hr)) err = fmt(L"HRESULT 0x%08lX", (unsigned long)hr);
    if (en) en->Release();
    if (svc) svc->Release();
    if (loc) loc->Release();
    return result;
}

static void addWmiMatch(Node& out, const wchar_t* name, const std::vector<WmiObject>& objs, const wchar_t* keyProp, const std::wstring& keyVal)
{
    for (auto& o : objs) {
        bool match = false;
        for (auto& kv : o) if (kv.first == keyProp && kv.second == keyVal) { match = true; break; }
        if (!match) continue;
        Node& n = out.obj(name);
        for (auto& kv : o) n.str(kv.first, kv.second);
        return;
    }
}

// ---------------------------------------------------------------------------
// ディスク (GUID_DEVINTERFACE_DISK / CDROM)
// ---------------------------------------------------------------------------

static bool queryStorageProp(HANDLE h, STORAGE_PROPERTY_ID id, std::vector<BYTE>& buf, DWORD* err)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = id;
    q.QueryType = PropertyStandardQuery;
    buf.assign(8192, 0);
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
        if (err) *err = GetLastError();
        return false;
    }
    buf.resize(std::max<DWORD>(ret, 64));   // 構造体の固定部を安全に読めるよう最低長を確保
    return true;
}

struct DiskFacts {
    bool opened = false;
    bool hasDevNum = false; DWORD deviceType = 0, deviceNumber = 0;
    bool hasDesc = false; DWORD busType = 0; bool removableMedia = false;
    std::wstring vendor, product, revision, serial;
    int mediaHotplug = -1, deviceHotplug = -1;
    int mediaPresent = -1;
    bool hasSize = false; ULONGLONG size = 0; std::wstring geometryMediaType;
};

static void dumpStorage(HANDLE h, Node& out, DiskFacts& f)
{
    DWORD ret = 0, e = 0;
    std::vector<BYTE> buf;

    STORAGE_DEVICE_NUMBER dn = {};
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof(dn), &ret, nullptr)) {
        f.hasDevNum = true; f.deviceType = dn.DeviceType; f.deviceNumber = dn.DeviceNumber;
        Node& n = out.obj(L"deviceNumber");
        n.str(L"DeviceType", fmt(L"0x%02lX (%s)", dn.DeviceType,
              dn.DeviceType == FILE_DEVICE_DISK ? L"FILE_DEVICE_DISK" : dn.DeviceType == FILE_DEVICE_CD_ROM ? L"FILE_DEVICE_CD_ROM" : L"other"));
        n.num(L"DeviceNumber", dn.DeviceNumber);
    } else out.err(L"IOCTL_STORAGE_GET_DEVICE_NUMBER", GetLastError());

    if (queryStorageProp(h, StorageDeviceProperty, buf, &e)) {
        auto* d = (PSTORAGE_DEVICE_DESCRIPTOR)buf.data();
        auto s = [&](DWORD off) { return (off && off < buf.size()) ? trim(a2w((const char*)buf.data() + off, buf.size() - off)) : std::wstring(); };
        f.hasDesc = true; f.busType = d->BusType; f.removableMedia = d->RemovableMedia != 0;
        f.vendor = s(d->VendorIdOffset); f.product = s(d->ProductIdOffset);
        f.revision = s(d->ProductRevisionOffset); f.serial = s(d->SerialNumberOffset);
        Node& n = out.obj(L"deviceDescriptor(StorageDeviceProperty)");
        n.str(L"BusType", fmt(L"%lu (%s)", (DWORD)d->BusType, busTypeName(d->BusType)));
        n.boolean(L"RemovableMedia", d->RemovableMedia != 0);
        n.boolean(L"CommandQueueing", d->CommandQueueing != 0);
        n.hex(L"DeviceType(SCSI)", d->DeviceType, 2);
        n.hex(L"DeviceTypeModifier", d->DeviceTypeModifier, 2);
        n.str(L"VendorId", f.vendor);
        n.str(L"ProductId", f.product);
        n.str(L"ProductRevision", f.revision);
        n.str(L"SerialNumber", f.serial);
    } else out.err(L"StorageDeviceProperty", e);

    if (queryStorageProp(h, StorageAdapterProperty, buf, &e)) {
        auto* a = (PSTORAGE_ADAPTER_DESCRIPTOR)buf.data();
        Node& n = out.obj(L"adapterDescriptor(StorageAdapterProperty)");
        n.str(L"BusType", fmt(L"%u (%s)", a->BusType, busTypeName(a->BusType)));
        n.num(L"MaximumTransferLength", a->MaximumTransferLength);
        n.num(L"MaximumPhysicalPages", a->MaximumPhysicalPages);
        n.hex(L"AlignmentMask", a->AlignmentMask, 8);
        n.boolean(L"AdapterUsesPio", a->AdapterUsesPio != 0);
        n.boolean(L"CommandQueueing", a->CommandQueueing != 0);
        n.boolean(L"AcceleratedTransfer", a->AcceleratedTransfer != 0);
        n.str(L"BusVersion", fmt(L"%u.%u", a->BusMajorVersion, a->BusMinorVersion));
    } else out.err(L"StorageAdapterProperty", e);

    STORAGE_HOTPLUG_INFO hp = {};
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0, &hp, sizeof(hp), &ret, nullptr)) {
        f.mediaHotplug = hp.MediaHotplug ? 1 : 0; f.deviceHotplug = hp.DeviceHotplug ? 1 : 0;
        Node& n = out.obj(L"hotplugInfo");
        n.boolean(L"MediaRemovable", hp.MediaRemovable != 0);
        n.boolean(L"MediaHotplug", hp.MediaHotplug != 0);
        n.boolean(L"DeviceHotplug", hp.DeviceHotplug != 0);
        n.boolean(L"WriteCacheEnableOverride", hp.WriteCacheEnableOverride != 0);
    } else out.err(L"IOCTL_STORAGE_GET_HOTPLUG_INFO", GetLastError());

    if (DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0, &ret, nullptr)) { f.mediaPresent = 1; out.boolean(L"mediaPresent", true); }
    else { e = GetLastError(); f.mediaPresent = 0; out.boolean(L"mediaPresent", false); out.err(L"IOCTL_STORAGE_CHECK_VERIFY2", e); }

    BYTE gb[sizeof(DISK_GEOMETRY_EX) + 256] = {};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, gb, sizeof(gb), &ret, nullptr)) {
        auto* g = (PDISK_GEOMETRY_EX)gb;
        f.hasSize = true; f.size = (ULONGLONG)g->DiskSize.QuadPart;
        DWORD mt = g->Geometry.MediaType;
        f.geometryMediaType = mt == FixedMedia ? L"FixedMedia" : mt == RemovableMedia ? L"RemovableMedia" : fmt(L"%lu", mt);
        Node& n = out.obj(L"geometry");
        n.str(L"MediaType", fmt(L"%lu (%s)", mt, f.geometryMediaType.c_str()));
        n.num(L"DiskSize", f.size);
        n.num(L"BytesPerSector", g->Geometry.BytesPerSector);
    } else out.err(L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX", GetLastError());

    if (queryStorageProp(h, StorageAccessAlignmentProperty, buf, &e)) {
        auto* a = (PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)buf.data();
        Node& n = out.obj(L"accessAlignment");
        n.num(L"BytesPerLogicalSector", a->BytesPerLogicalSector);
        n.num(L"BytesPerPhysicalSector", a->BytesPerPhysicalSector);
    }
    if (queryStorageProp(h, StorageDeviceSeekPenaltyProperty, buf, &e))
        out.boolean(L"incursSeekPenalty", ((PDEVICE_SEEK_PENALTY_DESCRIPTOR)buf.data())->IncursSeekPenalty != 0);
    if (queryStorageProp(h, StorageDeviceTrimProperty, buf, &e))
        out.boolean(L"trimEnabled", ((PDEVICE_TRIM_DESCRIPTOR)buf.data())->TrimEnabled != 0);
    if (queryStorageProp(h, StorageDeviceIdProperty, buf, &e)) {
        auto* d = (PSTORAGE_DEVICE_ID_DESCRIPTOR)buf.data();
        out.num(L"deviceIdDescriptor.NumberOfIdentifiers", d->NumberOfIdentifiers);
        out.str(L"deviceIdDescriptor.raw", hexDump(buf.data(), std::min<size_t>(d->Size, buf.size()), 256));
    }

    SCSI_ADDRESS sa = {};
    if (DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, nullptr, 0, &sa, sizeof(sa), &ret, nullptr))
        out.str(L"scsiAddress", fmt(L"Port %u, Path %u, Target %u, Lun %u", sa.PortNumber, sa.PathId, sa.TargetId, sa.Lun));

    if (DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &ret, nullptr)) out.boolean(L"writable", true);
    else { e = GetLastError(); if (e == ERROR_WRITE_PROTECT) out.boolean(L"writable", false); }

    std::vector<BYTE> lb(64 * 1024);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, lb.data(), (DWORD)lb.size(), &ret, nullptr)) {
        auto* l = (PDRIVE_LAYOUT_INFORMATION_EX)lb.data();
        Node& n = out.obj(L"driveLayout");
        static const wchar_t* styles[] = { L"MBR", L"GPT", L"RAW" };
        n.str(L"PartitionStyle", l->PartitionStyle < 3 ? styles[l->PartitionStyle] : L"?");
        if (l->PartitionStyle == PARTITION_STYLE_MBR) n.hex(L"mbrSignature", l->Mbr.Signature, 8);
        if (l->PartitionStyle == PARTITION_STYLE_GPT) n.str(L"gptDiskId", guidStr(l->Gpt.DiskId));
        Node& pa = n.arr(L"partitions");
        for (DWORD i = 0; i < l->PartitionCount; ++i) {
            const PARTITION_INFORMATION_EX& p = l->PartitionEntry[i];
            if (p.PartitionLength.QuadPart == 0) continue;
            Node& pn = pa.item();
            pn.num(L"number", p.PartitionNumber);
            pn.num(L"offset", (ULONGLONG)p.StartingOffset.QuadPart);
            pn.num(L"length", (ULONGLONG)p.PartitionLength.QuadPart);
            if (p.PartitionStyle == PARTITION_STYLE_MBR) {
                pn.hex(L"mbrType", p.Mbr.PartitionType, 2);
                pn.boolean(L"bootIndicator", p.Mbr.BootIndicator != 0);
            } else if (p.PartitionStyle == PARTITION_STYLE_GPT) {
                pn.str(L"gptType", guidStr(p.Gpt.PartitionType));
                pn.str(L"gptName", std::wstring(p.Gpt.Name, wcsnlen(p.Gpt.Name, _countof(p.Gpt.Name))));
            }
        }
    } else out.err(L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX", GetLastError());
}

struct Options {
    std::wstring format = L"text";
    std::wstring outFile;
    bool wmi = true;
    bool allProps = true;
};

struct WmiData {
    std::vector<WmiObject> diskDrive, msftDisk, msftPhysicalDisk;
};

static void processDisk(const std::wstring& ifPath, bool isCdrom, const Options& opt, const std::vector<VolumeInfo>& vols,
                        const WmiData& wmi, std::map<std::wstring, UsbBrief>& usbCache, Node& disks, Node& summary)
{
    Node& d = disks.item();
    d.str(L"kind", isCdrom ? L"CdRom" : L"Disk");
    d.str(L"interfacePath", ifPath);
    std::wstring instId = interfaceInstanceId(ifPath);
    d.str(L"instanceId", instId);

    // --- ストレージ IOCTL ---
    DiskFacts f;
    {
        Node& st = d.obj(L"storage");
        HANDLE h = CreateFileW(ifPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) st.err(L"open", GetLastError());
        else { f.opened = true; dumpStorage(h, st, f); CloseHandle(h); }
    }

    // --- ボリューム ---
    std::wstring mountsSummary, driveTypesSummary;
    {
        Node& va = d.arr(L"volumes");
        for (auto& v : vols) {
            bool match = false;
            if (f.hasDevNum) {
                if (v.hasDevNum && v.devNum.DeviceType == f.deviceType && v.devNum.DeviceNumber == f.deviceNumber) match = true;
                if (!isCdrom) for (auto& e : v.extents) if (e.DiskNumber == f.deviceNumber) match = true;
            }
            if (!match) continue;
            Node& n = va.item();
            n.str(L"volumeGuidPath", v.guidPath);
            n.str(L"dosDevice", v.dosDevice);
            n.str(L"mountPoints", join(v.mounts, L" "));
            n.str(L"GetDriveType", fmt(L"%u (%s)", v.driveType, driveTypeName(v.driveType)));
            if (v.hasDevNum) n.num(L"partitionNumber", v.devNum.PartitionNumber);
            if (v.volInfoErr) n.err(L"GetVolumeInformation", v.volInfoErr);
            else {
                n.str(L"fileSystem", v.fsName);
                n.str(L"label", v.label);
                n.hex(L"volumeSerial", v.serial, 8);
                n.hex(L"fileSystemFlags", v.fsFlags, 8);
                if (v.hasSpace) { n.num(L"totalBytes", v.total.QuadPart); n.num(L"freeBytes", v.freeBytes.QuadPart); }
            }
            if (!v.mounts.empty()) {
                if (!mountsSummary.empty()) { mountsSummary += L" "; driveTypesSummary += L" "; }
                mountsSummary += join(v.mounts, L" ");
                driveTypesSummary += driveTypeName(v.driveType) + 6;   // "DRIVE_" を省く
            }
        }
    }

    // --- devnode と祖先 ---
    DEVINST di = 0;
    std::vector<DEVINST> chain;
    if (!instId.empty() && CM_Locate_DevNodeW(&di, const_cast<DEVINSTID_W>(instId.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        chain.push_back(di);
        DEVINST p = 0, cur = di;
        while (CM_Get_Parent(&p, cur, 0) == CR_SUCCESS) { chain.push_back(p); cur = p; }
    }

    // ハブ直下のノード = USB デバイス本体 (複合デバイスの MI_xx ノードはその下)
    int usbIdx = -1;
    for (size_t i = 0; i + 1 < chain.size(); ++i)
        if (isUsbHub(chain[i + 1])) { usbIdx = (int)i; break; }

    DWORD diskRemovalPolicy = 0;
    bool hasRemovalPolicy = false;
    bool diskSafeRemoval = false, hasSafeRemoval = false;
    if (!chain.empty()) {
        Node& dn = d.obj(L"devnode");
        briefNode(di, dn);
        hasRemovalPolicy = getU32Prop(di, DEVPKEY_Device_RemovalPolicy, diskRemovalPolicy);
        hasSafeRemoval = getBoolProp(di, DEVPKEY_Device_SafeRemovalRequired, diskSafeRemoval);
        if (opt.allProps) dumpAllProps(di, dn.obj(L"allProperties"));

        Node& an = d.arr(L"ancestors");
        // 全プロパティは USB デバイス本体の手前まで (非 USB は直近の親のみ)。USB デバイス本体は usbDevices 側に出す。
        size_t fullUntil = usbIdx >= 0 ? (size_t)usbIdx : 2;
        for (size_t i = 1; i < chain.size(); ++i) {
            Node& n = an.item();
            briefNode(chain[i], n);
            if ((int)i == usbIdx) n.str(L"note", L"USB device node (see usbDevices for full dump)");
            if (opt.allProps && i < fullUntil) dumpAllProps(chain[i], n.obj(L"allProperties"));
        }
    }

    // --- USB 情報 ---
    UsbBrief usb;
    std::wstring storageDriver;
    if (usbIdx >= 0) {
        std::wstring usbId = devInstId(chain[usbIdx]);
        auto it = usbCache.find(usbId);
        if (it != usbCache.end()) usb = it->second;
        else { Node tmp; usbInfo(chain[usbIdx], tmp, usb); usbCache[usbId] = usb; }
        addUsbBrief(d.obj(L"usb"), usb);
        // ディスクに最も近い USB 列挙ノードのサービス = 実際に使われているストレージドライバ (USBSTOR / UASPStor など)
        for (int i = 1; i <= usbIdx; ++i)
            if (startsWithI(devInstId(chain[i]), L"USB\\")) { getStrProp(chain[i], DEVPKEY_Device_Service, storageDriver); break; }
        d.str(L"usbStorageDriverService", storageDriver);
    }

    // --- WMI ---
    if (opt.wmi && f.hasDevNum && !isCdrom) {
        Node& w = d.obj(L"wmi");
        std::wstring num = fmt(L"%lu", f.deviceNumber);
        addWmiMatch(w, L"Win32_DiskDrive", wmi.diskDrive, L"Index", num);
        addWmiMatch(w, L"MSFT_Disk", wmi.msftDisk, L"Number", num);
        addWmiMatch(w, L"MSFT_PhysicalDisk", wmi.msftPhysicalDisk, L"DeviceId", num);
    }

    // --- 判定 ---
    bool viaUsb = usbIdx >= 0;
    bool busUsb = f.hasDesc && f.busType == BusTypeUsb;
    bool policyRemovable = hasRemovalPolicy && diskRemovalPolicy != CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL;
    const wchar_t* verdict =
        (viaUsb && policyRemovable) ? L"USB_REMOVABLE_STORAGE" :
        viaUsb ? L"USB_NON_REMOVABLE(internal?)" :
        busUsb ? L"BUSTYPE_USB_ONLY(check)" : L"NOT_USB";
    {
        Node& j = d.obj(L"judgement");
        j.boolean(L"A_usbAncestorInDevnodeTree", viaUsb);
        j.boolean(L"B_busTypeIsUsb", busUsb);
        j.boolean(L"C_diskRemovalPolicyExpectsRemoval", policyRemovable);
        j.str(L"D_usbDeviceCapRemovable", tri(usb.capRemovable));
        j.str(L"E_usbDeviceOutsideLocalMachineContainer", usb.inLocalMachineContainer < 0 ? L"(n/a)" : (usb.inLocalMachineContainer ? L"false" : L"true"));
        j.str(L"F_portIsUserConnectable", tri(usb.portUserConnectable));
        j.boolean(L"G_removableMediaBit", f.removableMedia);
        j.str(L"H_deviceHotplug", tri(f.deviceHotplug));
        j.str(L"verdict(A&&C)", verdict);
    }

    // --- 比較用サマリー行 ---
    Node& s = summary.item();
    s.str(L"kind", isCdrom ? L"CdRom" : L"Disk");
    s.str(L"number", f.hasDevNum ? fmt(L"%lu", f.deviceNumber) : L"?");
    s.str(L"model", trim(f.vendor + L" " + f.product));
    s.str(L"sizeGB", f.hasSize ? fmt(L"%.1f", f.size / 1e9) : L"");
    s.str(L"mounts", mountsSummary);
    s.str(L"driveType", driveTypesSummary);
    s.str(L"busType", f.hasDesc ? busTypeName(f.busType) : L"?");
    s.str(L"removableMedia", f.hasDesc ? (f.removableMedia ? L"true" : L"false") : L"?");
    s.str(L"mediaPresent", tri(f.mediaPresent));
    s.str(L"deviceHotplug", tri(f.deviceHotplug));
    s.str(L"diskRemovalPolicy", hasRemovalPolicy ? removalPolicyName(diskRemovalPolicy) : L"?");
    s.str(L"diskSafeRemovalRequired", hasSafeRemoval ? (diskSafeRemoval ? L"true" : L"false") : L"(n/a)");
    s.str(L"usbAncestor", viaUsb ? L"true" : L"false");
    s.str(L"usbVidPid", usb.vidPid);
    s.str(L"usbSerial", usb.serial);
    s.str(L"usbProduct", usb.product);
    s.str(L"usbSpeed", usb.speed);
    s.str(L"usbStorageDriver", storageDriver);
    s.str(L"usbCapRemovable", viaUsb ? tri(usb.capRemovable) : L"");
    s.str(L"usbInLocalMachineContainer", viaUsb ? tri(usb.inLocalMachineContainer) : L"");
    s.str(L"usbPortUserConnectable", viaUsb ? tri(usb.portUserConnectable) : L"");
    s.str(L"verdict", verdict);
    s.str(L"instanceId", instId);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage()
{
    fputws(
        L"usbinfo - USB device / storage information dumper\n"
        L"\n"
        L"usage: usbinfo [options]\n"
        L"  -f, --format text|json|csv   output format (default: text)\n"
        L"                               csv outputs only the per-disk comparison summary\n"
        L"  -o, --out FILE               write UTF-8 output to FILE instead of stdout\n"
        L"      --no-wmi                 skip WMI queries\n"
        L"      --no-props               skip the full devnode property dump\n"
        L"  -h, --help                   show this help\n",
        stderr);
}

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

int wmain(int argc, wchar_t** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if ((a == L"-f" || a == L"--format") && i + 1 < argc) opt.format = argv[++i];
        else if ((a == L"-o" || a == L"--out") && i + 1 < argc) opt.outFile = argv[++i];
        else if (a == L"--no-wmi") opt.wmi = false;
        else if (a == L"--no-props") opt.allProps = false;
        else { usage(); return (a == L"-h" || a == L"--help") ? 0 : 2; }
    }
    if (opt.format != L"text" && opt.format != L"json" && opt.format != L"csv") { usage(); return 2; }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);   // メディアなしドライブのダイアログ抑止

    Node root;
    root.str(L"tool", L"usbinfo 1.0");
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        root.str(L"generatedAt", fmt(L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        DWORD n = _countof(name);
        if (GetComputerNameW(name, &n)) root.str(L"computer", name);
        OSVERSIONINFOW vi = { sizeof(vi) };
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto rtlGetVersion = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
        if (rtlGetVersion && rtlGetVersion(&vi) == 0) root.str(L"os", fmt(L"Windows %lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber));
    }
    Node& summary = root.arr(L"summary");
    Node& disks = root.arr(L"disks");
    Node& usbDevices = root.arr(L"usbDevices");

    // --- USB デバイス全件 ---
    std::map<std::wstring, UsbBrief> usbCache;
    for (auto& path : listInterfaces(GUID_DEVINTERFACE_USB_DEVICE)) {
        std::wstring id = interfaceInstanceId(path);
        DEVINST di = 0;
        if (id.empty() || CM_Locate_DevNodeW(&di, const_cast<DEVINSTID_W>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        Node& u = usbDevices.item();
        u.str(L"interfacePath", path);
        briefNode(di, u.obj(L"devnode"));
        UsbBrief brief;
        usbInfo(di, u.obj(L"usb"), brief);
        usbCache[id] = brief;
        Node tmp;
        dumpDescendants(di, tmp, 0);
        Node& c = u.arr(L"descendants");
        c.kids = std::move(tmp.kids);
        if (opt.allProps) dumpAllProps(di, u.obj(L"allProperties"));
    }

    // --- ディスク / 光学ドライブ全件 (比較のため USB 以外も出力) ---
    std::vector<VolumeInfo> vols = collectVolumes();
    WmiData wmi;
    bool comInit = false;
    if (opt.wmi) {
        comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        std::wstring e1, e2, e3;
        wmi.diskDrive = wmiQuery(L"ROOT\\CIMV2", L"SELECT * FROM Win32_DiskDrive", e1);
        wmi.msftDisk = wmiQuery(L"ROOT\\Microsoft\\Windows\\Storage", L"SELECT * FROM MSFT_Disk", e2);
        wmi.msftPhysicalDisk = wmiQuery(L"ROOT\\Microsoft\\Windows\\Storage", L"SELECT * FROM MSFT_PhysicalDisk", e3);
        if (!e1.empty()) root.str(L"wmiError.Win32_DiskDrive", e1);
        if (!e2.empty()) root.str(L"wmiError.MSFT_Disk", e2);
        if (!e3.empty()) root.str(L"wmiError.MSFT_PhysicalDisk", e3);
    }
    for (auto& path : listInterfaces(GUID_DEVINTERFACE_DISK)) processDisk(path, false, opt, vols, wmi, usbCache, disks, summary);
    for (auto& path : listInterfaces(GUID_DEVINTERFACE_CDROM)) processDisk(path, true, opt, vols, wmi, usbCache, disks, summary);
    if (comInit) CoUninitialize();

    // --- 出力 ---
    std::wstring out;
    if (opt.format == L"json") { emitJson(root, out, 0); out += L"\n"; }
    else if (opt.format == L"csv") emitCsv(summary, out);
    else { size_t i = 0; for (const Node& k : root.kids) emitText(k, out, 0, false, i++); }

    std::string utf8 = toUtf8(out);
    if (!opt.outFile.empty()) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, opt.outFile.c_str(), L"wb") != 0 || !fp) { fwprintf(stderr, L"cannot open %s\n", opt.outFile.c_str()); return 1; }
        if (opt.format == L"csv") fwrite("\xEF\xBB\xBF", 1, 3, fp);   // Excel 用 BOM
        fwrite(utf8.data(), 1, utf8.size(), fp);
        fclose(fp);
    } else {
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(so, &mode)) {
            DWORD written = 0;
            WriteConsoleW(so, out.data(), (DWORD)out.size(), &written, nullptr);
        } else {
            DWORD written = 0;
            WriteFile(so, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
    }
    return 0;
}
