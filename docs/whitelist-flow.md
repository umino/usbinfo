# ホワイトリスト方式のドライブ抽出フロー (設計メモ)

「リムーバブルな USB ストレージ」を OS の申告値 (取り外しポリシー、`GetDriveType` など) で判定する代わりに、許可する USB 機器を VID:PID (+ シリアル番号) で列挙しておき、それに一致した機器のドライブだけを抽出する方式の設計です。
出力は既存方式・新方式と同じ `DriveEntry { driveIndex, devInst }` のリストとし、`namespace drivesel` に `SelectDrivesWhitelist()` として並べる想定です。

## 基本の考え方: 「どの機器か」と「どのドライブか」を分けて判定する

ELECOM の暗号化 USB メモリのように、1 台の USB デバイスが仮想 CD-ROM (ロック解除ソフト入り) とマスストレージの 2 つのドライブを持つ機器では、2 つのドライブが同じ USB デバイスの下にぶら下がります。
VID:PID もシリアル番号も USB デバイス単位の値なので、2 つのドライブで同じ値になり、ホワイトリストでは区別できません。

```
USB\VID_056E&PID_xxxx\<シリアル>        ← USB デバイス本体。VID:PID とシリアルはここにある
 └ USBSTOR\CdRom&Ven_ELECOM&...\...&0   ← LUN 0: 仮想 CD-ROM   (GUID_DEVINTERFACE_CDROM)
 └ USBSTOR\Disk&Ven_ELECOM&...\...&1    ← LUN 1: マスストレージ (GUID_DEVINTERFACE_DISK)
```

そこで、判定を 2 段に分けます。

| 段 | 何を決めるか | 使う値 |
|---|---|---|
| 1. ドライブの種類 | そのドライブが「ディスク」か | ディスクのインターフェイス (`GUID_DEVINTERFACE_DISK`) と `IOCTL_STORAGE_GET_DEVICE_NUMBER` の `DeviceType` |
| 2. 機器 | そのディスクが載っている USB デバイスが許可されているか | USB デバイス本体のインスタンス ID (VID:PID、シリアル) |

仮想 CD-ROM は 1 段目で外れるので、ホワイトリストには VID:PID を 1 行書くだけで済みます。
1 台に**ディスクが 2 つ以上**ある機器 (公開領域 + 暗号化領域が両方ディスクとして見える機器、複数スロットのカードリーダーなど) を区別したいときだけ、エントリに LUN を指定します (後述)。

1 段目で使うのは「ディスクか光学ドライブか」という、OS が昔から変えていない区別だけです。
新方式の取り外しポリシー (`DEVPKEY_Device_RemovalPolicy`) や `CM_DEVCAP_REMOVABLE` のような、ドライバーや ACPI の記述で変わりうる値は使いません。

## ホワイトリストの形式

```
# VID:PID[:SERIAL][;lun=N]      # 以降はコメント。VID / PID は 16 進 4 桁、大文字小文字は区別しない
0781:5583                        # 機種単位 (この機種ならどの個体でも許可)
0781:5583:4C530001234567891234   # 個体単位 (このシリアルの個体だけ許可)
056E:xxxx;lun=1                  # 1 台に複数ディスクがある機器で、LUN 1 だけ許可
```

| フィールド | 省略時 | 照合先 |
|---|---|---|
| `VID:PID` | 省略不可 | USB デバイス本体のインスタンス ID の `USB\VID_vvvv&PID_pppp` |
| `SERIAL` | 機種単位で照合 | USB デバイス本体のインスタンス ID の 3 番目の要素 (`CM_DEVCAP_UNIQUEID` が立っているときだけ) |
| `lun=N` | LUN を問わない | ディスクの `SCSI_ADDRESS.Lun` |

`SERIAL` には、USB の文字列ディスクリプタの値ではなく、**インスタンス ID に入っている値**を登録します。
Windows はシリアル番号に使えない文字 (空白、`,`、0x20 未満や 0x7F 超の文字) を `_` に置き換えてインスタンス ID にするため、ディスクリプタの値と一致しないことがあります。
登録する値は `usbinfo -f json` の `usbDevices[].devnode.instanceId` で確認できます。

## フロー

### 0. ホワイトリストを読み込む

- 各行を `VID`、`PID`、`SERIAL` (なしは空)、`LUN` (なしは -1) に分解して保持する。
- VID / PID は数値 (`USHORT`) で比較する。文字列で比べると `056e` と `056E` の扱いを誤りやすい。
- 書式の誤った行はエラーにする (黙って無視すると、許可したつもりの機器が抽出されない原因が分からなくなる)。

### 1. ドライブレターとボリューム GUID の表を作る

| API | 主な引数 | 判定 |
|---|---|---|
| `GetVolumeNameForVolumeMountPointW` | `L"A:\\"` 〜 `L"Z:\\"` | 成功したレターだけ、(ドライブ index, `\\?\Volume{GUID}\`) を控える |

`GetDriveType` では絞りません (USB の HDD/SSD は `DRIVE_FIXED` になるため)。

### 2. ディスク番号 → ディスクの表を作る

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` | `&GUID_DEVINTERFACE_DISK`, `nullptr`, `nullptr`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | |
| `SetupDiEnumDeviceInterfaces` | 同じ GUID、index を 0 から増やす | `FALSE` で終了 |
| `SetupDiGetDeviceInterfaceDetailW` | 2 段階呼び出し、`SP_DEVINFO_DATA` も受け取る | `DevicePath` とディスクの `DevInst` を得る |
| `CreateFileW` | `DevicePath`, アクセス `0`, `FILE_SHARE_READ \| FILE_SHARE_WRITE`, `OPEN_EXISTING` | アクセス 0 なので管理者権限は不要 |
| `DeviceIoControl` | `IOCTL_STORAGE_GET_DEVICE_NUMBER` → `STORAGE_DEVICE_NUMBER` | `DeviceType == FILE_DEVICE_DISK` のものだけ表に入れる |
| `DeviceIoControl` | `IOCTL_SCSI_GET_ADDRESS` → `SCSI_ADDRESS` | ホワイトリストに `lun=` があるときだけ必要。`Lun` を表に入れる。失敗したら LUN 不明 (-1) |

表のキーは `DeviceNumber`、値は (ディスクの `DevInst`, `Lun`) です。
仮想 CD-ROM は `GUID_DEVINTERFACE_CDROM` で列挙されるので、この表には入りません。

### 3. ボリュームを列挙し、ディスクに対応付ける

| API | 主な引数 | 判定 |
|---|---|---|
| `SetupDiGetClassDevsW` | `&GUID_DEVINTERFACE_VOLUME`, `nullptr`, `nullptr`, `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE` | |
| `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetailW` | 2. と同じ | `DevicePath` とボリュームの `DevInst` (= `DriveEntry.devInst`) を得る |
| `GetVolumeNameForVolumeMountPointW` | `DevicePath + L"\\"` | 1. の表と GUID を `_wcsicmp` で比べ、一致するレターがなければ対象外 |
| `CM_Get_Device_IDW` | ボリュームの `DevInst` | ID が `STORAGE\Volume\` で始まらなければ対象外 (`_wcsnicmp`)。**仮想 CD-ROM とフロッピーはここで外れる** |
| `CreateFileW` + `DeviceIoControl` | `DevicePath` に `IOCTL_STORAGE_GET_DEVICE_NUMBER` | 失敗 (複数ディスクにまたがるボリューム) なら対象外。`DeviceType != FILE_DEVICE_DISK` なら対象外 |
| (表引き) | `DeviceNumber` で 2. の表を引く | 見つからなければ対象外 |

仮想 CD-ROM のボリュームは、インターフェイスを持つ devnode がドライブ自身 (`USBSTOR\CdRom&...`) で、`DeviceType` も `FILE_DEVICE_CD_ROM` です。
ID の形と `DeviceType` の 2 か所で外れるので、どちらかの振る舞いが将来変わっても、2. の表に載らない限り抽出されません。

フロッピーは `DeviceType` が `FILE_DEVICE_DISK` になり、番号が `PhysicalDrive` の番号と衝突するため、`STORAGE\Volume\` の確認は省略できません ([README](../README.md) の「volumeInterfaces[] の各項目」を参照)。

### 4. ディスクから USB デバイス本体を探す

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_Parent` | ディスクの `DevInst` から順に親へ | `CR_SUCCESS` でなくなったら USB でない (対象外) |
| `CM_Get_Device_IDW` | 親の `DevInst` | 親の ID を得る |
| `CM_Get_Device_Interface_List_SizeW` | `&GUID_DEVINTERFACE_USB_HUB`, 親の ID, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | 返った長さが 1 より大きければ親は USB ハブ。そのとき**子のほう**が USB デバイス本体 |

既存コードの `findUsbDevice()` と同じ処理です。
BOT (`USBSTOR\Disk&...`)、UAS (`SCSI\Disk&...`)、複合デバイス (`USB\VID_...&MI_00\...` を経由する機器)、ハブ経由のいずれでも、ハブ直下のノードは `USB\VID_vvvv&PID_pppp\...` になります。
複合デバイスで止まるのは `&MI_xx` の付いたインターフェイスのノードではなく、その親のデバイス本体なので、VID:PID とシリアルを正しく取れます。

### 5. ホワイトリストと照合する

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_Device_IDW` | USB デバイス本体の `DevInst` | `USB\VID_vvvv&PID_pppp\<3 番目の要素>` を `\` で 3 つに分け、2 番目から VID / PID を 16 進で読む。形が違えば対象外 |
| `CM_Get_DevNode_PropertyW` | USB デバイス本体, `&DEVPKEY_Device_Capabilities` (`DEVPROP_TYPE_UINT32`) | `CM_DEVCAP_UNIQUEID` が立っていれば、3 番目の要素をシリアルとして使う。立っていなければシリアルなし |

各エントリについて、次をすべて満たせば一致とします。どれか 1 つのエントリに一致したら採用です。

1. VID と PID が一致する。
2. エントリに `SERIAL` があるとき: シリアルありで、`_wcsicmp` で一致する。**シリアルなしの機器は、`SERIAL` 付きのエントリには一致させない。**
3. エントリに `lun=` があるとき: 2. で得た `Lun` が一致する。LUN 不明 (-1) なら一致させない。

`CM_DEVCAP_UNIQUEID` が立っていない機器の 3 番目の要素は、`5&2c3a1b&0&3` のような、接続ポートから作られた値です。これをシリアルとして比べると、同じポートにつないだ別の個体と一致しうるので使いません。
Windows の設定 (`usbflags` の `IgnoreHWSerNum`) でシリアルを無視するよう指定された機器も、同じ扱いになります。

一致したボリュームについて、1. の表で一致したドライブレターすべてを `DriveEntry { driveIndex, devInst(ボリューム) }` として返します。

### 6. 再評価のタイミング

暗号化 USB メモリは、ロック解除前は暗号化領域のディスクがメディアなしの状態で、ボリュームがありません。この時点では 3. でレターが見つからず、抽出されません。
ロック解除でボリュームが現れた時点で抽出対象になるので、抽出は接続時に 1 回ではなく、ボリュームの到着ごとにやり直します。

| API | 主な引数 | 判定 |
|---|---|---|
| `RegisterDeviceNotificationW` | `DEV_BROADCAST_DEVICEINTERFACE_W` (`dbcc_classguid = GUID_DEVINTERFACE_VOLUME`), `DEVICE_NOTIFY_WINDOW_HANDLE` | `WM_DEVICECHANGE` の `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE` で 1. からやり直す |

ウィンドウを持たないプロセスでは `CM_Register_Notification` (`CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE`、同じ GUID) を使えます。
既存コードが `WM_DEVICECHANGE` の `DBT_DEVTYP_VOLUME` (ドライブレターのビットマスク) で再抽出しているなら、それをそのまま使ってもかまいません。通知の種類によらず、抽出は毎回 1. から全件やり直します。

## ELECOM の暗号化 USB メモリでの動き (想定)

| ドライブ | 外れる / 採用される段 | 結果 |
|---|---|---|
| 仮想 CD-ROM (LUN 0) | 3. ボリュームの ID が `USBSTOR\CdRom&...` で、`DeviceType` も `FILE_DEVICE_CD_ROM` | 対象外 |
| 暗号化領域 (LUN 1)、ロック解除前 | 3. ボリュームがないのでレターがない | 対象外 |
| 暗号化領域 (LUN 1)、ロック解除後 | 3.〜5. をすべて通り、VID:PID が一致 | **採用** |

上の構成は一般的な暗号化 USB メモリの形からの想定で、実機では確認していません。採用前に次を `usbinfo` で確かめてください。

- **ロック解除の前後で VID:PID が変わらないか。** 製品によっては、ロック解除時に USB デバイスとして再接続し、別の PID で現れるものがあります。その場合はロック解除後の PID を登録します。`-f lsusb` を解除の前後で比べれば分かります。
- **ロック解除前の暗号化領域がディスクとして見えるか、見えないか。** どちらでも抽出結果は変わりませんが、ディスクが 2 つ見えるなら `lun=` の検討が必要です (`disks[]` の `kind` と `storage.scsiAddress`)。
- **シリアルを持つか。** `usbDevices[].devnode.capUniqueID` が `false` なら、個体単位の登録はできません。

## 既存方式・新方式との違い

| ケース | 既存方式 | 新方式 | ホワイトリスト方式 |
|---|---|---|---|
| 登録した USB メモリ | 抽出 | 抽出 | 抽出 |
| 未登録の USB メモリ | 抽出 | 抽出 | 抽出しない |
| 登録した USB HDD/SSD (`DRIVE_FIXED`、UAS) | 抽出しない | 抽出 | 抽出 |
| 暗号化 USB の仮想 CD-ROM | 抽出しない (`DRIVE_CDROM`) | 抽出しない | 抽出しない |
| 取り外しポリシーが `ExpectNoRemoval` と申告される登録機器 | (`GetDriveType` 次第) | 抽出しない | 抽出 |
| 同じ VID:PID の別の個体 | 抽出 | 抽出 | `SERIAL` を書けば抽出しない |

ホワイトリスト方式が OS の申告値に頼るのは、3. の「ディスクのボリュームか」と 4. の「親をたどって USB ハブに着くか」だけです。
取り外しポリシーなどに頼らない代わりに、**登録していない機器は、正当な USB ストレージでも抽出されません**。新しい機器を使い始めるたびに登録が必要になる点は、運用で吸収する前提です。

## 実装上の注意

- インスタンス ID の比較はすべて大文字小文字を区別しない (`_wcsicmp` / `_wcsnicmp`)。`CM_Get_Device_ID` は大文字、`DEVPKEY_Device_InstanceId` は元の表記で返ることを確認しています ([findings.md](findings.md))。
- 1 つの USB デバイスに複数のディスクやパーティションがある場合、条件を満たすボリュームはすべて採用されます。1 台につき 1 レターに絞るなら、`DriveEntry` を返した後に USB デバイス本体の `DevInst` でまとめてください。
- 2.〜5. で使う関数 (`interfaceDetail`、`getDeviceNumber`、`findUsbDevice`、`getU32`) は、`namespace drivesel` に既にあるものをそのまま使えます。新たに必要なのは、ホワイトリストの読み込み、インスタンス ID からの VID / PID / シリアルの切り出し、`IOCTL_SCSI_GET_ADDRESS` の 3 つです。
