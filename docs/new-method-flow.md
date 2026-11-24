# 新方式のドライブ抽出フロー (設計メモ)

`usbinfo.cpp` の `drivesel::SelectDrivesNew()` が行っている、「リムーバブルな USB ストレージのドライブレター」の抽出手順です。
VID:PID で機器を指定するのではなく、**USB 経由で接続された、取り外し可能なディスク**かどうかを、OS が持つデバイスツリーと取り外しポリシーから判定します。

出力は既存方式と同じ `DriveEntry { driveIndex, devInst }` のリストです。`devInst` はボリュームのインターフェイス (`GUID_DEVINTERFACE_VOLUME`) を持つ devnode です。
同じ形で VID:PID を照合する方式は [whitelist-flow.md](whitelist-flow.md) にあります。

## 考え方: ボリュームからディスク、ディスクから USB 機器へたどる

ドライブレターを持つボリュームを起点に、次の順で対応付けて判定します。

```
ドライブレター (E:)
 └ ボリューム    STORAGE\Volume\{GUID}#...       ← GUID_DEVINTERFACE_VOLUME
     │  (ディスク番号で対応付け。ボリュームの親は ROOT\volmgr なので、親をたどっても USB に着かない)
     ↓
   ディスク      USBSTOR\Disk&... / SCSI\Disk&...  ← GUID_DEVINTERFACE_DISK。取り外しポリシーはここで見る
     │  (CM_Get_Parent で親へ)
     ↓
   USB デバイス本体  USB\VID_xxxx&PID_xxxx\...    ← 親が USB ハブのノード
     ↑
   USB ハブ          (GUID_DEVINTERFACE_USB_HUB を登録している)
```

| 段 | 何を決めるか | 使う値 |
|---|---|---|
| ディスクのボリュームか | 光学ドライブ・フロッピーを外す | ボリュームの ID の形、`IOCTL_STORAGE_GET_DEVICE_NUMBER` の `DeviceType` |
| USB 経由か | 内蔵 SSD などを外す | ディスクから親をたどって USB ハブに着くか |
| 取り外し可能か | 内蔵の USB 接続ストレージを外す | ディスクの `DEVPKEY_Device_RemovalPolicy` |
| (任意) 内蔵の USB 機器でないか | 同上をさらに絞る | USB デバイス本体の `CM_DEVCAP_REMOVABLE` |

`GetDriveType` は使いません。USB の HDD/SSD は `DRIVE_FIXED` になり、`DRIVE_REMOVABLE` で絞ると漏れるためです ([findings.md](findings.md))。

## フロー

### 1. ドライブレターとボリューム GUID の表を作る

| API | 主な引数 | 判定 |
|---|---|---|
| `GetVolumeNameForVolumeMountPointW` | `L"A:\\"` 〜 `L"Z:\\"` | 成功したレターだけ、(ドライブ index, `\\?\Volume{GUID}\`) を控える |

### 2. ディスク番号 → ディスクの表を作る

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` | `&GUID_DEVINTERFACE_DISK`, `nullptr`, `nullptr`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | ディスクだけを列挙する |
| `SetupDiEnumDeviceInterfaces` | 同じ GUID、index を 0 から増やす | `FALSE` で終了 |
| `SetupDiGetDeviceInterfaceDetailW` | 2 段階呼び出し、`SP_DEVINFO_DATA` も受け取る | `DevicePath` とディスクの `DevInst` を得る |
| `CreateFileW` | `DevicePath`, アクセス `0`, `FILE_SHARE_READ \| FILE_SHARE_WRITE`, `OPEN_EXISTING` | アクセス 0 なので管理者権限は不要 |
| `DeviceIoControl` | `IOCTL_STORAGE_GET_DEVICE_NUMBER` → `STORAGE_DEVICE_NUMBER` | `DeviceType == FILE_DEVICE_DISK` のものだけ、`DeviceNumber` → ディスクの `DevInst` の表に入れる |

### 3. ボリュームを列挙し、ディスクに対応付ける

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` | `&GUID_DEVINTERFACE_VOLUME`, `nullptr`, `nullptr`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | |
| `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetailW` | 2. と同じ | `DevicePath` とボリュームの `DevInst` (= `DriveEntry.devInst`) を得る |
| `GetVolumeNameForVolumeMountPointW` | `DevicePath + L"\\"` | 1. の表と GUID を `_wcsicmp` で比べる。一致するレターがなければ対象外 |
| `CM_Get_Device_IDW` | ボリュームの `DevInst` | ID が `STORAGE\Volume\` で始まらなければ対象外 (`_wcsnicmp`)。**光学ドライブとフロッピーはここで外れる** |
| `CreateFileW` + `DeviceIoControl` | `DevicePath` に `IOCTL_STORAGE_GET_DEVICE_NUMBER` | 失敗したら対象外 (複数ディスクにまたがるボリューム)。`DeviceType != FILE_DEVICE_DISK` なら対象外 |
| (表引き) | `DeviceNumber` で 2. の表を引く | 見つからなければ対象外 |

ボリュームの devnode (`STORAGE\Volume\...`) の親はボリュームマネージャー (`ROOT\volmgr\0000`) で、親をたどっても USB 機器には着きません。そのため、ディスク番号でディスクに対応付けてから、ディスク側で親をたどります。

`STORAGE\Volume\` の確認を番号の表引きより先に行うのは、フロッピーを除外するためです。
フロッピーのボリュームに `IOCTL_STORAGE_GET_DEVICE_NUMBER` を発行すると、`DeviceType` が `FILE_DEVICE_DISK`、`DeviceNumber` がフロッピーの通し番号 (`Floppy0` なら 0) になり、`PhysicalDrive0` と区別できません ([README](../README.md) の「volumeInterfaces[] の各項目」を参照)。
光学ドライブとフロッピーのボリュームは、インターフェイスを持つ devnode がドライブ自身 (`USBSTOR\CdRom&...`、`USBSTOR\SFloppy&...` など) なので、ID の形で外れます。

### 4. ディスクから USB デバイス本体を探す

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_Parent` | ディスクの `DevInst` から順に親へ | `CR_SUCCESS` でなくなったら (ルートに着いたら) USB でないので対象外 |
| `CM_Get_Device_IDW` | 親の `DevInst` | 親の ID を得る |
| `CM_Get_Device_Interface_List_SizeW` | `&GUID_DEVINTERFACE_USB_HUB`, 親の ID, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | 返った長さが 1 より大きければ、親は USB ハブ。そのとき**子のほう**を USB デバイス本体とする |

コードでは `findUsbDevice()` と `isUsbHubNode()` です。
インターフェイスを持たないノードでも、終端の NUL 1 文字ぶんの長さ (1) が返るので、1 より大きいかで判定します。

BOT (`USBSTOR\Disk&...`)、UAS (`SCSI\Disk&...`)、ハブ経由のいずれでも、この方法で USB デバイス本体 (`USB\VID_...`) に着くことを確認しています ([findings.md](findings.md))。
UAS 接続のディスクの ID は内蔵 NVMe (`SCSI\Disk&Ven_NVMe&...`) と同じ形なので、ID の文字列では USB かどうかを判別できません。親をたどる必要があるのはこのためです。

### 5. 取り外しポリシーを確かめる

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_DevNode_PropertyW` | **ディスク**の `DevInst`, `&DEVPKEY_Device_RemovalPolicy` (`DEVPROP_TYPE_UINT32`) | 取得できない、または `CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL` (1) なら対象外。`ExpectOrderlyRemoval` (2)、`ExpectSurpriseRemoval` (3) なら次へ |

USB デバイス本体ではなく、ディスクの devnode の値を見ます。
内蔵 NVMe は `ExpectNoRemoval`、調べた USB 機器はすべて `ExpectSurpriseRemoval` でした ([findings.md](findings.md))。
ノート PC の内蔵カードリーダーのような、USB 接続だが内蔵の機器を、ここで外せることを期待しています (未確認)。

### 6. (任意) 内蔵の USB 機器を外す

`excludeInternalUsb` が true のとき (`usbinfo` では `--exclude-internal-usb`) だけ行います。

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_DevNode_PropertyW` | **USB デバイス本体**の `DevInst`, `&DEVPKEY_Device_Capabilities` (`DEVPROP_TYPE_UINT32`) | `CM_DEVCAP_REMOVABLE` が立っていなければ対象外。プロパティが取得できないときは除外しない |

`CM_DEVCAP_REMOVABLE` は、USB の非ストレージ機器では内蔵 (Bluetooth アダプターなど) と外付けを正しく分けました。内蔵の USB ストレージで分けられるかは未確認です。
ACPI の記述 (`_UPC`、`_PLD`) に依存するため、記述が不正確な機種では、内蔵機器が取り外し可能と報告されることがあります。

`DEVPKEY_Device_InLocalMachineContainer` も取得していますが、出力の参考値で、判定には使っていません。

### 7. 採用する

3.〜5. (と 6.) をすべて満たしたボリュームについて、1. の表で一致したドライブレターすべてを `DriveEntry { driveIndex, devInst(ボリューム) }` として返します。
1 つのボリュームが複数のレターを持つ場合は、すべてのレターが採用されます。

### 8. 再評価のタイミング

ボリュームが現れたり消えたりするたびに、1. からやり直します。

| API | 主な引数 | 判定 |
|---|---|---|
| `RegisterDeviceNotificationW` | `DEV_BROADCAST_DEVICEINTERFACE_W` (`dbcc_classguid = GUID_DEVINTERFACE_VOLUME`), `DEVICE_NOTIFY_WINDOW_HANDLE` | `WM_DEVICECHANGE` の `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE` で 1. からやり直す |

ウィンドウを持たないプロセスでは `CM_Register_Notification` (`CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE`、同じ GUID) を使えます。
既存コードが `DBT_DEVTYP_VOLUME` の通知で再抽出しているなら、それをそのまま使ってもかまいません。

## 機器ごとの結果

| 機器 | 外れる段 / 採用 | 結果 | 確認 |
|---|---|---|---|
| 内蔵 NVMe SSD | 4. USB ハブに着かない | 対象外 | 実機で確認 |
| USB メモリ (BOT) | すべて通る | **採用** | 実機で確認 |
| USB-SATA 変換の SSD (BOT、`DRIVE_FIXED`) | すべて通る | **採用** | 実機で確認 |
| USB の SSD (UAS、ID が `SCSI\Disk&...`) | すべて通る | **採用** | 判定値は確認。ドライブレターのある状態では未確認 |
| USB の光学ドライブ | 3. ボリュームの ID がドライブ自身 | 対象外 | `verdict` で確認。メディアありは未確認 |
| ELECOM 暗号化 USB の仮想 CD-ROM | 3. ボリュームの ID が `USBSTOR\CdRom&...` | 対象外 | 想定 (未確認) |
| ELECOM 暗号化 USB の暗号化領域 (ロック解除後) | すべて通る | **採用** | 想定 (未確認) |
| ELECOM 暗号化 USB の暗号化領域 (ロック解除前) | メディアなしのディスクとしてレターが付く製品では、5. まで通る | 採用されうる (空のドライブ) | 想定 (未確認) |
| USB フロッピー | 3. ボリュームの ID がドライブ自身 | 対象外 | 未確認 |
| ノート PC の内蔵カードリーダー (USB 接続) | 5. または 6. | 機種による | 未確認 |
| SD ホストコントローラー接続のカードリーダー、eSATA | 4. USB ハブに着かない | 対象外 | 未確認 |

## 既存方式との違い

| 条件 | 既存方式 | 新方式 |
|---|---|---|
| `GetDriveType == DRIVE_REMOVABLE` | 必須 | 使わない |
| USB 経由かの判定 | ボリュームの ID が `USBSTOR#` を含む、または親の ID が `USBSTOR\` を含む | ディスクから親をたどり USB ハブに着く |
| 光学ドライブの除外 | `GetDriveType` が `DRIVE_CDROM` になるので外れる | ボリュームの ID の形と `DeviceType` |
| 取り外し可能かの判定 | `GetDriveType` (メディアが抜けるか) | ディスクの取り外しポリシー |

この結果、USB の HDD/SSD (`DRIVE_FIXED`) と UAS 接続の機器が新たに抽出されます。実機での比較は [findings.md](findings.md) の「既存方式と新方式の比較」にあります。

## 判定が OS の振る舞いに依存する箇所

| 箇所 | 依存しているもの | 変わったときの影響 |
|---|---|---|
| 3. ボリュームの ID が `STORAGE\Volume\` で始まる | ボリュームマネージャーが作る devnode の ID の形 | 形が変わると、ディスクのボリュームがすべて外れる |
| 4. 親が USB ハブ | ハブのドライバーが `GUID_DEVINTERFACE_USB_HUB` を登録していること | 登録しないハブドライバーの配下では、USB 機器がすべて外れる |
| 5. 取り外しポリシー | ストレージドライバーと ACPI の記述 | 外付け機器が `ExpectNoRemoval` と報告されると外れる。内蔵機器が `ExpectSurpriseRemoval` と報告されると入る |
| 6. `CM_DEVCAP_REMOVABLE` | ACPI の記述 (`_UPC`、`_PLD`) | 5. と同じ |

5. と 6. は機種やドライバーによって変わりうる値です。VID:PID の照合だけで判定したい場合は、[whitelist-flow.md](whitelist-flow.md) の方式を使います。

## 採用前に確認すること

`usbinfo -f letters` で、既存方式と新方式の抽出結果を 1 ドライブ 1 行で比べられます。`-f json` の `driveLetters[].newDetail.result` に、外れた理由が出ます。

| 確認すること | 見る場所 |
|---|---|
| ELECOM の仮想 CD-ROM のレターが外れるか | `-f letters` の `New: result` が `not a disk volume (optical/floppy)` |
| ロック解除前の暗号化領域にレターが付くか、採用されるか | ロック解除前の `-f letters` |
| 内蔵カードリーダーが外れるか | `-f letters` の `New: result` が `removal policy is ExpectNoRemoval`。外れなければ `--exclude-internal-usb` 付きで再確認 |
| Windows 10 で同じ結果になるか | 調査は Windows 11 のみ |

## 実装上の注意

- インスタンス ID の比較は大文字小文字を区別しない (`_wcsicmp` / `_wcsnicmp`)。`CM_Get_Device_ID` は大文字 (`STORAGE\VOLUME\...`) で返ります ([findings.md](findings.md))。
- `namespace drivesel` は usbinfo の他の部分に依存しないので、そのまま移植できます (`setupapi.lib` と `cfgmgr32.lib` が必要)。
- 1 つの USB デバイスに複数のディスクやパーティションがある場合、条件を満たすレターはすべて採用されます。1 台につき 1 レターに絞るなら、USB デバイス本体の `DevInst` でまとめてください。
