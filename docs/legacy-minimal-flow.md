# 既存コードの最小改修案 (設計メモ)

既存方式 (既存コード) の構造をそのまま残し、数行の変更だけで、次の 2 つを抽出できるようにする案です。

- **UAS で動く SanDisk の USB メモリ** (既存コードの検証環境の K:、`0781:55A3`)
- **USB 接続の HDD/SSD** (同じく J:、`4971:1025`。1 台目の PC の E: も同じ)

どちらも既存コードでは漏れることを、実機で確認しています ([findings.md](findings.md) の「既存コードの検証環境での比較」)。
usbinfo では `drivesel::SelectDrivesLegacyMinimal()` として実装しています。
新方式 ([new-method-flow.md](new-method-flow.md)) やホワイトリスト方式 ([whitelist-flow.md](whitelist-flow.md)) のような作り直しをせず、既存コードへの差分を小さく保つことを優先します。

## 既存コードが漏らす原因

既存コード (`drivesel::SelectDrivesLegacy()` で再現しているもの) の判定は 3 段です。

1. `GetDriveType` が `DRIVE_REMOVABLE` のドライブレターだけを残す。
2. `GUID_DEVINTERFACE_VOLUME` のボリュームのうち、ボリュームの ID が `USBSTOR#` を含むか、親の ID が `USBSTOR\` を含むものを USB とみなす。
3. ボリューム GUID が 1. のレターと一致すれば採用する。

| 機器 | `GetDriveType` | ボリュームの ID | 外れる段 | 原因 |
|---|---|---|---|---|
| SanDisk の USB メモリ (UAS、K:) | `REMOVABLE` | `STORAGE\Volume\_??_SCSI#Disk&...` | 2. | UAS で動くとディスクの列挙子が `USBSTOR` ではなく `SCSI` になり、ボリュームの ID に `USBSTOR#` が入らない |
| USB 接続の HDD/SSD (J:、E:) | `FIXED` | `STORAGE\Volume\{GUID}#<オフセット>` | 1. (2. でも外れる) | 固定ディスク扱いで `DRIVE_REMOVABLE` にならない。ボリュームの ID にもディスクの情報が入らない |

SanDisk の USB メモリだけなら、2. の文字列に `_??_SCSI#` を足せば拾えます。
しかし、USB の HDD/SSD はボリュームの ID に USB の手がかりがまったくないので、文字列の変更では対応できません。
そこで、USB かどうかを ID の文字列以外の方法でも確かめるようにします。

## 変更点

| 変更 | 内容 | 目的 |
|---|---|---|
| 1. ドライブの種類 | `GetDriveType` が `DRIVE_REMOVABLE` **または `DRIVE_FIXED`** のレターを残す | USB の HDD/SSD を候補に入れる。光学ドライブ (`DRIVE_CDROM`) は引き続き外れる |
| 2. USB の判定 | 既存の ID の確認に、**ディスクのバス種別が USB か** (`STORAGE_DEVICE_DESCRIPTOR.BusType == BusTypeUsb`) を OR で加える | UAS の機器と USB の HDD/SSD を USB と判定する |
| 3. フロッピーの除外 | ボリュームの devnode が `GUID_DEVINTERFACE_FLOPPY` を登録していれば外す | 変更 2 で USB フロッピーが新たに USB と判定されうるので、それを防ぐ |

既存の ID の確認は残すので、**既存コードで抽出されていたドライブは、すべて引き続き抽出されます** (変更 3 で外れるフロッピーは、既存コードでも ID の確認で外れていました)。
変更後の抽出結果は、既存コードの結果に、変更 2 で USB と判定されたものを足したものになります。

### バス種別を使う理由

| 機器 | ボリュームの ID | バス種別 |
|---|---|---|
| USB の SSD (UAS) 2 台 | (ドライブレターなし) | `Usb` (実測) |
| USB-SATA 変換の SSD (BOT、`DRIVE_FIXED`) | `{GUID}#...` | `Usb` (実測) |
| USB の BD ドライブ (BOT) | ドライブ自身 | `Usb` (実測) |
| 内蔵 NVMe SSD | `{GUID}#...` | `Nvme` (実測) |
| USB メモリ (BOT、UAS) | `_??_USBSTOR#...`、`_??_SCSI#...` | 記録なし (`Usb` の想定) |

1 台目の PC では、調べた USB 機器はすべて `Usb`、内蔵 NVMe は `Nvme` でした ([findings.md](findings.md) の「判定に関わる値の実測」)。USB メモリと、既存コードの検証環境の機器のバス種別は記録していません。
`BusType` はストレージドライバーが申告するバスの種類で、転送方式 (BOT / UAS) やボリュームの ID の形に左右されません ([STORAGE_DEVICE_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-storage_device_descriptor): "BusType: Specifies an enumerator value of type STORAGE_BUS_TYPE that indicates the type of bus to which the device is connected")。
既存コードと同じく「1 回の IOCTL で済む値」なので、構造を変えずに足せます。

## フロー

### 1. ドライブレターごとに、種類とバス種別を控える

| API | 主な引数 | 判定 |
|---|---|---|
| `GetDriveTypeW` | `L"A:\\"` 〜 `L"Z:\\"` | **`DRIVE_REMOVABLE` または `DRIVE_FIXED`** でなければ対象外 (変更 1) |
| `GetVolumeNameForVolumeMountPointW` | 同じルートパス | ボリューム GUID を控える (既存と同じ) |
| `CreateFileW` | `L"\\\\.\\X:"`, アクセス `0`, `FILE_SHARE_READ \| FILE_SHARE_WRITE`, `OPEN_EXISTING` | 開けなければバス種別は「USB でない」とする (変更 2) |
| `DeviceIoControl` | `IOCTL_STORAGE_QUERY_PROPERTY` (`PropertyId = StorageDeviceProperty`, `QueryType = PropertyStandardQuery`) → `STORAGE_DEVICE_DESCRIPTOR` | `BusType == BusTypeUsb` なら「USB」と控える。失敗したら「USB でない」(変更 2) |

### 2. ボリュームを列挙し、USB かどうかを確かめる

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` / `SetupDiEnumDeviceInterfaces` | `&GUID_DEVINTERFACE_VOLUME`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | 既存と同じ |
| `CM_Get_Device_IDW` / `CM_Get_Parent` | ボリュームの `DevInst` | ID が `USBSTOR#` を含むか、親の ID が `USBSTOR\` を含むかを控える (既存と同じ) |
| `CM_Get_Device_Interface_List_SizeW` + `CM_Get_Device_Interface_ListW` | `&GUID_DEVINTERFACE_FLOPPY`, ボリュームの devnode の ID, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | 一覧が空でなければフロッピーのドライブ自身なので対象外 (変更 3) |
| `SetupDiGetDeviceInterfaceDetailW` + `GetVolumeNameForVolumeMountPointW` | ボリュームの `DevicePath + L"\\"` | ボリューム GUID を得る (既存と同じ。ただし ID の確認に当たらなかったボリュームでも取得する) |

### 3. 採用する

ボリューム GUID が 1. のレターと一致し、かつ次のどちらかを満たせば採用します。

- 既存の ID の確認に当たった (`USBSTOR#` / `USBSTOR\`)。
- そのレターのバス種別が `Usb` だった (変更 2)。

## 既存コードへの差分 (例)

このプロジェクトで再現している `SelectDrivesLegacy()` に対する差分です。既存コードそのものではないので、同じ意味の変更を既存コードに当ててください。

```diff
 static std::vector<DriveEntry> SelectDrivesLegacy()
 {
-    struct VolumeInfo { int index; std::wstring guid; };
+    struct VolumeInfo { int index; std::wstring guid; bool busUsb; };
     std::vector<VolumeInfo> volumeInfos;
     for (int i = 0; i < 26; ++i) {
         wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
-        if (GetDriveTypeW(root) != DRIVE_REMOVABLE) continue;
+        UINT type = GetDriveTypeW(root);
+        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;          // 変更 1
         wchar_t guid[MAX_PATH] = L"";
         if (!GetVolumeNameForVolumeMountPointW(root, guid, _countof(guid))) continue;
-        volumeInfos.push_back({ i, guid });
+        volumeInfos.push_back({ i, guid, isUsbBus(i) });                        // 変更 2
     }
     ...
         bool idHit = id.find(L"USBSTOR#") != std::wstring::npos;
         bool parentHit = parentId.find(L"USBSTOR\\") != std::wstring::npos;
-        if (!idHit && !parentHit) continue;
+        if (hasInterface(id, GUID_DEVINTERFACE_FLOPPY)) continue;              // 変更 3

         std::wstring path;
         if (!interfaceDetail(h, ifData, path, nullptr)) continue;
         std::wstring guid = volumeGuidOf(path);
         if (guid.empty()) continue;
         for (auto& v : volumeInfos)
-            if (v.guid == guid) result.push_back({ v.index, info.DevInst });
+            if (v.guid == guid && (idHit || parentHit || v.busUsb))            // 変更 2
+                result.push_back({ v.index, info.DevInst });
     }
```

追加する関数:

```cpp
// ドライブレター (0 = A:) のディスクのバス種別が USB か
static bool isUsbBus(int driveIndex)
{
    wchar_t path[] = { L'\\', L'\\', L'.', L'\\', (wchar_t)(L'A' + driveIndex), L':', 0 };
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr);
    CloseHandle(h);
    const auto* d = (const STORAGE_DEVICE_DESCRIPTOR*)buf;
    return ok && ret >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(d->BusType) && d->BusType == BusTypeUsb;
}
```

`hasInterface()` は、このプロジェクトの `namespace drivesel` にあるものと同じです (一覧を取得し、空でないかで判定する)。

## ケースごとの抽出結果

| ケース | 既存コード | 最小改修案 | 新方式 |
|---|---|---|---|
| USB メモリ (BOT、H: / D:) | 抽出 | 抽出 | 抽出 |
| **SanDisk の USB メモリ (UAS、K:)** | 抽出しない | **抽出** | 抽出 |
| **USB の HDD/SSD (`DRIVE_FIXED`、J: / E:)** | 抽出しない | **抽出** | 抽出 |
| 光学ドライブ、暗号化 USB の仮想 CD-ROM | 抽出しない | 抽出しない (`DRIVE_CDROM`) | 抽出しない |
| フロッピー (A:) | 抽出しない | 抽出しない (変更 3) | 抽出しない |
| 内蔵ディスク (C:〜F:) | 抽出しない | 抽出しない (バス種別が `Usb` でない) | 抽出しない |
| ノート PC の内蔵 USB カードリーダー | `DRIVE_REMOVABLE` なら抽出 | 抽出 | 取り外しポリシー次第 |
| 内蔵の USB 接続の固定ディスク | 抽出しない | **抽出** (下記) | 取り外しポリシー次第 |
| USB から起動したシステムドライブ (C:) | 抽出しない | **抽出** (下記) | 抽出 |
| SD ホストコントローラー接続のカードリーダー | 抽出しない | 抽出しない (バス種別が `Sd` の想定) | 抽出しない |

既存コードの検証環境 (A:〜K:) では、最小改修案の結果は新方式と同じ H:、J:、K: になる想定です (未確認)。

## 弱いところ

- **`BusType == Usb` は「取り外せる」ことを意味しません。** 内蔵の USB 接続の機器も `Usb` です。`DRIVE_REMOVABLE` の機器 (内蔵カードリーダーなど) は既存コードでも抽出されていたので変わりませんが、変更 1 で `DRIVE_FIXED` を候補に入れたため、内蔵の USB 接続の固定ディスクが新たに抽出されます。ノート PC の一部に USB 接続の内蔵 SSD があります。
- **USB から Windows を起動している PC では、システムドライブも抽出されます。** 既存コードでは `DRIVE_FIXED` で外れていました。抽出したドライブに書き込み制限などをかける用途なら、Windows のあるボリューム (`GetWindowsDirectoryW` → `GetVolumePathNameW` → `GetVolumeNameForVolumeMountPointW`) を除外する変更を足すことを勧めます。
- **バス種別は、ベンダー製ドライバーでは `Usb` にならないことがあります** (README の「判定方法について」)。ただし既存の ID の確認は残しているので、既存コードで抽出できていた機器が漏れることはありません。漏れるのは、変更 2 で新たに拾おうとしている UAS の機器と USB の HDD/SSD のうち、そうしたドライバーを使うものだけです。
- **ボリュームのハンドル (`\\.\X:`) に `IOCTL_STORAGE_QUERY_PROPERTY` を発行すると、下のディスクの値が返る**ことは、広く使われている方法ですが、ドキュメントに明記はありません。複数のディスクにまたがるボリュームでは失敗し、その場合は既存の ID の確認だけで判定します。
- **USB フロッピーでバス種別が `Usb` になるかは未確認です。** 変更 3 は、なったときのための予防です。

## 他の方式との比較

| | 既存コード | 最小改修案 | 新方式 | ホワイトリスト方式 | タスクトレイ方式 |
|---|---|---|---|---|---|
| 既存コードからの変更量 | — | 数行 | 作り直し | 作り直し | 作り直し |
| SanDisk の UAS メモリ、USB HDD/SSD | 漏れる | 抽出 | 抽出 | 登録すれば抽出 | 抽出 (想定) |
| 既存コードで抽出していたものを必ず抽出するか | — | する | しない (取り外しポリシーで外れうる) | しない (未登録は外れる) | しない |
| 内蔵の USB 機器の扱い | `DRIVE_REMOVABLE` なら抽出 | `Usb` なら抽出 | 取り外しポリシーで外す | 登録次第 | 取り外せる祖先がなければ外す |
| ID の文字列の解析 | する | する (既存部分) | しない | しない | しない |

最小改修案は、「既存の抽出結果を一切減らさずに、漏れていた 2 種類を足す」ことに目的を絞った案です。
取り外せるかどうかの判定は既存コード (`GetDriveType`) のままで、新方式のような判定の見直しはしていません。

## 採用前に確認すること

最小改修案は usbinfo に実装済みです。既存コードの検証環境で次のどちらかを実行すれば、既存コード・新方式と並べて結果を確かめられます。

- `usbinfo -f letters`: `Min` 列が最小改修案の結果、`Bus` 列がバス種別です。
- `usbinfo -f code --vidpid`: 手で写して持ち帰る場合。13 文字目が最小改修案で抽出したか (`Y` / `N`)、14 文字目がバス種別 (`U` = Usb) です (形式 `UI2` 以降。[manual-transcription.md](manual-transcription.md) を参照)。

| 確認すること | 期待する値 |
|---|---|
| H: (既存コードで抽出していた USB メモリ) | `Min` が `selected` (13 文字目 `Y`) |
| K: (SanDisk の UAS メモリ) と J: (USB HDD) | `Min` が `selected`、`Bus` が `Usb` (13、14 文字目 `YU`) |
| 内蔵ディスク (C:〜F:) | `Min` が `-`、`Bus` が `Usb` 以外 |
| フロッピー (A:) と光学ドライブ (G:、I:) | `Min` が `-` |
| システムドライブ | `Bus` が `Usb` 以外 (USB から起動していない) |
