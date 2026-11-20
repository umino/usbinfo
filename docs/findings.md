# 調査レポート: リムーバブルな USB ストレージの判定方法

`usbinfo` を実機で動かし、「USB 経由で接続された取り外し可能なストレージ」をどの値で判定できるかを調べた結果です。

- 調査日: 2026-11-12 〜 2026-11-15
- 調査環境: Windows 11 (ビルド 26300) のデスクトップ PC 1 台
- 各項目の意味は [README](../README.md) を参照してください。

## 結論

次の 3 条件の AND で判定するのがよい、という結果になりました。

1. **ディスクである**: `GUID_DEVINTERFACE_DISK` で列挙されたデバイスである。
2. **USB 経由である**: devnode の親をたどると USB ハブ直下のノードに行き着く。
3. **取り外し可能である**: ディスク devnode の `DEVPKEY_Device_RemovalPolicy` が `ExpectNoRemoval` 以外である。

今回つないだ機器では、この条件で対象と対象外が期待どおりに分かれました。
一方、`GetDriveType`、`RemovableMedia` ビット、WPD (ポータブル デバイス) の有無は、単独では判定に使えませんでした。

ただし、調査は PC 1 台、Windows 11 のみです。USB メモリは 1 本だけ、カードリーダーと内蔵カードリーダーは試していません (「未検証の範囲」を参照)。

## 調査した機器

| 機器 | 接続 | 状態 |
|---|---|---|
| 内蔵 NVMe SSD (Crucial CT1000P310SSD8) | マザーボード直結 | システムドライブ (C:) |
| I-O DATA SSPH-UA (ポータブル SSD、960 GB) | USB 3.x、UAS | APFS (Mac 用) でフォーマット済み。Windows からは読めない |
| Intel SSD 40 GB + JMicron USB-SATA 変換 (152D:2329) | USB 2.0 速度、BOT、ハブ経由 | NTFS、ドライブレターあり |
| PLEXTOR PX-512M8PeGN + NVMe ケース (152D:A583) | USB 3.x、UAS | 容量が取得できない (読み取り不能) |
| I-O DATA 外付け BD ドライブ (04BB:0229) | USB 3.x、BOT | メディアなし |
| USB レシーバー、USB カメラ | 外部ポート | ストレージではない |
| Bluetooth アダプター、オンボードの HID 機器 | 内部接続の USB | ストレージではない |

## 判定に関わる値の実測

| 機器 | busType | GetDriveType | removableMedia | deviceHotplug | diskRemovalPolicy | safeRemovalRequired | WPD | verdict |
|---|---|---|---|---|---|---|---|---|
| 内蔵 NVMe | Nvme | FIXED | false | false | ExpectNoRemoval | false | なし | `NOT_USB` |
| I-O DATA SSPH-UA | Usb | (レターなし) | false | true | ExpectSurpriseRemoval | false | なし | `USB_REMOVABLE_STORAGE` |
| Intel SSD + USB-SATA | Usb | FIXED | false | true | ExpectSurpriseRemoval | true | あり | `USB_REMOVABLE_STORAGE` |
| PLEXTOR + NVMe ケース | Usb | (レターなし) | false | true | ExpectSurpriseRemoval | false | なし | `USB_REMOVABLE_STORAGE` |
| 外付け BD ドライブ | Usb | CDROM | true | true | ExpectSurpriseRemoval | true | なし | `USB_OPTICAL_DRIVE(excluded)` |

## わかったこと

### GetDriveType と RemovableMedia ビットは使えない

USB 接続の SSD は 3 台とも `RemovableMedia` が false で、ドライブレターのある Intel SSD の `GetDriveType` は内蔵 NVMe と同じ `DRIVE_FIXED` でした。
`RemovableMedia` が true になったのは BD ドライブだけです。この値で判定すると、対象にしたい SSD が外れ、対象外にしたい BD ドライブが入ります。

### インスタンス ID の文字列では USB かどうかを判別できない

BOT 接続の機器のディスクは `USBSTOR\Disk&...` という ID になりますが、UAS 接続の機器は `SCSI\Disk&...` になります。
後者は内蔵 NVMe (`SCSI\Disk&Ven_NVMe&...`) と同じ形で、ID の先頭だけでは区別できません。
devnode の親をたどる方法は、BOT、UAS、ハブ経由のいずれでも USB デバイス本体に行き着きました。

`busType` は今回の USB 機器すべてで `Usb` でした。照合用には使えますが、ベンダー製ドライバーで別の値になる例は今回の機器では確認できていません。

### 光学ドライブは別のインターフェイスで列挙される

外付け BD ドライブは USB 経由かつ取り外し可能で、当初の 2 条件 (USB 経由、取り外し可能) では対象に入ってしまいました。
BD ドライブは `GUID_DEVINTERFACE_CDROM` で列挙されるので、「`GUID_DEVINTERFACE_DISK` で列挙されたものに限る」という条件を足して除外しています。

### 取り外しポリシーは内蔵と外付けを分けた

ディスク devnode の `DEVPKEY_Device_RemovalPolicy` は、内蔵 NVMe が `ExpectNoRemoval`、USB 機器がすべて `ExpectSurpriseRemoval` でした。

元になる USB デバイス本体の `CM_DEVCAP_REMOVABLE` は、非ストレージ機器でも内蔵と外付けを正しく分けました。

| 機器 | capRemovable | inLocalMachineContainer | portIsUserConnectable |
|---|---|---|---|
| USB レシーバー、USB カメラ、各 USB ストレージ | true | false | true |
| Bluetooth アダプター、オンボードの HID 機器 | false | true | false |

内蔵の USB 接続ストレージ (ノート PC の内蔵カードリーダーなど) がこの値で除外できるかは、該当する機器がなく確認できていません。

### 読めないディスクでも判定できる

PLEXTOR は容量取得が失敗し、`Get-Disk` にも出ない状態でしたが、判定に使う 3 つの値はどれも取得でき、`USB_REMOVABLE_STORAGE` と判定されました。
判定に使っているのは devnode の情報で、ディスクの中身を読む必要がないためです。

なお、機器が準備完了にならない間は、ディスクのデバイスインターフェイスが登録されず、列挙に現れないことがありました。

### WPD は対応付けできるが、判定の主軸には向かない

**対応付けは可能でした。** ストレージ由来の WPD デバイスのインスタンス ID は `SWD\WPDBUSENUM\<後半>` の形で、`<後半>` がボリューム devnode (`STORAGE\Volume\<後半>`) の ID と一致しました。
そこからボリュームを開き、`IOCTL_STORAGE_GET_DEVICE_NUMBER` でディスク番号を得られます。Intel SSD ではこの方法で WPD デバイスとディスクを対応付けできました。

**ただし、WPD に現れたのは 1 台だけでした。**

| 機器 | WPD デバイス | 理由と考えられること |
|---|---|---|
| Intel SSD (NTFS、ドライブレターあり) | あり | Windows がボリュームをマウントしている |
| I-O DATA SSPH-UA (APFS) | なし | Windows が読めるファイルシステムがない |
| PLEXTOR (読み取り不能) | なし | ボリュームがない |
| 外付け BD ドライブ | なし | (メディアなしの状態でのみ確認) |

WPD を判定に使うと、未フォーマットのディスクや、他の OS でフォーマットされたディスクを取りこぼします。
BD ドライブが WPD に出ないのは期待どおりですが、メディアを入れた状態は試していません。

また、WPD デバイスを作る `WPDBusEnum` サービスは、ストレージがない間は停止しており、必要になった時点で起動していました。サービスが無効化された環境では WPD デバイスは現れないはずです (未確認)。

### diskSafeRemovalRequired は機器によって違う

`DEVPKEY_Device_SafeRemovalRequired` は、BOT 接続の Intel SSD と BD ドライブで true、UAS 接続の 2 台で false でした。
接続方式で変わるようで、取り外し可能かどうかの判定には使えません。

### ハブディスクリプタの DeviceRemovable ビットは区別に使えない

ルートハブでは、内蔵機器が接続されたポートでもこのビットは 0 (取り外し可能) でした。

## 既存方式と新方式の比較 (2026-11-21 追記)

`usbinfo -f letters` で、既存方式 (`SelectDrivesLegacy`) と新方式 (`SelectDrivesNew`) の抽出結果を比べました。方式の内容は README の「ドライブレターの抽出」を参照してください。

| ドライブ | 機器 | GetDriveType | 既存方式 | 新方式 |
|---|---|---|---|---|
| C: | 内蔵 NVMe (CT1000P310SSD8) | FIXED | 対象外 | 対象外 (USB に届かない) |
| D: | SanDisk USB メモリ (0781:5583、BOT、SuperSpeed) | REMOVABLE | **抽出** | **抽出** |
| E: | Intel SSD + USB-SATA 変換 (152D:2329、BOT) | FIXED | 対象外 | **抽出** |

わかったこと:

- **USB メモリのボリュームの ID は `_??_USBSTOR#` を含む形だった。** `STORAGE\VOLUME\_??_USBSTOR#DISK&VEN__USB&PROD__SANDISK_3.2GEN1&...#{53F56307-...}` で、既存方式の `USBSTOR#` のチェックはこれに当たっていました。
- **既存方式の「親の ID が `USBSTOR\` を含む」チェックは、どのボリュームにも当たらなかった。** ボリュームの親は 3 台とも `ROOT\VOLMGR\0000` でした。
- **USB-SATA 変換の SSD は、既存方式では漏れた。** `GetDriveType` が `DRIVE_FIXED` を返し、ボリュームの ID も `STORAGE\Volume\{GUID}#<オフセット>` の形で `USBSTOR#` を含まないためです。新方式では、ディスク番号から `USBSTOR\DISK&...` のディスクに対応付け、その親で USB デバイス本体 (`USB\VID_152D&PID_2329\...`) に届きました。
- `CM_Get_Device_ID` が返す ID は大文字 (`STORAGE\VOLUME\...`)、`DEVPKEY_Device_InstanceId` は元の大文字小文字のまま (`STORAGE\Volume\...`) でした。文字列で比べるときは大文字小文字を区別しないほうが安全です。
- 2 台の USB 機器はどちらも `capRemovable` が true、`inLocalMachineContainer` が false で、`--exclude-internal-usb` を付けても結果は同じでした。

## 調査中に遭遇した接続の問題

判定方法とは別に、接続の状態によってディスクが読めなくなる例がありました。

- **ハブ経由で読めなくなった例**: I-O DATA SSPH-UA は、USB ハブ経由では SCSI コマンドに「ハードウェアエラー / 内部ターゲット障害」(センスキー 04h、ASC 44h、ASCQ 81h) を返し、容量が取得できませんでした。PC 背面のポートに直結すると正常に読めました。ハブの電力不足か相性と考えられますが、どちらかは切り分けていません。
- **読めないままだった例**: PLEXTOR は直結でも同じエラーを返し続けました。USB 2.0 のポートにつないだときは「準備中」(センスキー 02h、ASC 04h、ASCQ 01h) のまま進みませんでした。SSD 本体とケースのどちらが原因かは切り分けていません。

どちらの状態でも、機器の型番やシリアル番号は取得でき、判定結果は変わりませんでした。

## 未検証の範囲

次の条件は試していないため、判定方法を採用する前に確認が必要です。

- **Windows 10**: 調査は Windows 11 のみです。本来の対象である Windows 10 では動かしていません。
- **USB メモリ**: 1 本 (SanDisk、BOT) のみ確認しました (「既存方式と新方式の比較」を参照)。UAS で動く USB メモリは試していません。
- **カードリーダー**: メディアあり、メディアなし、複数スロットの各状態。
- **内蔵の USB 接続ストレージ**: ノート PC の内蔵カードリーダーなど。取り外しポリシーで除外できるかが要点です。
- **USB 以外の取り外し可能ストレージ**: eSATA、SD ホストコントローラー接続のカードリーダーなど。
- **ベンダー製ドライバーを使う USB ストレージ**: `busType` が `Usb` にならない可能性があります。
- **メディアを入れた光学ドライブ**: WPD に現れるかどうか。
- **MTP 機器**: スマートフォンなど。`wpdDevices[]` での見え方を確認していません。
- **複数の PC**: 内蔵か外付けかの情報は ACPI の記述に依存するため、機種による違いがありえます。
