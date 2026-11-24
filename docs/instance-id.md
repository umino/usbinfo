# デバイスインスタンス ID の成り立ち

usbinfo と、ドライブ抽出の各方式 ([new-method-flow.md](new-method-flow.md)、[whitelist-flow.md](whitelist-flow.md)) で見ているデバイスインスタンス ID が、どの部品からどう作られているかのまとめです。
各項目には、**Microsoft のドキュメントに書かれていること**と、**このプロジェクトの実機調査で観察しただけのこと**を分けて記します。

参照したドキュメントは 2026-11-24 に確認しました。リンク先は英語版です。

## 前提: ID は「解析せず、比較だけに使う」もの

Microsoft は、デバイス識別文字列について次のように注意しています。

> Device identification strings should not be parsed. They are meant only for string comparisons and should be treated as opaque strings.
>
> — [Device Identification Strings](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-identification-strings)

この文書で説明する ID の形は、ID の意味を理解したり調査結果を読んだりするためのものです。判定で ID の一部を切り出して使う場合は、それがドキュメントに書かれた形か、観察しただけの形かを確認してください (「このプロジェクトでの使い方」を参照)。

## 構成

デバイスインスタンス ID は、PnP マネージャーが devnode ごとに割り当てる、システム内で一意な文字列です。次の 2 つを `\` でつないだ形になります。

```
USB\VID_0781&PID_5583\4C530001234567891234
└──── デバイス ID ────┘└─── インスタンス ID ───┘
```

| 部品 | 作るもの | 内容 | 出典 |
|---|---|---|---|
| デバイス ID | バスドライバー (列挙子) | デバイスの種類を表す。ハードウェア ID と同じ書式で、1 つのデバイスに 1 つ | [Device ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-ids) |
| インスタンス ID | バスドライバー (列挙子) | 同じ種類のデバイスどうしを区別する。バスが対応していればシリアル番号、そうでなければ接続位置などの情報。`\` を含まない | [Instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids) |
| デバイスインスタンス ID | PnP マネージャー | 上の 2 つと、`DEVICE_CAPABILITIES.UniqueID` から作る。再起動しても変わらない | [Device instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids) |

デバイス ID は一般に `<列挙子>\<列挙子ごとのデバイス ID>` の形で、**先頭の要素が、その devnode を見つけた列挙子 (バスドライバー) の名前**です ([Hardware ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/hardware-ids))。
`USB\...` は USB ハブドライバー、`USBSTOR\...` は USB ストレージポートドライバー、`SCSI\...` は SCSI / Storport 系のポートドライバーが見つけた devnode です。

長さは、終端の NUL を除いて `MAX_DEVICE_ID_LEN` (200) 文字未満です ([Device instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids))。

### UniqueID とインスタンス ID

バスドライバーが報告する `DEVICE_CAPABILITIES.UniqueID` によって、PnP マネージャーの扱いが変わります ([Instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids)、[DEVICE_CAPABILITIES](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_capabilities))。

| UniqueID | 意味 | デバイスインスタンス ID |
|---|---|---|
| TRUE | インスタンス ID がシステム全体で一意 | デバイス ID + インスタンス ID がそのまま使われる |
| FALSE | インスタンス ID はそのバスの中でしか一意でない | PnP マネージャーがインスタンス ID を書き換えてから使う |

FALSE のときの書き換えの具体的な形はドキュメントにありません。一般には `5&2c3a1b&0&3` のように、`&` で区切った値が前に付いた形になります (このプロジェクトの調査では記録していません)。
usbinfo では `UniqueID` を `CM_DEVCAP_UNIQUEID` として `capUniqueID` に出力しています。

### 大文字・小文字

`CM_Get_Device_ID` は大文字 (`STORAGE\VOLUME\...`) で返し、`DEVPKEY_Device_InstanceId` は元の表記 (`STORAGE\Volume\...`) で返しました (観察、[findings.md](findings.md))。
比較するときは大文字小文字を区別しない (`_wcsicmp`) のが安全です。

## USB ストレージのデバイスツリーに現れる ID

USB メモリをつないだときのツリーと、各ノードの ID です (一部は形を示すための例)。

```
USB\ROOT_HUB30\...                                   USB ルートハブ
 └ USB\VID_0781&PID_5583\4C530001234567891234        USB デバイス本体
    └ USBSTOR\Disk&Ven_SanDisk&Prod_...&Rev_1.00\...&0   ディスク (BOT の場合)
ROOT\volmgr\0000                                     ボリュームマネージャー
 └ STORAGE\Volume\_??_USBSTOR#Disk&Ven_...#{53f56307-...}  ボリューム
SWD\WPDBUSENUM\_??_USBSTOR#Disk&Ven_...#{53f56307-...}     WPD デバイス (ポータブル デバイス)
```

ボリュームと WPD デバイスは、ディスクの下ではなく別の枝にあります。

### USB デバイス本体: `USB\VID_vvvv&PID_pppp\<インスタンス ID>`

**ドキュメントの記述** ([Standard USB Identifiers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/standard-usb-identifiers)、[Enumeration of the composite parent device](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device)):

- USB ハブドライバーが、デバイスディスクリプタの `idVendor`、`idProduct` (、`bcdDevice`) から作る。それぞれ 16 進 4 桁。
- デバイス ID は、Standard USB Identifiers では `USB\VID_v(4)&PID_d(4)&REV_r(4)`、Enumeration of the composite parent device では `USB\VID_xxxx&PID_yyyy` と書かれている。
- ハードウェア ID には `USB\VID_v(4)&PID_d(4)&REV_r(4)` と `USB\VID_v(4)&PID_d(4)` がある。

**観察**: デバイスインスタンス ID の前半は `USB\VID_0781&PID_5583` の形で、`&REV_` は付いていませんでした。`&REV_` はハードウェア ID (`DEVPKEY_Device_HardwareIds`) のほうにあります。

**インスタンス ID**:

| 機器 | インスタンス ID | 出典 |
|---|---|---|
| 有効なシリアル番号を持つ (`UniqueID` = TRUE) | シリアル番号 | インスタンス ID が「シリアル番号の情報を含む」ことは [Instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids) に一般論として記載。USB でシリアル番号の文字列がそのまま入ることは観察 |
| シリアル番号を持たない (`UniqueID` = FALSE) | PnP マネージャーが書き換えた値 (`5&2c3a1b&0&3` など) | 書き換えることは [Instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids) に記載。値の形は未記録 |
| `IgnoreHWSerNum` が設定されている | 接続しているポートに結び付いた値 | [USB device-specific registry settings](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-device-specific-registry-settings) ("the device instance is tied to the port to which the device is attached") |

**未確認**: シリアル番号の文字列ディスクリプタに、インスタンス ID に使えない文字 (`\` など) や制御文字が含まれる場合の扱い (無効として無視されるのか、置き換えられるのか) は、ドキュメントに記載が見つからず、実機でも確認していません。

### 複合デバイスのインターフェイス: `USB\VID_vvvv&PID_pppp&MI_zz\<インスタンス ID>`

**ドキュメントの記述** ([Standard USB Identifiers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/standard-usb-identifiers)):

- インターフェイスが複数ある USB デバイス (複合デバイス) で、汎用親ドライバー (usbccgp.sys) が読み込まれると、インターフェイスごとに PDO を作る。
- そのデバイス ID は `USB\VID_v(4)&PID_d(4)&MI_z(2)`。`z(2)` はインターフェイスディスクリプタの `bInterfaceNumber`。

ストレージ機能を持つ複合デバイス (キーボード付きのカードリーダーなど) では、ツリーが `USB デバイス本体 → USB\VID_...&MI_00 → USBSTOR\Disk...` のように 1 段深くなります。
インターフェイスのノードのインスタンス ID の形は、ドキュメントに記載がなく、このプロジェクトでも記録していません。

### BOT 接続のディスク: `USBSTOR\<種類>&Ven_<v>&Prod_<p>&Rev_<r>\<インスタンス ID>`

**ドキュメントの記述** ([Identifiers Generated by USBSTOR.SYS](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-generated-by-usbstor-sys)):

- USB ストレージポートドライバー (usbstor.sys) が、論理ユニット (LUN) ごとに PDO を作る。
- ID は SCSI の INQUIRY データから作る。ドキュメント上のデバイス ID の形は `USBSTOR\v(8)p(16)r(4)`、ハードウェア ID の形は `USBSTOR\t*v(8)p(16)r(4)` など。
- `t*` は SCSI のデバイス種別から決まる文字列。

| SCSI の種別コード | `t*` |
|---|---|
| `DIRECT_ACCESS_DEVICE` (0) | `Disk` または `SFloppy` |
| `READ_ONLY_DIRECT_ACCESS_DEVICE` (5) | `CdRom` |
| `OPTICAL_DEVICE` (7) | `Optical` |
| その他 | `Other` など |

**観察**: 実機のデバイスインスタンス ID は `USBSTOR\DISK&VEN__USB&PROD__SANDISK_3.2GEN1&...` のように、`Ven_` / `Prod_` / `Rev_` の区切りが入った形で、ドキュメントの例 (`USBSTOR\SEAGATE_ST39102LW_______0004`) とは異なりました。
INQUIRY の文字列の空白は `_` に置き換わっています (SCSI では「空白、カンマ、表示できない文字を `_` に置き換える」と [Identifiers for SCSI Devices](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-scsi-devices) に記載)。

**未確認**: インスタンス ID は一般に `<USB のシリアル番号>&<LUN>` の形 (例: `...&0`) とされますが、ドキュメントに記載がなく、このプロジェクトでも記録していません。ELECOM の暗号化 USB メモリのように 1 台に複数の LUN がある機器では、ここが `&0`、`&1` と分かれると考えられます。

### UAS 接続のディスク、NVMe: `SCSI\<種類>&Ven_<v>&Prod_<p>\<インスタンス ID>`

**ドキュメントの記述** ([Identifiers for SCSI Devices](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-scsi-devices)):

- デバイス ID は `SCSI\t*v(8)p(16)r(4)`。Windows 10 バージョン 2004 以降、NVMe では `SCSI\t*v(8)p(40)r(8)` などの形も追加された。
- `t*` は `Disk`、`CdRom` など (USBSTOR とほぼ同じ表)。

**観察**: UAS 接続の USB SSD と内蔵 NVMe SSD は、どちらも `SCSI\Disk&Ven_...` の形でした ([findings.md](findings.md))。
**列挙子が同じ `SCSI` なので、ID の先頭だけでは USB 経由かどうかを判別できません。** 新方式が ID の文字列ではなく、親をたどって USB ハブに着くかで判定しているのはこのためです。

### ボリューム: `STORAGE\Volume\<インスタンス ID>`

**ドキュメントの記述**:

- `GUID_DEVINTERFACE_VOLUME` は「ボリュームデバイス」のインターフェイスで、システムのストレージクラスドライバーが登録する ([GUID_DEVINTERFACE_VOLUME](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-volume))。
- マウントマネージャーは、このインターフェイスの到着でボリュームを知る。ボリュームを扱うドライバー (通常はクラスドライバー) は `MOUNTDEV_MOUNTED_DEVICE_GUID` のインターフェイスを登録しなければならない ([Supporting Mount Manager Requests in a Storage Class Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/supporting-mount-manager-requests-in-a-storage-class-driver))。`MOUNTDEV_MOUNTED_DEVICE_GUID` と `GUID_DEVINTERFACE_VOLUME` は同じ値 `{53F5630D-B6BF-11D0-94F2-00A0C91EFB8B}` (ヘッダーの定義で確認)。
- デバイスセットアップクラス `Volume` は「システムの論理ボリュームマネージャーと、ボリュームを表すデバイスオブジェクトを作るクラスドライバーが定義するストレージボリューム」 ([System-defined device setup classes](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/system-defined-device-setup-classes-available-to-vendors))。

**観察 (ドキュメントには ID の形の記載が見つからない)**:

- 列挙子は `STORAGE` で、親はボリュームマネージャー (`ROOT\volmgr\0000`) でした。親をたどってもディスクや USB 機器には着きません。
- インスタンス ID は、ディスクの扱いによって 2 通りでした。

| ディスク | インスタンス ID の形 | 例 |
|---|---|---|
| 固定ディスク扱い (内蔵 SSD、USB-SATA 変換の SSD など) | `{<GUID>}#<パーティションの開始オフセット>` | `STORAGE\Volume\{xxxxxxxx-...}#...` |
| リムーバブルメディア扱い (USB メモリなど) | ディスクのインターフェイスパスの `\` を `#` に置き換えたもの | `STORAGE\Volume\_??_USBSTOR#Disk&Ven_...#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}` |

後者の末尾の `{53f56307-...}` は `GUID_DEVINTERFACE_DISK` の値です ([GUID_DEVINTERFACE_DISK](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-disk))。
既存方式が「ボリュームの ID が `USBSTOR#` を含む」で USB メモリを見分けられたのは、この埋め込みのためです。固定ディスク扱いの USB-SATA 変換の SSD はこの形にならないので、既存方式では漏れました ([findings.md](findings.md))。

### 光学ドライブ・フロッピーのボリューム: ドライブ自身の devnode

**ドキュメントの記述**:

- CD-ROM のクラスドライバーは `GUID_DEVINTERFACE_CDROM` を ([GUID_DEVINTERFACE_CDROM](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-cdrom))、フロッピーのクラスドライバーは `GUID_DEVINTERFACE_FLOPPY` を ([GUID_DEVINTERFACE_FLOPPY](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-floppy)) 登録する。
- ボリュームを扱うクラスドライバーは、マウントマネージャー向けにボリュームのインターフェイスも登録する (前節)。

**観察**: 光学ドライブ (`USBSTOR\CdRom&...`) とフロッピー (`USBSTOR\SFloppy&...`) では、ボリュームのインターフェイスを持つ devnode がドライブ自身でした。パーティションを持たないので、ボリュームマネージャーの devnode を経由しません。

新方式と usbinfo のボリュームの対応付けは、この性質を使っています。ボリュームの devnode が `GUID_DEVINTERFACE_CDROM` / `GUID_DEVINTERFACE_FLOPPY` も登録していれば「ドライブ自身」と判定します。ID が `STORAGE\Volume\` で始まるかという文字列の形は、判定に使いません。

### WPD デバイス: `SWD\WPDBUSENUM\<インスタンス ID>`

**観察**: ストレージ由来の WPD デバイス (デバイスマネージャーの「ポータブル デバイス」) は、列挙子が `SWD` (ソフトウェアデバイス) で、`WPDBUSENUM\` の後ろがボリュームのインスタンス ID (`STORAGE\Volume\` の後ろ) と同じでした ([findings.md](findings.md))。
usbinfo はこの一致を使って WPD デバイスとボリュームを対応付けています。WPD は判定には使っていません。

## このプロジェクトでの使い方

判定で ID のどの部分に頼っているかと、その根拠の強さです。

| 使っている箇所 | 頼っている形 | 根拠 |
|---|---|---|
| 新方式: USB 経由かの判定 | なし (親が `GUID_DEVINTERFACE_USB_HUB` を登録しているか) | ドキュメント ([GUID_DEVINTERFACE_USB_HUB](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-usb-hub)) |
| 新方式: 光学ドライブ・フロッピーの除外 | なし (ボリュームの devnode が `GUID_DEVINTERFACE_CDROM` / `FLOPPY` を登録しているか) | ドキュメントと観察 (前節) |
| ホワイトリスト方式: USB デバイス本体の特定 | なし (親が `GUID_DEVINTERFACE_USB_HUB` を登録しているか) | ドキュメント |
| ホワイトリスト方式: VID / PID、シリアル番号 | なし (ハブへの IOCTL で機器のディスクリプタを直接読む) | ドキュメント ([whitelist-flow.md](whitelist-flow.md) を参照) |
| 既存方式: USB 経由かの判定 | ボリュームの ID に `USBSTOR#` を含む | 観察のみ (ボリュームの ID の形はドキュメントに記載なし) |
| usbinfo: WPD とボリュームの対応付け | `SWD\WPDBUSENUM\` の後ろがボリュームの ID と同じ | 観察のみ |

ホワイトリスト方式は、以前は ID から `USB\VID_vvvv&PID_pppp` とシリアル番号を切り出す設計でしたが、上の「解析しない」という注意に合わせて、ハブへの IOCTL でディスクリプタを直接読む設計に変えました。

## 参照したドキュメント

| ページ | 内容 |
|---|---|
| [Device Identification Strings](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-identification-strings) | 識別文字列の種類。「解析せず比較だけに使う」という注意 |
| [Device ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-ids) | デバイス ID |
| [Hardware ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/hardware-ids) | `<enumerator>\<enumerator-specific-device-ID>` の形 |
| [Instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids) | インスタンス ID と `UniqueID` |
| [Device instance ID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids) | デバイスインスタンス ID の作り方と長さ |
| [DEVICE_CAPABILITIES](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_capabilities) | `UniqueID`、`Removable` などのメンバー |
| [Standard USB Identifiers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/standard-usb-identifiers) | USB のデバイス ID、`&MI_xx` |
| [Enumeration of the composite parent device](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device) | 複合デバイスと usbccgp.sys |
| [USB device-specific registry settings](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-device-specific-registry-settings) | `IgnoreHWSerNum` |
| [Identifiers Generated by USBSTOR.SYS](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-generated-by-usbstor-sys) | `USBSTOR\...` の ID と種別の文字列 |
| [Identifiers for SCSI Devices](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-scsi-devices) | `SCSI\...` の ID と種別の文字列 |
| [GUID_DEVINTERFACE_VOLUME](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-volume) | ボリュームのインターフェイス |
| [GUID_DEVINTERFACE_DISK](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-disk) / [CDROM](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-cdrom) / [FLOPPY](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-floppy) / [USB_HUB](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-usb-hub) | 各デバイスインターフェイスと登録するドライバー |
| [Supporting Mount Manager Requests in a Storage Class Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/supporting-mount-manager-requests-in-a-storage-class-driver) | マウントマネージャーとボリュームのインターフェイス |
| [System-defined device setup classes](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/system-defined-device-setup-classes-available-to-vendors) | セットアップクラス `Volume`、`CDROM`、`DiskDrive` など |
