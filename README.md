# usbinfo

接続中の USB デバイスとディスクについて、Windows から取得できる情報をまとめてダンプするコンソールツールです。
「リムーバブルな USB ストレージだけを対象にしたい」といった要件に対して、どの値で判定できるかを実機で比較するための調査用です。

- 対象 OS: Windows 10 以降
- 管理者権限: 不要
- 出力: テキスト / JSON / CSV (いずれも UTF-8)

USB の仕様に詳しくない人向けに、動作のしくみを図で説明したページを [docs/how-it-works.html](docs/how-it-works.html) に置いています (ブラウザで開いてください)。

## ビルド

Visual Studio (C++ デスクトップ開発ワークロード) で `usbinfo.vcxproj` を開いてビルドするか、MSBuild を使います。

```
msbuild usbinfo.vcxproj -p:Configuration=Release -p:Platform=x64
```

出力先は `bin\x64\Release\usbinfo.exe` です。CRT を静的リンクしているので、exe 単体で調査対象の PC に持ち込めます。

## 使い方

```
usbinfo [options]
```

| オプション | 意味 |
|---|---|
| `-f`, `--format FORMAT` | 出力形式。既定は `text`。下の表を参照 |
| `-o`, `--out FILE` | 標準出力の代わりに FILE へ書き出す。CSV のときは Excel で開けるよう BOM を付ける |
| `--no-wmi` | WMI の問い合わせを省略する (実行が速くなる) |
| `--no-props` | devnode の全プロパティダンプ (`allProperties`) を省略する |
| `--exclude-internal-usb` | 新方式のドライブ抽出で、`CM_DEVCAP_REMOVABLE` が立っていない USB デバイス (内蔵扱い) も除外する |
| `--vidpid` | `-f code` の各行に VID:PID を付ける |
| `--decode [FILE]` | 手で打ち直した `-f code` の内容を FILE または標準入力から読み、写し間違いを検査して展開する |
| `-h`, `--help` | ヘルプを表示する |

| FORMAT | 内容 |
|---|---|
| `text` | 全情報をインデント付きテキストで出力する |
| `json` | `text` と同じ内容の JSON |
| `csv` | ディスク 1 台 1 行の比較表 (`summary`) だけを出力する |
| `lsusb` | USB デバイスを 1 台 1 行で一覧する (Linux の `lsusb` 風) |
| `lsusb-tree` | USB の接続ツリーを表示する (Linux の `lsusb -tv` 風) |
| `letters` | ドライブレターごとに、各方式で抽出されるかを並べる (「ドライブレターの抽出」を参照) |
| `code` | 比較結果を、手で写しやすいチェック値付きの短いコードで出力する ([docs/manual-transcription.md](docs/manual-transcription.md) を参照) |

例:

```
usbinfo -f text -o result.txt     全情報
usbinfo -f json -o result.json    同じ内容の JSON
usbinfo -f csv  -o summary.csv    比較表のみ
usbinfo -f lsusb                  USB デバイスの一覧
usbinfo -f lsusb-tree             USB の接続ツリー
```

## lsusb 風の表示

`-f lsusb` は USB デバイスを 1 台 1 行で表示します。ルートハブと外付けハブも含みます。

```
Bus 001 Device 001: ID 8086:7a60 USB ルート ハブ (USB 3.0)
Bus 001 Device 002: ID 046d:c534 Logitech USB Receiver
Bus 001 Device 004: ID 05e3:0608 USB2.0 Hub
```

`-f lsusb-tree` はハブとポートの接続関係をツリーで表示します。

```
/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=USBHUB3/26p
    ID 8086:7a60 USB ルート ハブ (USB 3.0)
    |__ Port 005: Dev 002, Class=HID, Driver=usbccgp, 12M
        ID 046d:c534 Logitech USB Receiver
    |__ Port 011: Dev 004, Class=Hub, Driver=USBHUB3/4p, 480M
        ID 05e3:0608 USB2.0 Hub
```

| 項目 | 意味 |
|---|---|
| `Bus` | ルートハブごとに振った連番。Windows にはバス番号がないため、ルートハブのインスタンス ID 順に 1 から付けている |
| `Device` / `Dev` | USB デバイスアドレス。ルートハブは 001 固定 |
| `ID` | ベンダー ID : プロダクト ID。ルートハブはホストコントローラーの ID |
| 名前 | 文字列ディスクリプタの製造元 + 製品名。読めないときはデバイスマネージャー上の名前 |
| `Port` | 親ハブ上のポート番号 |
| `Class` | デバイスクラス。インターフェイスごとに定義されている機器は、インターフェイスクラスを列挙する |
| `Driver` | 割り当てられたドライバーサービス名。ハブは `/Np` でポート数を付ける |
| 速度 | 現在の接続速度 (`1.5M` / `12M` / `480M` / `5000M` / `10000M`) |

Linux の `lsusb` との違い:

- 名前は機器自身が持つ文字列で、`usb.ids` データベースは引いていません。
- xHCI では USB 2.0 と 3.x が 1 つのルートハブにまとまるため、バスは速度別に分かれません。
- Windows のデバイスアドレスは 1 から始まることがあり、ルートハブの 001 と重複する場合があります。
- ツリーはデバイス単位の表示で、インターフェイスごとの行 (`If N`) は出しません。

出力には PC 名やデバイスのシリアル番号が含まれます。共有するときは注意してください。

## 出力の構造

| キー | 内容 |
|---|---|
| `tool` / `generatedAt` / `computer` / `os` | ツール名、実行日時、PC 名、OS ビルド |
| `summary[]` | 判定に効く項目だけを並べた比較表。ディスク 1 台につき 1 行 |
| `disks[]` | 全ディスクと光学ドライブの詳細。比較のため USB 以外も出力する |
| `usbDevices[]` | 全 USB デバイスの詳細。ストレージ以外も含む |
| `wpdDevices[]` | デバイスマネージャーの「ポータブル デバイス」(WPD) に出ている全デバイスと、対応するディスク |
| `volumeInterfaces[]` | `GUID_DEVINTERFACE_VOLUME` で列挙した全ボリュームと、対応するディスクの判定結果 |
| `driveLetters[]` | ドライブレターごとの既存方式 / 新方式の抽出結果と、その根拠 (「ドライブレターの抽出」を参照) |

## summary の各列

`-f csv` で出力される列です。まずここを見比べるのが早道です。

| 列 | 意味 | 取得元 |
|---|---|---|
| `kind` | `Disk`、`CdRom`、`Floppy` のいずれか | 列挙したデバイスインターフェイス |
| `number` | 物理ドライブ番号 (`\\.\PhysicalDriveN` の N)。光学ドライブとフロッピーはそれぞれ別の通し番号 (`CdRomN`、`FloppyN`) | `IOCTL_STORAGE_GET_DEVICE_NUMBER` |
| `model` | ベンダー名 + 製品名 | `STORAGE_DEVICE_DESCRIPTOR` |
| `sizeGB` | 容量 (10 進 GB)。メディアなしのときは空 | `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` |
| `mounts` | ドライブレターやマウントポイント | ボリューム API |
| `driveType` | `GetDriveType` の結果 (`REMOVABLE` / `FIXED` / `CDROM` など) | `GetDriveType` |
| `busType` | ストレージドライバーが申告するバス種別 (`Usb` / `Nvme` / `Sata` / `Sd` など) | `STORAGE_DEVICE_DESCRIPTOR.BusType` |
| `removableMedia` | メディアが取り外せる機器か (SCSI INQUIRY の RMB ビット) | `STORAGE_DEVICE_DESCRIPTOR.RemovableMedia` |
| `mediaPresent` | メディアが入っているか | `IOCTL_STORAGE_CHECK_VERIFY2` |
| `deviceHotplug` | デバイスがホットプラグ扱いか | `IOCTL_STORAGE_GET_HOTPLUG_INFO` |
| `diskRemovalPolicy` | ディスク devnode の取り外しポリシー | `DEVPKEY_Device_RemovalPolicy` |
| `diskSafeRemovalRequired` | 「ハードウェアの安全な取り外し」が必要とされているか | `DEVPKEY_Device_SafeRemovalRequired` |
| `usbAncestor` | devnode の祖先に USB ハブ直下のノードがあるか (= USB 経由で接続されているか) | `CM_Get_Parent` |
| `usbVidPid` | USB のベンダー ID : プロダクト ID | USB デバイスディスクリプタ |
| `usbSerial` | USB のシリアル番号文字列。持たない機器は `(none)` | USB 文字列ディスクリプタ |
| `usbProduct` | USB の製品名文字列 | USB 文字列ディスクリプタ |
| `usbSpeed` | 現在の接続速度 (`Low` / `Full` / `High` / `Super` / `SuperPlus`) | ハブ IOCTL |
| `usbStorageDriver` | 実際に使われているストレージドライバー (`USBSTOR` = BOT、`UASPStor` = UAS など) | `DEVPKEY_Device_Service` |
| `usbCapRemovable` | USB デバイス本体が取り外し可能とされているか | `CM_DEVCAP_REMOVABLE` |
| `usbInLocalMachineContainer` | PC 本体と同じコンテナーに属するか。`true` は内蔵扱い | `DEVPKEY_Device_InLocalMachineContainer` |
| `usbPortUserConnectable` | 接続先ポートがユーザーの手の届くポートか | `IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES` |
| `wpdDevice` | 対応する WPD デバイスの名前。なければ `(none)` | WPD クラスの devnode |
| `verdict` | このツールの判定結果 (後述) | 上記から算出 |
| `instanceId` | ディスク devnode のデバイスインスタンス ID | Configuration Manager |
| `volumeInstanceIds` | このディスク上のボリュームのインターフェイス (`GUID_DEVINTERFACE_VOLUME`) を持つ devnode のインスタンス ID。複数あれば ` \| ` 区切り | `CM_Get_Device_Interface_Property` |

`true` / `false` 以外に、取得できなかった項目は `(n/a)` や `?` になります。USB 経由でないディスクでは `usb*` の列は空です。

### verdict の値

| 値 | 条件 |
|---|---|
| `USB_REMOVABLE_STORAGE` | `kind` が `Disk`、USB 経由 (`usbAncestor`)、かつ `diskRemovalPolicy` が `ExpectNoRemoval` 以外 |
| `USB_OPTICAL_DRIVE(excluded)` | USB 経由の光学ドライブ (BD / DVD / CD)。ストレージの対象から外す |
| `USB_FLOPPY_DRIVE` | USB 経由のフロッピードライブ。対象にするかは要件次第なので、ほかと分けている |
| `USB_NON_REMOVABLE(internal?)` | USB 経由だが取り外し不可扱い。内蔵カードリーダーなどの可能性 |
| `BUSTYPE_USB_ONLY(check)` | devnode の祖先に USB は見つからないが `busType` は `Usb`。要確認 |
| `NOT_USB` | USB 経由ではない |

## disks[] の各項目

### 基本

| 項目 | 意味 |
|---|---|
| `kind` | `Disk` (`GUID_DEVINTERFACE_DISK`)、`CdRom` (`GUID_DEVINTERFACE_CDROM`)、`Floppy` (`GUID_DEVINTERFACE_FLOPPY`) のいずれか |
| `interfacePath` | デバイスインターフェイスのパス。`CreateFile` に渡せる |
| `instanceId` | ディスク devnode のデバイスインスタンス ID |

### storage (ストレージ IOCTL)

デバイスをアクセス権 0 で開いて取得しています。失敗した IOCTL は `<名前>_error` に Win32 エラーが入ります。

| 項目 | 意味 |
|---|---|
| `deviceNumber.DeviceType` | `FILE_DEVICE_DISK` / `FILE_DEVICE_CD_ROM` |
| `deviceNumber.DeviceNumber` | 物理ドライブ番号 |
| `deviceDescriptor.BusType` | デバイスのバス種別。USB 接続なら通常 `7 (Usb)` |
| `deviceDescriptor.RemovableMedia` | メディア可換ビット。USB メモリやカードリーダーは `true`、USB HDD/SSD は通常 `false` |
| `deviceDescriptor.CommandQueueing` | コマンドキューイング対応 |
| `deviceDescriptor.DeviceType(SCSI)` | SCSI デバイス種別 (0x00 = ディスク、0x05 = CD/DVD) |
| `deviceDescriptor.VendorId` / `ProductId` / `ProductRevision` / `SerialNumber` | INQUIRY などから得た識別文字列。USB ブリッジが書き換えることがある |
| `adapterDescriptor.BusType` | アダプター (コントローラー) 側のバス種別 |
| `adapterDescriptor.MaximumTransferLength` ほか | アダプターの転送能力 |
| `hotplugInfo.MediaRemovable` | メディアが取り外せるか |
| `hotplugInfo.MediaHotplug` | ロックなしでメディアを抜けるか |
| `hotplugInfo.DeviceHotplug` | デバイス自体がホットプラグ扱いか。eSATA などでも `true` になり、設定で変わる |
| `hotplugInfo.WriteCacheEnableOverride` | 書き込みキャッシュの上書き設定 |
| `mediaPresent` | メディアの有無。空のカードリーダーは `false` |
| `geometry.MediaType` | `FixedMedia` / `RemovableMedia` |
| `geometry.DiskSize` / `BytesPerSector` | 容量 (バイト) とセクターサイズ |
| `accessAlignment` | 論理 / 物理セクターサイズ |
| `incursSeekPenalty` | シークペナルティの有無。`true` は HDD、`false` は SSD やフラッシュ |
| `trimEnabled` | TRIM 対応 |
| `deviceIdDescriptor.*` | デバイス識別子 (VPD ページ 0x83) の個数と生データ |
| `scsiAddress` | SCSI ポート / パス / ターゲット / LUN |
| `writable` | 書き込み可能か。ライトプロテクト時は `false` |
| `driveLayout.PartitionStyle` | `MBR` / `GPT` / `RAW` |
| `driveLayout.partitions[]` | 各パーティションの番号、オフセット、サイズ、種別 |

### volumes[] (このディスク上のボリューム)

| 項目 | 意味 |
|---|---|
| `volumeGuidPath` | `\\?\Volume{GUID}\` 形式のボリューム名 |
| `dosDevice` | NT デバイス名 (`\Device\HarddiskVolumeN`) |
| `mountPoints` | ドライブレターやマウント先フォルダー |
| `GetDriveType` | `DRIVE_REMOVABLE` / `DRIVE_FIXED` / `DRIVE_CDROM` など |
| `partitionNumber` | パーティション番号 |
| `fileSystem` / `label` / `volumeSerial` / `fileSystemFlags` | ファイルシステム情報 |
| `totalBytes` / `freeBytes` | 容量と空き |

`volumes[]` は `FindFirstVolume` で列挙したボリュームです。

### volumeInterfaces[] (このディスク上のボリュームのインターフェイス)

`GUID_DEVINTERFACE_VOLUME` で列挙したインターフェイスのうち、このディスクに対応するものです。項目はルートの `volumeInterfaces[]` と同じです (下記)。

### devnode / ancestors[] (デバイスツリー)

`devnode` はディスク自身、`ancestors[]` は親からルートまでの各ノードです。項目は共通です。

| 項目 | 意味 |
|---|---|
| `instanceId` | デバイスインスタンス ID。先頭が列挙子 (`USBSTOR\` / `SCSI\` / `USB\` など) |
| `name` / `deviceDesc` | デバイスマネージャーに表示される名前 |
| `busReportedDeviceDesc` | バスから報告された名前。USB デバイスでは製品名文字列 |
| `manufacturer` | 製造元 (INF 由来) |
| `class` | デバイスセットアップクラス (`DiskDrive` / `USB` / `SCSIAdapter` など) |
| `service` | 割り当てられたドライバーサービス名 |
| `enumerator` | このノードを列挙したバスドライバー |
| `locationInfo` / `locationPaths` | 物理的な接続位置。USB ではハブとポートの番号を含む |
| `address` | バス上のアドレス。USB デバイスではハブのポート番号 |
| `capabilities` | `CM_DEVCAP_*` フラグの生値と名前 |
| `capRemovable` | `CM_DEVCAP_REMOVABLE`。親から取り外せるデバイスか |
| `capSurpriseRemovalOK` | 「安全な取り外し」なしで抜いてよいか |
| `capUniqueID` | インスタンス ID がバス全体で一意か。USB ではシリアル番号を持つとき `true` |
| `removalPolicy` | `1 ExpectNoRemoval` / `2 ExpectOrderlyRemoval` / `3 ExpectSurpriseRemoval` |
| `containerId` | 同じ物理機器に属するノードをまとめる ID |
| `inLocalMachineContainer` | PC 本体のコンテナーに属するか。外付け機器は `false` |
| `safeRemovalRequired` | 安全な取り外しが必要とされているか |
| `devNodeStatus` / `statusRemovable(DN_REMOVABLE)` / `problemCode` | devnode の状態フラグと問題コード |
| `allProperties` | そのノードが持つ全 `DEVPKEY`。名前の分からないキーは `{GUID}[pid]` で表示 |

`allProperties` を出力するのは、ディスク自身と、USB デバイス本体の手前までの祖先です (USB 以外では直近の親のみ)。USB デバイス本体の分は `usbDevices[]` 側にあります。

### usb / usbStorageDriverService

USB 経由のディスクにだけ出力されます。`usb` は対応する USB デバイスの要約で、項目の意味は次節と同じです。
`usbStorageDriverService` はディスクに最も近い USB 列挙ノード (列挙子 `DEVPKEY_Device_EnumeratorName` が `USB` のノード) のサービス名で、転送方式の判別に使えます。

### wmi

`Win32_DiskDrive`、`MSFT_Disk`、`MSFT_PhysicalDisk` のうち、ドライブ番号が一致するオブジェクトの全プロパティ (値が NULL のものを除く) です。
よく参照される `InterfaceType`、`MediaType`、`BusType` は、上記の IOCTL の値を元にしたものです。

### wpd

このディスクに対応する WPD デバイス (インスタンス ID、名前、サービス) です。対応付けの方法は「wpdDevices[] の各項目」を参照してください。

### judgement (判定の内訳)

| 項目 | 意味 |
|---|---|
| `A_usbAncestorInDevnodeTree` | devnode の祖先に USB ハブ直下のノードがある |
| `B_busTypeIsUsb` | `BusType` が `Usb` |
| `C_diskRemovalPolicyExpectsRemoval` | ディスクの取り外しポリシーが `ExpectNoRemoval` 以外 |
| `D_usbDeviceCapRemovable` | USB デバイス本体に `CM_DEVCAP_REMOVABLE` が立っている |
| `E_usbDeviceOutsideLocalMachineContainer` | USB デバイスが PC 本体のコンテナーの外にある |
| `F_portIsUserConnectable` | 接続先ポートがユーザー接続可能 |
| `G_removableMediaBit` | メディア可換ビット |
| `H_deviceHotplug` | デバイスホットプラグ |
| `I_hasWpdDevice` | 対応する WPD デバイスがある (参考値。判定には使っていない) |
| `J_kindIsDisk` | 光学ドライブやフロッピーではなくディスクである |
| `verdict(A&&C&&J)` | A、C、J から求めた判定結果 |

## usbDevices[] の各項目

### devnode / descendants / allProperties

`devnode` は前述のデバイスツリーの項目と同じです。`descendants` は配下の子デバイスのツリー (インスタンス ID、名前、クラス、サービス) で、ストレージならディスクのノードがここに現れます。

### usb

親ハブに IOCTL を発行して取得しています。

| 項目 | 意味 |
|---|---|
| `hubInstanceId` / `hubInterfacePath` | 接続先ハブ |
| `portNumber` | ハブ上のポート番号 |
| `hub.hubType` | `UsbRootHub` / `Usb20Hub` / `Usb30Hub` |
| `hub.highestPortNumber` | ハブのポート数 |
| `hub.deviceRemovableBitForThisPort(1=non-removable)` | ハブディスクリプタの DeviceRemovable ビット。ルートハブでは内蔵機器でも 0 のことがあり、単独では当てにならない |
| `portConnectorProperties.PortIsUserConnectable` | ユーザーが抜き挿しできるポートか。内蔵機器用のポートは `false` |
| `portConnectorProperties.PortConnectorIsTypeC` | Type-C コネクターか |
| `portConnectorProperties.PortIsDebugCapable` / `PortHasMultipleCompanions` / `CompanionPortNumber` | デバッグ対応、コンパニオンポートの情報 |
| `connection.connectionStatus` | 接続状態 (`DeviceConnected` など) |
| `connection.deviceAddress` | USB バスアドレス |
| `connection.currentConfigurationValue` | 選択中のコンフィグレーション |
| `connection.deviceIsHub` | ハブかどうか |
| `connection.portSupportsUsb110` / `200` / `300` | ポートが対応するプロトコル |
| `connection.deviceIsOperatingAtSuperSpeedOrHigher` ほか | デバイスの SuperSpeed 対応と現在の動作状態 |
| `connection.speed` | 現在の接続速度 |
| `connection.driverKeyName` / `driverKeyMatchesDevnode` | ハブが報告するドライバーキーと、devnode 側との照合結果。`true` ならポートの対応付けが正しい |
| `deviceDescriptor.bcdUSB` | 準拠する USB 仕様のバージョン |
| `deviceDescriptor.bDeviceClass` / `SubClass` / `Protocol` | デバイスクラス。`0x00` はインターフェイスごとに定義 |
| `deviceDescriptor.idVendor` / `idProduct` | VID / PID |
| `deviceDescriptor.bcdDevice` | デバイスのリビジョン |
| `deviceDescriptor.manufacturerString` / `productString` / `serialNumberString` | 文字列ディスクリプタ。サスペンド中などで読めないときは `(read failed)` |
| `configurationDescriptor.descriptors[]` | コンフィグレーション、インターフェイス、エンドポイントの各ディスクリプタと生データ |
| `bosDescriptorRaw` | BOS ディスクリプタの生データ (USB 2.01 以降) |

インターフェイスの `classDescription` で、マスストレージなら転送方式が分かります。

| クラス / サブクラス / プロトコル | 意味 |
|---|---|
| `08 / 06 / 50` | マスストレージ、SCSI、BOT (Bulk-Only Transport) |
| `08 / 06 / 62` | マスストレージ、SCSI、UAS (USB Attached SCSI) |

UAS 対応機器は同じインターフェイスに BOT と UAS の両方の代替設定を持つので、実際にどちらで動いているかは `usbStorageDriverService` で確認します。

## wpdDevices[] の各項目

デバイスセットアップクラスが WPD (`{eec5ad98-8080-425f-922a-dabf3de3f69a}`) の devnode を全件出力します。
ストレージ由来のものだけでなく、スマートフォンやカメラなどの MTP 機器も含みます。

| 項目 | 意味 |
|---|---|
| `instanceId` ほか | デバイスツリーの項目と同じ。`name` はドライブレターやボリュームラベル、`service` は `WUDFWpdFs` (ストレージ) や `WUDFWpdMtp` (MTP) など |
| `volumeInstanceId` | 対応するボリューム devnode の ID |
| `volumeInterfacePath` | そのボリュームのデバイスインターフェイスのパス |
| `mappedDevice` | 対応するディスク番号とパーティション番号。MTP 機器などボリュームを持たないものは `(none ...)` |
| `allProperties` | そのノードが持つ全 `DEVPKEY` |

ストレージ由来の WPD デバイスのインスタンス ID は `SWD\WPDBUSENUM\<後半>` の形で、`<後半>` はボリューム devnode (`STORAGE\Volume\<後半>`) の ID と共通です。
このツールはそこからボリュームを開き、`IOCTL_STORAGE_GET_DEVICE_NUMBER` でディスク番号を得て対応付けています。

## volumeInterfaces[] の各項目

`GUID_DEVINTERFACE_VOLUME` で列挙した全ボリュームです。ボリュームを起点に判定する場合の見え方を確認するためのものです。

| 項目 | 意味 |
|---|---|
| `instanceId` | インターフェイスを持つ devnode のインスタンス ID (`CM_Get_Device_ID` で得られる値と同じ) |
| `interfacePath` | インターフェイスのパス。`CreateFile` に渡せる |
| `volumeGuidPath` | `\\?\Volume{GUID}\` 形式のボリューム名 |
| `mountPoints` | ドライブレターやマウント先フォルダー |
| `deviceType` / `deviceNumber` / `partitionNumber` | `IOCTL_STORAGE_GET_DEVICE_NUMBER` の結果。`deviceType` は `DISK` / `CD_ROM` など |
| `extentDiskNumbers` | `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` で得たディスク番号。複数のディスクにまたがるボリュームでは複数になる |
| `mappedDisk` | 対応するディスク (`Disk` / `CdRom`、番号、モデル)。見つからなければ `(none)` |
| `verdict` | 対応するディスクの `verdict`。ディスクがなければ `(no disk)` |

`instanceId` の形はボリュームの種類で異なります。

| ボリューム | `instanceId` の例 |
|---|---|
| 固定ディスク扱いのディスク上のボリューム | `STORAGE\Volume\{GUID}#<パーティション開始オフセット>` |
| リムーバブルメディア扱いのディスク (USB メモリなど) 上のボリューム | `STORAGE\Volume\_??_USBSTOR#Disk&...#{GUID}` (ディスクのインターフェイスパスを埋め込んだ形) |
| 光学ドライブ、フロッピー | ドライブ自身の devnode (`USBSTOR\CdRom&...`、`USBSTOR\SFloppy&...` など) |

`STORAGE\Volume\...` の devnode の親をたどると、ボリュームマネージャー (`ROOT\volmgr\0000`) に行き着き、USB デバイスには届きません。
そのため、このツールは次のようにボリュームとドライブを対応付けています。

- **光学ドライブ、フロッピーのボリューム**: インターフェイスを持つ devnode が、それ自身で `GUID_DEVINTERFACE_CDROM` か `GUID_DEVINTERFACE_FLOPPY` も登録している (= ドライブ自身がボリュームを兼ねている)。インスタンス ID の一致でドライブと対応付ける。
- **それ以外 (ディスク上のボリューム)**: ボリュームを開いて `IOCTL_STORAGE_GET_DEVICE_NUMBER` でディスク番号を得て、`GUID_DEVINTERFACE_DISK` のディスクと番号で対応付ける。

どちらに当たるかは、ID の文字列 (`STORAGE\Volume\` で始まるか) ではなく、登録しているインターフェイスで判定しています。ID の形は Microsoft が公開している仕様ではないためです ([docs/instance-id.md](docs/instance-id.md) を参照)。

フロッピーを番号で対応付けてはいけない理由は、フロッピーのボリュームに `IOCTL_STORAGE_GET_DEVICE_NUMBER` を発行すると、`DeviceType` が `FILE_DEVICE_DISK`、`DeviceNumber` がフロッピーの通し番号 (`\Device\Floppy0` なら 0) になるためです。
この番号はハードディスクの番号 (`PhysicalDrive0`) と区別がつかず、種類と番号だけで探すと `PhysicalDrive0` のディスクに誤って対応付けられます。

## ドライブレターの抽出 (既存方式・最小改修案・新方式・タスクトレイ方式の比較)

`usbinfo.cpp` の `namespace drivesel` に、「リムーバブルな USB ストレージのドライブレター」を抽出する関数を 4 つ用意しています。
どれも結果は `DriveEntry { int driveIndex; DEVINST devInst; }` のリストで、既存コードの出力 (ドライブレター index と devinst の組) と同じ形です。
`devInst` は `GUID_DEVINTERFACE_VOLUME` を SetupDi で列挙したときの `SP_DEVINFO_DATA.DevInst` です。
この namespace は usbinfo の他の部分に依存しないので、そのまま移植できます (`setupapi.lib` と `cfgmgr32.lib` が必要)。

| 関数 | 内容 |
|---|---|
| `SelectDrivesLegacy()` | 既存方式の再現。`GetDriveType` が `DRIVE_REMOVABLE` のドライブのうち、ボリュームの devnode の ID が `USBSTOR#` を含むか、親の ID が `USBSTOR\` を含むもの |
| `SelectDrivesLegacyMinimal()` | 最小改修案。既存方式に 3 点だけ変更を加えたもの: `DRIVE_FIXED` も候補に残す、ID の確認にディスクのバス種別が `Usb` かを OR で加える、フロッピー (`GUID_DEVINTERFACE_FLOPPY` を登録したボリュームの devnode) を外す。既存方式で抽出していたものは、すべて抽出する ([docs/legacy-minimal-flow.md](docs/legacy-minimal-flow.md)) |
| `SelectDrivesNew(excludeInternalUsb)` | 新方式。下記の条件をすべて満たすもの |
| `SelectDrivesSafeRemoval(includeNonUsb)` | タスクトレイ方式。「ハードウェアを安全に取り外してメディアを取り出す」に出るものと同じ基準 (`DEVPKEY_Device_SafeRemovalRequired`) で抽出する。新方式の 1.〜3. のあと、ディスク自身か祖先のどれかで `DEVPKEY_Device_SafeRemovalRequired` が TRUE のもの。`includeNonUsb` が false なら 4. (USB 経由) も満たすもの。既定値は定数 `kSafeRemovalIncludeNonUsb` (= `false`、USB のみ) |

新方式の条件:

1. ドライブレターを持つボリュームである (`GetDriveType` では絞らない)。
2. ボリュームの devnode が `GUID_DEVINTERFACE_CDROM` / `GUID_DEVINTERFACE_FLOPPY` を登録していない (= ドライブ自身がボリュームを兼ねる光学ドライブ・フロッピーではない)。光学ドライブとフロッピーはここで外れる。
3. ボリュームに `IOCTL_STORAGE_GET_DEVICE_NUMBER` を発行し、`DeviceType` が `FILE_DEVICE_DISK` で、同じ番号のディスク (`GUID_DEVINTERFACE_DISK`) がある。
4. ディスクの devnode から親をたどると、親が USB ハブのノード (USB デバイス本体) がある。
5. ディスクの devnode の取り外しポリシーが `ExpectNoRemoval` 以外である。
6. (`excludeInternalUsb` が true のときのみ) USB デバイス本体に `CM_DEVCAP_REMOVABLE` が立っている。

既存方式との違い:

| ケース | 既存方式 | 新方式 |
|---|---|---|
| USB メモリ、USB カードリーダー | 抽出する | 抽出する |
| USB の HDD/SSD (`DRIVE_FIXED`) | 抽出しない | 抽出する |
| UAS 接続の機器 (ID が `SCSI\Disk&...`) | 抽出しない | 抽出する |
| ノート PC の内蔵 USB カードリーダー | 抽出する | 取り外しポリシー次第。`excludeInternalUsb` で除外できる |

新方式の手順 (使う API、引数、判定) は [docs/new-method-flow.md](docs/new-method-flow.md) に、タスクトレイ方式の手順は [docs/safe-removal-flow.md](docs/safe-removal-flow.md) に、既存コードに数行の変更だけを加えて UAS の USB メモリと USB の HDD/SSD に対応する最小改修案は [docs/legacy-minimal-flow.md](docs/legacy-minimal-flow.md) に、判定で見ているインスタンス ID の成り立ちは [docs/instance-id.md](docs/instance-id.md) に、異なる GUID のインターフェイスが同じ devnode を指すケースの図解は [docs/device-interfaces.html](docs/device-interfaces.html) に、VID:PID (+ シリアル) で許可する機器を指定するホワイトリスト方式の設計は [docs/whitelist-flow.md](docs/whitelist-flow.md) にまとめています。

`-f letters` で 4 方式の結果を 1 ドライブ 1 行で比べられます。

```
Drv  DriveType  Legacy    Min       New       Safe      Diff         Bus        New: result                              Safe: node / result              Disk
C:   FIXED      -         -         -         -         same         Nvme       not USB                                  not USB                          CT1000P310SSD8
```

| 列 | 意味 |
|---|---|
| `Legacy` / `Min` / `New` / `Safe` | その方式で抽出されれば `selected`。`Min` は最小改修案、`Safe` はタスクトレイ方式 |
| `Diff` | 既存方式と新方式の比較。`same` (結果が同じ)、`LEGACY ONLY`、`NEW ONLY` |
| `Bus` | 最小改修案が見ているディスクのバス種別 (`Usb`、`Sata`、`Nvme` など。`?` は取得できない、または `GetDriveType` で対象外) |
| `New: result` | 新方式で抽出した、または除外した理由 |
| `Safe: node / result` | タスクトレイ方式で抽出したときは、`SafeRemovalRequired` が TRUE だったノードの名前 (タスクトレイの一覧に出る名前と見比べる)。除外したときはその理由 |
| `Disk` | ボリュームが載っているディスクの名前 |

`-f json` / `-f text` の `driveLetters[]` には、`legacyDetail` (既存方式が見ている各チェックの結果) と `newDetail` (ディスク番号、ディスクと USB デバイス本体のインスタンス ID、ディスクの列挙子 (`diskEnumerator`)、ハブから読んだ VID:PID (`usbVidPid`)、取り外しポリシー、`capRemovable`、`inLocalMachineContainer`)、`legacyMinimalDetail` (最小改修案の結果、バス種別、既存の ID の確認の結果)、`safeRemovalDetail` (タスクトレイ方式の結果、ディスク自身の `SafeRemovalRequired`、TRUE だったノードのインスタンス ID と名前) も出力します。
`-f code` には最小改修案の結果とバス種別を含めています (形式 `UI2`)。タスクトレイ方式の結果はまだ含めていません。

## 判定方法について

このツールは次の 3 条件の AND を「リムーバブルな USB ストレージ」としています。

1. **ディスクである**: `GUID_DEVINTERFACE_DISK` で列挙されたデバイスである。光学ドライブ (`GUID_DEVINTERFACE_CDROM`) は除く。
2. **USB 経由である**: ディスクの devnode から親をたどり、USB ハブ直下のノードに行き着く。
3. **取り外し可能である**: ディスク devnode の `DEVPKEY_Device_RemovalPolicy` が `ExpectNoRemoval` 以外。

ボリュームを起点にする場合は、ボリュームのインターフェイスで `IOCTL_STORAGE_GET_DEVICE_NUMBER` を発行し、`DeviceType` が `FILE_DEVICE_DISK` のものだけを残してから、ディスク番号で対応付けたディスクに 2、3 の条件を適用します。
光学ドライブは `DeviceType` が `FILE_DEVICE_CD_ROM` になるので、これで除外できます。
ただし、フロッピーは `DeviceType` が `FILE_DEVICE_DISK` になるので、この確認では除外できません。ボリュームの devnode が `GUID_DEVINTERFACE_CDROM` / `GUID_DEVINTERFACE_FLOPPY` を登録しているもの (ドライブ自身) は、その devnode 自身から親をたどり、それ以外だけをディスク番号で対応付けてください (「volumeInterfaces[] の各項目」を参照)。
ボリュームを持たないディスク (未フォーマットなど) は一覧に現れません。

WPD デバイスの有無 (`wpdDevice`) は判定には使わず、比較用に出力しています。WPD を判定に使う場合は次の点に注意してください。

- WPD デバイスは Portable Device Enumerator Service (`WPDBusEnum`) が作ります。このサービスが止められている環境では現れません。
- 接続から WPD デバイスが現れるまでに時間差があります。
- USB 以外 (内蔵カードリーダーなど) のボリュームや、ディスクではない MTP 機器も WPD に含まれます。

単独で判定に使うと誤りやすい値は次のとおりです。

| 値 | 問題点 |
|---|---|
| `GetDriveType == DRIVE_REMOVABLE`、`RemovableMedia` | メディアが抜けるという意味で、USB HDD/SSD は該当しない。USB 以外のカードリーダーは該当する |
| `BusType == Usb` | ベンダー製ドライバーでは別の値になることがあり、内蔵の USB 接続機器も含む |
| `DeviceHotplug` | eSATA などでも `true` になり、設定で変わる |
| WMI の `MediaType` / `InterfaceType` | 上記の値を元にしており、同じ弱点を持つ |

特定の機器だけを識別するには VID + PID + シリアル番号を使います。シリアル番号を持たない機器 (`capUniqueID` が `false`) は個体を区別できず、機種単位の識別になります。

実機での調査結果は [docs/findings.md](docs/findings.md) にまとめています。

## 注意

- 判定ロジックは、USB メモリ、USB HDD/SSD、カードリーダー (メディアあり / なし)、内蔵カードリーダーなど、手元の実機で `verdict` を確認してから採用してください。
- 内蔵か外付けかの情報は PC のファームウェア (ACPI) の記述に依存します。記述が不正確な機種では、内蔵機器が取り外し可能と報告されることがあります。
- スマートフォンなどの MTP 機器はディスクとして現れないため、`disks[]` には出ません (`usbDevices[]` には出ます)。
