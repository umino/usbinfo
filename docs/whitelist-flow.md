# ホワイトリスト方式のドライブ抽出フロー (設計メモ)

許可する USB 機器を VID:PID (+ シリアル番号) で列挙しておき、それに一致した機器のドライブだけを抽出する方式の設計です。
取り外しポリシーや `GetDriveType` のような OS の申告値は使わず、VID:PID の照合と、仮想 CD-ROM を誤認識しないための最小限の確認だけで判定します。
VID:PID とシリアル番号は、インスタンス ID の文字列から切り出すのではなく、**USB ハブに IOCTL を発行して、機器のデバイスディスクリプタと文字列ディスクリプタから直接読みます**。

出力は既存方式・新方式と同じ `DriveEntry { driveIndex, devInst }` のリストとし、`namespace drivesel` に `SelectDrivesWhitelist()` として並べる想定です。

## 考え方: ディスクを起点に VID:PID を照合する

ELECOM の暗号化 USB メモリのように、1 台の USB デバイスが仮想 CD-ROM (ロック解除ソフト入り) とマスストレージを持つ機器では、2 つのドライブが同じ USB デバイスの下にあります。
VID:PID もシリアル番号も USB デバイス本体の値なので、2 つのドライブで同じになり、値だけでは区別できません。

```
USB\VID_056E&PID_xxxx\<シリアル>        ← USB デバイス本体 (親は USB ハブ)。VID:PID とシリアルはこの機器のディスクリプタにある
 ├ USBSTOR\CdRom&Ven_ELECOM&...\...&0   ← LUN 0: 仮想 CD-ROM   → GUID_DEVINTERFACE_CDROM を登録
 └ USBSTOR\Disk&Ven_ELECOM&...\...&1    ← LUN 1: マスストレージ → GUID_DEVINTERFACE_DISK を登録
```

一方、その下にできるストレージの devnode は別々で、登録するインターフェイスも違います。
そこで、ドライブから USB 機器を探すのではなく、**ディスク (`GUID_DEVINTERFACE_DISK`) を起点にして、その上の USB 機器の VID:PID を照合**します。
仮想 CD-ROM はディスクとして列挙されないので、VID:PID が一致しても候補に入りません。

判定に使う OS の仕組みは次の 3 つだけです。

| 仕組み | 用途 |
|---|---|
| ディスクのデバイスインターフェイス (`GUID_DEVINTERFACE_DISK`) | 「ディスクか」の判定。仮想 CD-ROM を除外する |
| ディスク番号の IOCTL (`IOCTL_STORAGE_GET_DEVICE_NUMBER` ほか) | ドライブレターとディスクの対応付け |
| USB ハブのインターフェイス (`GUID_DEVINTERFACE_USB_HUB`) | 「USB 経由か」の判定と、USB デバイス本体の特定 |
| USB ハブへの IOCTL (デバイスディスクリプタ、文字列ディスクリプタ) | 「登録した機器か」の判定 (VID:PID、シリアル番号) |

インスタンス ID は CM API に渡す引数として使うだけで、中身を解析しません ([instance-id.md](instance-id.md) の「前提」を参照)。

## ホワイトリストの形式

```
# VID:PID[:SERIAL][;lun=N]      # 以降はコメント。VID / PID は 16 進 4 桁、大文字小文字は区別しない
0781:5583                        # 機種単位 (この機種ならどの個体でも許可)
0781:5583:4C530001234567891234   # 個体単位 (このシリアルの個体だけ許可)
056E:xxxx;lun=1                  # 1 台に複数のディスクがある機器で、LUN 1 だけ許可
```

| フィールド | 省略時 | 照合先 |
|---|---|---|
| `VID:PID` | 省略不可 | USB デバイス本体のデバイスディスクリプタの `idVendor` / `idProduct` |
| `SERIAL` | 機種単位で照合 | USB デバイス本体の文字列ディスクリプタ (`iSerialNumber` が指すもの) |
| `lun=N` | LUN を問わない | ディスクの `SCSI_ADDRESS.Lun` |

- 仮想 CD-ROM は手順 1 で外れるので、ELECOM の暗号化 USB メモリも VID:PID を 1 行書くだけで済みます。
- `lun=` は、1 台に**ディスクが 2 つ以上**見える機器 (公開領域と暗号化領域が両方ディスクとして見える機器、複数スロットのカードリーダーなど) で、どちらかだけを許可したいときに使います。
- `SERIAL` には、機器の文字列ディスクリプタの値をそのまま登録します。`usbinfo -f csv` の `usbSerial` 列 (`-f json` では `usbDevices[].usb.deviceDescriptor.serialNumberString`) で確認できます。インスタンス ID の値ではないので、Windows がシリアル番号をどう扱うか (`UniqueID`、`IgnoreHWSerNum`、使えない文字の扱い) には左右されません。

## フロー

### 0. ホワイトリストを読み込む

- 各行を `VID`、`PID`、`SERIAL` (なしは空)、`LUN` (なしは -1) に分解して保持する。
- VID / PID は数値 (`USHORT`) にして比べる。
- 書式の誤った行はエラーにする。黙って無視すると、許可したつもりの機器が抽出されない原因が分からなくなる。

### 1. 許可するディスク番号の集合を作る

ディスクを 1 台ずつ見て、登録した USB 機器のディスクなら、その番号を集合に入れます。

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` | `&GUID_DEVINTERFACE_DISK`, `nullptr`, `nullptr`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | ディスクだけを列挙する。**仮想 CD-ROM はここに現れない** |
| `SetupDiEnumDeviceInterfaces` | 同じ GUID、index を 0 から増やす | `FALSE` で終了 |
| `SetupDiGetDeviceInterfaceDetailW` | 2 段階呼び出し、`SP_DEVINFO_DATA` も受け取る | `DevicePath` とディスクの `DevInst` を得る |
| `CM_Get_Parent` | ディスクの `DevInst` から順に親へ | ルートまでたどっても下記に当たらなければ USB でないので対象外 |
| `CM_Get_Device_IDW` + `CM_Get_Device_Interface_List_SizeW` + `CM_Get_Device_Interface_ListW` | 親の ID, `&GUID_DEVINTERFACE_USB_HUB`, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | 一覧が空でなければ親は USB ハブ。そのとき**子のほう**を USB デバイス本体とする (新方式の `findUsbDevice()` と同じ) |
| `CM_Get_Device_Interface_ListW` | `&GUID_DEVINTERFACE_USB_HUB`, ハブの ID, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | ハブのインターフェイスパスを得る |
| `CM_Get_DevNode_PropertyW` | USB デバイス本体, `&DEVPKEY_Device_Address` (`DEVPROP_TYPE_INT32` または `DEVPROP_TYPE_UINT32`) | ハブ上のポート番号を得る。取得できない、または 0 なら対象外 |
| `CreateFileW` | ハブのインターフェイスパス, `GENERIC_WRITE`, `FILE_SHARE_WRITE`, `OPEN_EXISTING` | 開けなければ対象外。usbinfo と同じ開き方 (下記の補足を参照) |
| `DeviceIoControl` | `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` → `USB_NODE_CONNECTION_INFORMATION_EX` (`ConnectionIndex` = ポート番号) | 失敗した、または `ConnectionStatus != DeviceConnected` なら対象外。`DeviceDescriptor.idVendor` / `idProduct` / `iSerialNumber` を得る |
| `DeviceIoControl` | `IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME` → `USB_NODE_CONNECTION_DRIVERKEY_NAME` (`ConnectionIndex` = ポート番号) | 返った `DriverKeyName` が USB デバイス本体の `DEVPKEY_Device_Driver` と `_wcsicmp` で一致しなければ対象外 (別のポートの機器を読んでいないことの確認) |
| `DeviceIoControl` | `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` (`ConnectionIndex` = ポート番号, `SetupPacket.wValue = (USB_STRING_DESCRIPTOR_TYPE << 8) \| 0`, `wIndex = 0`, `wLength = 255`) | `SERIAL` 付きのエントリがあり、`iSerialNumber` が 0 でないときだけ。対応言語 ID の一覧を読み、先頭の言語 ID を使う。読めなければ `0x0409` |
| `DeviceIoControl` | `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` (`wValue = (USB_STRING_DESCRIPTOR_TYPE << 8) \| iSerialNumber`, `wIndex` = 言語 ID, `wLength = 255`) | 返った `USB_STRING_DESCRIPTOR` の `bString` (UTF-16、長さは `bLength - 2` バイト) をシリアル番号とする。失敗したらシリアル番号不明 |
| `CreateFileW` | ディスクの `DevicePath`, アクセス `0`, `FILE_SHARE_READ \| FILE_SHARE_WRITE`, `OPEN_EXISTING` | アクセス 0 なので管理者権限は不要 |
| `DeviceIoControl` | `IOCTL_SCSI_GET_ADDRESS` → `SCSI_ADDRESS` | ホワイトリストに `lun=` があるときだけ必要。失敗したら LUN 不明 (-1) |
| (照合) | ホワイトリストの各エントリ | 下記の 3 条件をすべて満たすエントリが 1 つでもあれば許可 |
| `DeviceIoControl` | `IOCTL_STORAGE_GET_DEVICE_NUMBER` → `STORAGE_DEVICE_NUMBER` | 許可したディスクの `DeviceNumber` を集合に入れる。`DeviceType != FILE_DEVICE_DISK` または失敗なら入れない |

エントリと一致する条件:

1. VID と PID が一致する。
2. エントリに `SERIAL` があるとき: 機器のシリアル番号を読めて、完全に一致する (`wcscmp`)。**`iSerialNumber` が 0 の機器と、読み取りに失敗した機器は、`SERIAL` 付きのエントリには一致させない。**
3. エントリに `lun=` があるとき: `Lun` が一致する。LUN 不明なら一致させない。

補足:

- 親が USB ハブであることで USB デバイス本体を見分けるので、ハブ経由の接続でも上流のハブと取り違えません。複合デバイスでも、止まるのは `&MI_xx` のインターフェイスのノードではなく、ハブ直下のデバイス本体です。
- BOT (`USBSTOR\Disk&...`)、UAS (`SCSI\Disk&...`)、ハブ経由のいずれでも、ディスクから親をたどると USB ハブ直下の USB デバイス本体に着くことは、新方式の調査で確認しています ([findings.md](findings.md))。
- USB デバイスの `DEVPKEY_Device_Address` がハブのポート番号であることは、`DEVICE_CAPABILITIES.Address` の説明に記載があります ([DEVICE_CAPABILITIES](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_capabilities): "USB: The port number")。ドライバーキー名の照合は、その対応が正しいことの念のための確認です。usbinfo では `connection.driverKeyMatchesDevnode` で同じ確認をしています。
- `DEVPKEY_Device_Address` の型はドキュメント上 `DEVPROP_TYPE_INT32` ですが、新方式の取り外しポリシーと同じく、実機では `DEVPROP_TYPE_UINT32` で返ることがあるので両方を受け付けます。
- ハブを `GENERIC_WRITE` で開けば、管理者権限なしでハブの IOCTL が通ることは、**ドキュメントに記載がなく、観察が根拠です** (usbinfo を管理者権限なしで実行して確認。Microsoft の USBView サンプルも同じ開き方)。必要な権限が変わると、ハブを開けずにすべての機器が対象外になります。
- 3 つの IOCTL は、いずれも USB ハブ (`GUID_DEVINTERFACE_USB_HUB`) に発行するユーザーモード向けの要求です ([IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_node_connection_information_ex)、[IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_node_connection_driverkey_name)、[IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_descriptor_from_node_connection))。
- 文字列ディスクリプタの読み取りは機器との通信を伴うので、機器がサスペンド中などで失敗することがあります (usbinfo では `(read failed)` と表示)。失敗したときは `SERIAL` 付きのエントリに一致させないので、誤って抽出する方向には倒れません。機種単位のエントリ (`SERIAL` なし) の照合には影響しません。
- インスタンス ID からシリアル番号を切り出す方式と違い、`CM_DEVCAP_UNIQUEID` が立っていない機器や、`IgnoreHWSerNum` が設定された機器でも、機器が持つシリアル番号で照合できます。

### 2. ドライブレターごとに、許可したディスクのものか確かめる

| API | 主な引数 | 判定 |
|---|---|---|
| `CreateFileW` | `L"\\\\.\\A:"` 〜 `L"\\\\.\\Z:"`, アクセス `0`, `FILE_SHARE_READ \| FILE_SHARE_WRITE`, `OPEN_EXISTING` | 開けなければ対象外 |
| `DeviceIoControl` | `IOCTL_STORAGE_GET_DEVICE_NUMBER` → `STORAGE_DEVICE_NUMBER` | 失敗したら対象外。`DeviceType != FILE_DEVICE_DISK` なら対象外 (**仮想 CD-ROM のレターはここで外れる**)。`DeviceNumber` が 1. の集合になければ対象外 |
| `DeviceIoControl` | `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` → `VOLUME_DISK_EXTENTS` | 失敗したら対象外 (**フロッピーはここで外れる想定**)。全エクステントの `DiskNumber` が 1. の集合にあれば採用 |

`GetDriveType` では絞りません (USB の HDD/SSD は `DRIVE_FIXED` になるため)。

2 つの IOCTL を両方使う理由は、番号の衝突を防ぐためです。

- **光学ドライブ**: `DeviceNumber` は `CdRom0` の 0 のような、ディスクとは別の通し番号です。番号だけで照らすと、許可したディスクの番号と偶然一致することがあります。`DeviceType` が `FILE_DEVICE_CD_ROM` になるので、それで除外します。
- **フロッピー**: `DeviceType` が `FILE_DEVICE_DISK` になり、`DeviceNumber` は `Floppy0` の 0 のようなフロッピーの通し番号です ([README](../README.md) の「volumeInterfaces[] の各項目」を参照)。`DeviceType` では除外できません。フロッピーのボリュームはボリュームマネージャーを通らないので、`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` が失敗するはずです。これを除外の根拠にします (未確認。下記「採用前に確認すること」を参照)。

複数のディスクにまたがるボリューム (ダイナミックディスクなど) は、`IOCTL_STORAGE_GET_DEVICE_NUMBER` が失敗するので対象外になります。

### 3. (必要なときだけ) `DriveEntry.devInst` を得る

既存コードの出力と同じく、ボリュームのインターフェイスを持つ devnode が必要な場合だけ行います。判定には使いません。

| API | 主な引数 | 判定 |
|---|---|---|
| `GetVolumeNameForVolumeMountPointW` | 採用したレター `L"X:\\"` | ボリューム GUID パスを得る |
| `SetupDiGetClassDevsW` / `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetailW` | `&GUID_DEVINTERFACE_VOLUME`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | 各ボリュームの `DevicePath` と `DevInst` を得る |
| `GetVolumeNameForVolumeMountPointW` | `DevicePath + L"\\"` | GUID パスがレターのものと `_wcsicmp` で一致したボリュームの `DevInst` を使う |

### 4. 再評価のタイミング

暗号化 USB メモリでは、ロック解除の操作でドライブの状態が変わります。抽出は接続時の 1 回で終わらせず、ボリュームが現れたり消えたりするたびに 1. からやり直します。

| API | 主な引数 | 判定 |
|---|---|---|
| `RegisterDeviceNotificationW` | `DEV_BROADCAST_DEVICEINTERFACE_W` (`dbcc_classguid = GUID_DEVINTERFACE_VOLUME`), `DEVICE_NOTIFY_WINDOW_HANDLE` | `WM_DEVICECHANGE` の `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE` で 1. からやり直す |

ウィンドウを持たないプロセスでは `CM_Register_Notification` (`CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE`、同じ GUID) を使えます。
既存コードが `WM_DEVICECHANGE` の `DBT_DEVTYP_VOLUME` で再抽出しているなら、それをそのまま使ってもかまいません。どの通知でも、抽出は毎回 1. から全件やり直します。

## ELECOM の暗号化 USB メモリでの動き (想定)

| ドライブ | 判定 | 結果 |
|---|---|---|
| 仮想 CD-ROM (LUN 0) | 1. ディスクとして列挙されない。2. `DeviceType` が `FILE_DEVICE_CD_ROM` | 対象外 |
| 暗号化領域 (LUN 1)、ロック解除後 | 1. ディスクで VID:PID が一致。2. 番号が集合にある | **採用** |
| 暗号化領域 (LUN 1)、ロック解除前 | 下記を参照 | 機器による |

ロック解除前の暗号化領域は、製品によって「ディスク自体が見えない」か「メディアなしのリムーバブルディスクとして見える」かのどちらかになると考えられます。
後者の場合、空のカードリーダーと同じくドライブレターが付くことがあります。そのとき 2. の `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` が成功すれば採用され、失敗すれば対象外になります。採用されても、中身の読めない空のドライブとして見えるだけです。

## 採用前に確認すること

上の動きは一般的な暗号化 USB メモリの構成からの想定で、実機では確認していません。`usbinfo -f json` で次を確かめてください。

| 確認すること | 見る場所 | 期待する値 |
|---|---|---|
| 仮想 CD-ROM のレターが 2. で外れるか | `volumeInterfaces[]` の仮想 CD-ROM の行 | `deviceType` が `CD_ROM`。`extentDiskNumbers` が空ならなお安全 |
| 仮想 CD-ROM がディスクとして列挙されないか | `disks[]` の ELECOM の行 | 仮想 CD-ROM の `kind` が `CdRom` |
| ロック解除の前後で VID:PID が変わらないか | `-f lsusb` を解除の前後で比べる | 同じ。変わる製品では、ロック解除後の PID を登録する |
| ロック解除前に暗号化領域がどう見えるか | 解除前の `disks[]` と `volumeInterfaces[]` | ディスクが 2 つ見えるなら、`storage.scsiAddress` の LUN で `lun=` を検討する |
| シリアルを持つか、読めるか | `-f csv` の `usbSerial` | `(none)` なら個体単位の登録はできない。`(read failed)` なら接続し直して再確認する |
| フロッピーが 2. で外れるか | USB フロッピーをつないだときの `volumeInterfaces[]` | `extentDiskNumbers` が空 |

フロッピーの確認ができない場合でも、誤って一致するのは、フロッピーの番号と同じ番号のディスク (`Floppy0` なら `PhysicalDrive0`、通常はシステムディスク) が登録済みの USB 機器だったときだけです。

## 既存方式・新方式との違い

| ケース | 既存方式 | 新方式 | ホワイトリスト方式 |
|---|---|---|---|
| 登録した USB メモリ | 抽出 | 抽出 | 抽出 |
| 未登録の USB メモリ | 抽出 | 抽出 | 抽出しない |
| 登録した USB HDD/SSD (`DRIVE_FIXED`、UAS) | 抽出しない | 抽出 | 抽出 |
| 暗号化 USB の仮想 CD-ROM | 抽出しない (`DRIVE_CDROM`) | 抽出しない | 抽出しない |
| 内蔵扱い (`ExpectNoRemoval`) の登録機器 | `GetDriveType` 次第 | 抽出しない | 抽出 |
| 登録した機器と同じ VID:PID の別の個体 | 抽出 | 抽出 | 抽出 (`SERIAL` を書けば抽出しない) |

新方式にあってホワイトリスト方式にない条件:

| 条件 | ホワイトリスト方式で外した理由 |
|---|---|
| ディスクの取り外しポリシーが `ExpectNoRemoval` 以外 | 許可するかは VID:PID で決めるため |
| USB デバイスに `CM_DEVCAP_REMOVABLE` が立っている | 同上 |
| ボリュームの devnode がドライブのインターフェイス (`GUID_DEVINTERFACE_CDROM` / `FLOPPY`) を持たない | `DeviceType` とエクステントの確認で、光学ドライブとフロッピーが外れるため |

取り外しポリシーを見ないので、**登録した VID:PID と同じチップを使った内蔵機器 (ノート PC の内蔵カードリーダーなど) も抽出されます**。
カードリーダーや USB-SATA 変換のチップ (Realtek、Genesys Logic、JMicron、ASMedia など) は多くの製品で共通なので、これらは `SERIAL` 付きで個体単位に登録してください。

また、VID:PID とシリアルは機器自身が申告する値です。ファームウェアを書き換えられるコントローラーなら偽装できるので、ホワイトリストを不正な機器の排除に使う場合は、この限界を前提にしてください。

## 実装上の注意

- インスタンス ID は、CM API に渡す引数と、ドライバーキー名の比較 (`_wcsicmp`) にだけ使い、中身は解析しません。
- 1 つの USB デバイスに複数のディスクやパーティションがある場合、条件を満たすレターはすべて採用されます。1 台につき 1 レターに絞るなら、USB デバイス本体の `DevInst` でまとめてください。
- `namespace drivesel` の既存の関数のうち、`deviceId`、`interfaceDetail`、`getDeviceNumber` (`\\.\X:` にもそのまま使える)、`getU32`、`findUsbDevice` を流用できます。
- ハブへの IOCTL は、usbinfo 本体の `usbGetDescriptor()` / `usbGetString()` と、USB デバイスの情報を集める処理 (ポート番号の取得からドライバーキー名の照合まで) に実装例があります。`namespace drivesel` に移すときは、これらを持ち込みます。
- 新たに必要なのは、ホワイトリストの読み込み、ハブへの 3 つの IOCTL、`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`、`IOCTL_SCSI_GET_ADDRESS` です。
