# タスクトレイ方式のドライブ抽出フロー (設計メモ)

`usbinfo.cpp` の `drivesel::SelectDrivesSafeRemoval()` が行っている抽出手順です。
通知領域の「ハードウェアを安全に取り外してメディアを取り出す」(以下、タスクトレイ) に出るストレージと同じものを抽出することを狙っています。

出力は既存方式・新方式と同じ `DriveEntry { driveIndex, devInst }` のリストです。

## 考え方: タスクトレイの表示を決めているプロパティを使う

Windows は、`DEVPKEY_Device_SafeRemovalRequired` が TRUE のデバイスがあると、タスクトレイにアイコンを表示します ([DEVPKEY_Device_SafeRemovalRequired](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-safeRemovalRequired))。

> If this property for a hot-plug device instance has a value of DEVPROP_TRUE, the device instance requires safe removal from the computer. In this case, Windows displays the Safely Remove Hardware icon in the notification area ...

同じページに、Windows がこの値を TRUE にする条件が書かれています。

1. 機器が現在接続されている。
2. 機器が開始済みか、システムが自動で取り出せる。
3. `CM_DEVCAP_SURPRISEREMOVALOK` が立っていない。
4. `DEVPKEY_Device_SafeRemovalRequiredOverride` が FALSE に設定されていない (TRUE に設定されていれば、ほかの条件によらず TRUE)。
5. 機器自身が親から取り外せるか、取り外せる祖先がある。

新方式の取り外しポリシーは、既定値の決め方が公開されていませんでした ([new-method-flow.md](new-method-flow.md) の「取り外しポリシーを使う根拠」)。
こちらは条件とタスクトレイとの関係が公開されているので、「タスクトレイと同じものを抽出する」という要件の根拠になります。

タスクトレイに出るのはディスクの devnode ではなく、取り外せる祖先 (USB デバイス本体など) のことが多いので、ディスクから祖先へたどって確かめます。

## USB 以外を含めるか

定数 `kSafeRemovalIncludeNonUsb` で切り替えます。既定値は `false` (USB のみ) で、関数の引数 `includeNonUsb` の既定値にもなっています。

| 値 | 抽出するもの |
|---|---|
| `false` (既定) | USB 経由のディスクのうち、タスクトレイに出るもの |
| `true` | USB 以外 (eSATA、Thunderbolt や PCIe のホットプラグ接続の NVMe など) も含め、タスクトレイに出るディスク |

`-f letters` / `-f json` の比較出力も、この定数の値で動きます。`driveLetters[].safeRemovalDetail.includeNonUsb` に、使った値が出ます。

## フロー

### 1.〜3. ドライブレター → ボリューム → ディスク

新方式の手順 1〜3 と同じで、コードも共通の `EnumerateDiskVolumes()` を使います ([new-method-flow.md](new-method-flow.md) を参照)。

1. `GetVolumeNameForVolumeMountPointW` でドライブレターとボリューム GUID の表を作る。
2. `GUID_DEVINTERFACE_DISK` のディスクに `IOCTL_STORAGE_GET_DEVICE_NUMBER` を発行し、ディスク番号 → ディスクの表を作る。
3. `GUID_DEVINTERFACE_VOLUME` のボリュームのうち、ドライブレターがあり、devnode が `GUID_DEVINTERFACE_CDROM` / `FLOPPY` を登録しておらず、`IOCTL_STORAGE_GET_DEVICE_NUMBER` が `FILE_DEVICE_DISK` で表にあるものを、ディスクに対応付ける。

**光学ドライブはここで外れます。** タスクトレイには媒体の入った光学ドライブも出ますが、ディスクを起点にしているので抽出しません。

### 4. (USB のみのとき) USB 経由か確かめる

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_Parent` | ディスクの `DevInst` から順に親へ | |
| `CM_Get_Device_IDW` + `CM_Get_Device_Interface_List_SizeW` + `CM_Get_Device_Interface_ListW` | 親の ID, `&GUID_DEVINTERFACE_USB_HUB`, `CM_GET_DEVICE_INTERFACE_LIST_PRESENT` | 一覧が空でない親 (USB ハブ) があれば USB 経由。なければ、`includeNonUsb` が false のとき対象外 |

新方式の手順 4 (`findUsbDevice()`) と同じです。

### 5. 安全な取り外しが必要なノードを探す

| API | 主な引数 | 判定 |
|---|---|---|
| `CM_Get_DevNode_PropertyW` | ディスクの `DevInst`, `&DEVPKEY_Device_SafeRemovalRequired` (`DEVPROP_TYPE_BOOLEAN`) | TRUE ならこのノードで見つかったとする |
| `CM_Get_Parent` + 同上 | 親、その親…とルートまで | 最初に TRUE だったノードを「取り外し対象のノード」とする。ルートまでなければ対象外 |

取得できなかったノードは FALSE と同じ扱いで、さらに親へ進みます。

### 6. 採用する

5. でノードが見つかったボリュームについて、1. の表で一致したドライブレターすべてを `DriveEntry { driveIndex, devInst(ボリューム) }` として返します。

## 新方式との違い

| 条件 | 新方式 | タスクトレイ方式 |
|---|---|---|
| ドライブレター → ボリューム → ディスク (1.〜3.) | ○ | ○ (共通) |
| USB 経由 (4.) | 必須 | `includeNonUsb` が false のときだけ |
| ディスクの取り外しポリシーが `ExpectNoRemoval` 以外 | 必須 | 見ない |
| USB デバイスの `CM_DEVCAP_REMOVABLE` (`excludeInternalUsb`) | 任意 | 見ない |
| ディスクか祖先の `SafeRemovalRequired` が TRUE | 見ない | 必須 |

食い違いうるケース:

| ケース | 新方式 | タスクトレイ方式 |
|---|---|---|
| ドライバーが `SurpriseRemovalOK` を立てた USB 機器 | 抽出 | 抽出しない (条件 3 で TRUE にならない) |
| `SafeRemovalRequiredOverride` で表示を消された機器 | 抽出 | 抽出しない |
| 内蔵扱いの USB 機器 (取り外せる祖先がない) | 取り外しポリシー次第 | 抽出しない想定 (条件 5) |
| USB から起動した機器 | 抽出 (`ExpectOrderlyRemoval` になる) | 未確認 (タスクトレイに出ない可能性がある) |

## 採用前に確認すること

実機で `usbinfo -f letters` を実行し、`Safe` 列と、実際のタスクトレイの一覧を見比べてください。
`Safe: node / result` 列に、`SafeRemovalRequired` が TRUE だったノードの名前が出ます。タスクトレイに出る名前と同じになるはずです。

| 確認すること | 見る場所 |
|---|---|
| タスクトレイに出る機器が、すべて `Safe` 列で `selected` になるか | `-f letters` とタスクトレイの一覧 |
| `Safe` 列で `selected` の機器が、すべてタスクトレイに出ているか | 同上 |
| UAS 接続の機器で、どのノードが TRUE になるか | `-f json` の `driveLetters[].safeRemovalDetail` (調査ではディスク自身の値は false だった。[findings.md](findings.md)) |
| 「クイック削除」と「高パフォーマンス」で結果が変わらないか | デバイスマネージャーのポリシーを切り替えて再実行 |
| ELECOM の暗号化 USB メモリでどうなるか | ロック解除の前後で `-f letters` |

タスクトレイの表示処理の中身は公開されていません。「この値が TRUE ならアイコンを表示する」ことは書かれていますが、一覧の各行がこの値と完全に一致するかは、実機で確かめる必要があります。

既存コードの検証環境のように結果を手で写して持ち帰る場合は、`usbinfo -f code` の 15、16 文字目を見ます ([manual-transcription.md](manual-transcription.md))。
15 文字目がタスクトレイ方式の結果 (`S` = 抽出)、16 文字目がディスク自身の `SafeRemovalRequired` です。15 文字目が `S` で 16 文字目が `N` なら、TRUE だったのはディスクではなく祖先です (UAS の機器で起こると想定している形)。
新方式の結果 (4 文字目) と見比べれば、両方式の食い違いが分かります。
