# ESP32-S3 UVCカメラ / MJPEG確認ファームウェア

## tolo-edge-bridge 接続（2026-11追加）

`viewer-server/` は使わず、ESP は tolo-edge-bridge の Firestore シグナリングに
edge ピアとして参加する。ブラウザ（観測ページ）が ESP セッションの console 役になり、
`?camera=<ESPのedgeDeviceId>` を付けて開くと ESP の JPEG を DataChannel で受け取る。
シグナリングは両端ともポーリングで、ESP は Firestore REST を約1秒（offer 待ちは約300ms）
間隔で読む。映像は従来どおり `jpeg` DataChannel（JPG1 断片）で送る。

必要な設定は NVS の `cloud` と `network` に保存する。Edge Bridge の URL は既存の
`signaling` コマンド、Firebase・ESP の ID・IdP の issuer は `cloud` で設定する。
通常は管理UIの Web Serial から JSON Lines でまとめて書き込み、設定後に自動で再起動する。

```text
signaling            -> http://<Edge Bridge のホスト>:8787
cloud                -> Edge device ID / IdP issuer / Firebase project ID / API key /
                        Firestore / Auth のエミュレータ
                        （LAN IP:port、または https://<TryCloudflare ドメイン>）
device_authorization -> Device Authorization を開始し、user_code を JSON で出力
show                 -> active/saved の cloud 設定と認可状態を確認
reboot
```

ESP は IdP の public client `esp-camera` として Device Authorization Grant を使う。
`scope` は `events.stream` だけで、`tenant_id` / `event_id` / `edge_device_id` は
スタッフが管理UIで承認した内容から event_access のクレームに入る。ESP 自身は
`event_id` や `edge_device_id` を Device Authorization 要求に送らない。
起動後は認可がなければ自動で Device Authorization を開始し、`user_code` をシリアルへ
JSON Lines で出力する。承認後は event_access を取得し、期限の70〜80%で Refresh Token
を使って更新する。トークンと Refresh Token は NVS に保存し、ログや画面には出さない。

Web Serial の設定コマンドは次の形式（1行1JSON、`\n` 終端）。

```json
{"type":"config","version":1,"wifi_ssid":"...","wifi_password":"...","signaling_url":"http://host:8787","idp_url":"https://auth.example","firebase_project_id":"...","firebase_api_key":"...","firestore_emulator_host":"","firebase_auth_emulator_host":"","edge_device_id":"0123456789abcdef"}
```

成功時は `{"type":"config_result","ok":true}` を返して約500ms後に再起動する。
`status`・`device_authorization`・`reboot` もJSONで応答する。Wi-Fiパスワードと
トークンはJSONにもログにも出さない。

Firestore / Auth を TryCloudflare 経由にする場合は、tolo-web 側で
`task esp-tunnel` を実行し、表示される2つのURLを使う（edge-bridge 用と
Firestore/Auth 用で別ドメイン）。URL はトンネル再起動で変わるため、その都度
`signaling` / `cloud` または Web Serial で設定し直す。メール認証付きの `task tunnel` は
ESP には使えない。

時刻は SNTP（`pool.ntp.org`）で合わせる。オフライン環境では既存の `clock` で設定する。
Firestore と HTTPS の都合上、ESP の時計が大きくずれていると接続できない。
Refresh が `invalid_grant` を返した場合や、更新中に再起動した場合は再承認が必要になる。
そのときは `show` の `Authorization: required` を確認し、管理UIから再度承認する。

ESP の `edgeDeviceId` は 16 桁小文字 hex とする。観測ページの管理UI向け URL
（`.../observation/<観測ページのedgeDeviceId>`）に `?camera=<ESPのedgeDeviceId>` を足す。
ESP は登録・Heartbeat を持たない。観測点の登録は観測ページ側の edgeDeviceId が担う。

この経路は Firebase Auth のカスタムトークンと Firestore のセキュリティルールをそのまま使う。
ESP は `IssueFirestoreToken` を event_access 付きで呼び、返ったカスタムトークンで
Auth にサインインし、`sessions` と `signals` を読み書きする。`viewer-server` の
`/api/esp/offer`・`/api/esp/answer` はもう使わない。
契約・設定・確認手順の詳細は [tolo-edge-bridge 接続メモ](../docs/tolo-edge-bridge-20261102.md) を参照する。

## NVS暗号化とパーティション（2026-11追加）

NVSにはWi-Fi資格情報、EAP証明書・秘密鍵、クラウド設定、`event_access`、
Refresh Tokenが保存される。ADR-0127の「可能なら保存領域を暗号化する」に合わせ、
ESP-IDFのNVS暗号化（XTS-AES）を使う。8MBフラッシュ用の
`esp_webcam/partitions.csv`へ`nvs_keys`パーティションを追加した。
factoryのオフセットとサイズは従来の`partitions_singleapp_large.csv`と同じ
`0x10000` / `0x177000`のままとし、既存のアプリ配置を変えない。

| 名前 | 種別 | オフセット | サイズ | 備考 |
| --- | --- | --- | --- | --- |
| `nvs` | data / nvs | `0x9000` | `0x6000` | 設定・トークン |
| `phy_init` | data / phy | `0xf000` | `0x1000` | |
| `factory` | app / factory | `0x10000` | `0x177000` | 従来と同じ |
| `nvs_keys` | data / nvs_keys | `0x187000` | `0x1000` | フラッシュ暗号化は未使用 |

`CONFIG_PARTITION_TABLE_CUSTOM=y`、`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`、
`CONFIG_NVS_ENCRYPTION=y`を`sdkconfig.defaults`へ設定した。NVS暗号化の有効・無効は
`CONFIG_WEBCAM_NVS_ENCRYPTION`（既定`y`）で切り替える。無効にすると従来どおり
暗号化なしでNVSを初期化する。`CONFIG_NVS_SEC_KEY_PROTECT_NONE=y`とし、鍵の読み書きは
`main/nvs_boot.c`が`nvs_keys`パーティションを直接扱う（`nvs_sec_provider`の既定方式は使わない）。
この構成では`nvs_flash_init()`を直接呼ばず、アプリのNVS初期化は必ず`nvs_boot_init()`を通す。

起動時の処理は次のとおり。

1. `nvs_keys`パーティションを探す。見つからなければ警告を出し、暗号化なしで起動する。
2. `nvs_flash_read_security_cfg`で鍵を読む。空なら`nvs_flash_generate_keys`で生成する。
3. `nvs_flash_secure_init`で`nvs`を暗号化して初期化する。
4. 鍵が壊れている、または暗号化初期化に失敗した場合は、警告を出して暗号化なしへフォールバックする。
5. `ESP_ERR_NVS_NO_FREE_PAGES`と`ESP_ERR_NVS_NEW_VERSION_FOUND`は従来どおり
   `nvs_flash_erase`後に再試行する。

**既存の暗号化なし端末を更新する場合**

暗号化へ切り替えると、既存の平文NVSはそのままでは読めない。更新前に
Web Serialの設定値（Wi-Fi、IdP、Firebase、edgeDeviceId）を控え、次のいずれかで
NVSを消去してから書き込み、再プロビジョニングする。

```sh
idf.py -p <PORT> erase-flash
python -m esptool --chip esp32s3 --port <PORT> erase-flash
```

シリアルフラッシャーを使う場合も`--erase`相当の全消去を行う。`nvs_keys`だけを
個別に消去してはいけない。鍵を失うと暗号化済みNVSを復号できず、設定・トークンが
失われる。`idf.py flash`は通常、消去を伴わないため、暗号化へ切り替える最初の
書き込みでは全消去を明示する。暗号化済みの端末で`CONFIG_WEBCAM_NVS_ENCRYPTION`を
無効にする場合も、同様に全消去と再プロビジョニングが必要になる。

このファームウェアの既定ビルドはフラッシュ暗号化を有効にしていないため、
`nvs_keys`には`encrypted`フラグを付けていない。フラッシュ暗号化無効のまま
`encrypted`を付けると、IDFが鍵のCRCを暗号化書き込みし、通常の読み出しと一致せず
NVS暗号化を再起動後に初期化できなくなるためだ。既定ビルドではNVSの内容は暗号化されるが、
鍵自体はフラッシュ上に平文で残る。これは開発・互換用であり、物理読み出しに対する
資格情報保護にはならない。

本番・セキュアビルドでは `sdkconfig.secure.defaults` を使い、ESP32-S3のeFuse HMAC鍵から
NVS鍵を導出する。HMAC鍵を先にバーンし、`CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION=y` で
平文フォールバックを禁止する。

```sh
# 例: eFuse key block 0 に HMAC_UP 用途の鍵を書き込む（値は安全に保管する）
idf.py efuse-burn-key 0 <hmac-key-hex> HMAC_UP
idf.py -B build-secure -DSDKCONFIG=build-secure/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure.defaults" build
idf.py -B build-secure -p <PORT> flash
```

eFuseはやり直せない。既存の平文NVS端末をセキュアビルドへ移す場合は、設定を控えて
全消去・再プロビジョニングする。HMAC鍵が未設定の端末では、暗号化必須ビルドは
平文へ降格せず起動を停止する。`sdkconfig.secure.defaults` は開発用の既定ビルドとは
別のビルドディレクトリを使ってください。

NVS暗号化の判定ロジックは`tests/nvs_boot_test.c`でホスト実行できる。

```sh
cc -std=c11 -Wall -Wextra -Werror -I esp_webcam/main \
  esp_webcam/tests/nvs_boot_test.c esp_webcam/main/nvs_boot_values.c \
  -o /tmp/nvs_boot_test
/tmp/nvs_boot_test
```

DockerのESP-IDF v6.1（`espressif/idf:v6.1`）でビルドに成功した（app 1,428,736 bytes、
0x15cd00、factory 0x177000に対して7%空き）。実機への書き込み、暗号化NVSの生成・
再起動後の読み出しは未検証。

## IP表示・簡易疎通確認（2026-11-02追加）

| コマンド | 確認内容 |
| --- | --- |
| `show` | 現在取得しているIPv4・ネットマスク・ゲートウェイを設定一覧の先頭に表示 |
| `ping` | 現在のゲートウェイへICMPを3回送信 |
| `ping 192.0.2.1` | 指定したIPv4へICMPを3回送信し、応答時間と送信/応答数を表示 |
| `check signaling` | Edge Bridge の`GET /healthz`でHTTP(S)応答を確認 |
| `check stun` | `WEBCAM_STUN_URL`へUDPのSTUN Binding Requestを1回送って応答を確認 |

これらのコマンドは設定・資材を保存しない。確認コマンドからSDP交換、WebRTC接続、ICEチェック、
TURN割当て、認証の再試行を開始する処理は呼ばない。シグナリング側の`/ice`もセッションやESPの
ポーリング状態を変更しない。HTTPリダイレクトとSTUNの別サーバー誘導・認証要求には追従しない。

`show`はNVSの設定値ではなく、現在のWi-FiインターフェースからIPを取得する。
未接続・DHCP待ちはその旨を表示し、切断前のキャッシュIPを表示しない。
pingはIPv4のみでホスト名の解決や追加オプションは持たず、1回の応答待ちと送信間隔は各1000ms。
実行中はコンソールが結果を待ち、最後の集計より後に`esp>`へ戻る。

シグナリングはHTTPステータスと所要時間を表示し、200以外も「HTTPには到達した」ことが分かる形で示す。
STUNは`stun:host[:port]`のIPv4 UDPに対応（省略ポート3478）。ホスト名は通常のDNSで解決する。
UDP応答待ちは3000ms。応答のmagic cookie・要求ID・長さ・Bindingメソッド・アドレス/エラー属性を検証し、
正常応答、STUNエラー応答、タイムアウト/受信失敗、不正応答を区別する。DNSの待ち時間はSDK設定に従う。
応答の形式は[RFC 8489](https://www.rfc-editor.org/rfc/rfc8489.html)、pingはSDKの
[ICMP Echo API](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/lwip/include/apps/ping/ping_sock.h)に従う。

`tests/test_network_diagnostics.py`で未接続・古いIP・pingの成功/タイムアウト・繰り返し・開始失敗時の解放、
`tests/test_network_probes.py`でGET専用・リダイレクトなし・STUN要求1回・異常系の解放をPC上で検証した。
STUN解析とコマンド分岐のテスト、`GET /ice`の状態不変を含む受信・描画・中継14テストも成功した。
診断版の通常ビルドは成功（app 1,382,560 bytes、0x1518a0、アプリ領域10%空き）。
FAST互換版もビルド成功（app 1,364,720 bytes、0x14d2f0、アプリ領域11%空き）。
診断版の通常版をCOM3経由でESP32-S3へ書き込み、bootloader・partition table・appの
ハッシュ一致を確認した。保存済みネットワーク設定・証明書のNVS領域は保持した。
書き込み後はbootloaderで待機。IO0のGND接続を外してENリセットする。
診断版の通常起動後のIP表示・疎通結果の実機確認は未実施。

## 802.1X/EAP対応（2026-11-02追加）

`eap`でTLS、PEAP/MSCHAPv2、TTLS（EAP/MSCHAPv2/MSCHAP/PAP/CHAP）を選択できる。
WPA/WPA2/WPA3-EnterpriseとSuite-B 192-bitに対応する設定を追加した。
FASTはSDKのTLS実装の制約があるため、`sdkconfig.fast.defaults`を使う別ビルドとする。
`protocols`でそのイメージの対応方式、`credentials`で証明書・鍵・PACの保存状態を確認する。
従来の通常Wi-Fiは`wifi`、シグナリングサーバーは`signaling`で設定する。

証明書・鍵は`tools/upload_wifi_credential.py`からシリアル経由で登録し、パスワードは非表示にする。
v1の保存済み設定はSSID・PSK・サーバーURLを保持して移行する。
通常版は実機へ書き込み・照合済み。通常起動、方式一覧、公開サンプル証明書・鍵の保存と削除を確認した。
802.1X Wi-Fiへの接続成功はユーザー報告済み。全方式のRADIUS相互運用は未確認。
操作手順、認証サーバー検証、FASTの制約、ビルド・テスト結果は
[802.1X/EAP対応メモ](../docs/wifi-enterprise-20261102.md)を参照。

## シリアルからWi-Fi・シグナリングサーバーを設定（2026-11-02追加）

UART0（115200bps / 8N1、ESP GPIO43=TX・GPIO44=RX）で変更し、NVSへ保存する。
保存した設定は次回起動から使用し、電源を切っても保持する。保存前は従来のmenuconfig設定を使う。
SSIDが未設定・接続先が圏外・パスワードが違う場合も、シリアルの設定操作は使用できる。
この機能を使うには、追加後のファームウェアを一度ビルドして書き込む必要がある。

Pico debugprobeの配線はGP4→GPIO44、GP5←GPIO43、GND共通。
通常起動（IO0のGND接続を解除）し、ESP-IDFのPython環境から次のように端末を開く。
COM番号は実際のポートに合わせる。既存のログモニターや書き込みツールは終了しておく。

```powershell
python -m serial.tools.miniterm COM3 115200 --dtr 1 --rts 0 --eol CRLF
```

ローカルエコーはOFFのまま使う（`--echo`を付けない）。ESPがコマンド・SSID・サーバー入力を表示し、
パスワードは`*`で表示する。設定コンソール起動後はUARTの診断ログを停止し、入力欄が流れないようにする。
起動済みならEnterで`esp>`を表示できる。`logs on`でUARTログを表示し、Enterまたは任意の入力で
ログを止めて設定へ戻る。HTTPの`/log`・`/usb-log`用の記録は、UART表示を止めても継続する。

| コマンド | 操作 |
| --- | --- |
| `wifi` | SSID、パスワードを順に入力し保存。空パスワードはオープンネットワーク |
| `eap` | 802.1X/EAP方式・Identity・認証情報・サーバー検証を設定 |
| `protocols` | 使用可能なEAP方式・Enterpriseリンクを表示 |
| `credentials` | CA・クライアント証明書・秘密鍵・PACの保存有無とサイズを表示 |
| `clock` | 起動中のUTC Unix秒を設定。証明書の有効期間検証用 |
| `signaling` | Edge Bridge のIPv4・ホスト名・`http(s)://` URLを入力し保存。`off`でWebRTCを無効化 |
| `cloud` | Edge device ID・IdP issuer・Firebase project/API key・Firestore/Auth エミュレータを設定（`host:port` または `https://host`） |
| `token` | 復旧用。event_access JWTをマスク入力で保存（通常はDevice Authorizationを使う） |
| `show` | 現在のIP・設定と次回起動用の保存設定を表示。パスワード・トークンは設定有無だけ表示 |
| `ping [IPv4]` | IPv4へ3回のICMP疎通確認。宛先省略時はゲートウェイ |
| `check signaling` / `check stun` | 設定済みサーバーへの読み取り専用HTTP(S)／UDP Binding疎通確認 |
| `reboot` | 再起動し、保存したWi-Fi・サーバー設定をまとめて反映 |
| `help` | コマンド一覧を表示 |
| `logs on` / `logs off` | UARTの診断ログ表示を開始／停止。表示中はEnterだけで停止できる |

例えば`wifi`→SSID→パスワード、`signaling`→`192.168.1.20:8080`、`reboot`の順に入力する。
各入力の後にEnterを押す。IP・ホスト名だけならHTTPのポート8080を補い、
`http://192.168.1.20:9000`や`https://viewer.example`のようなURLでは指定ポートを使う
（URLで省略したポートはHTTP標準の80、HTTPS標準の443）。HTTPSはESP-IDFのCAバンドルで
サーバー証明書を検証する。パス・クエリー・ユーザー情報・IPv6は受け付けない。
STUNの設定は引き続きmenuconfigの`WEBCAM_STUN_URL`を使用する。

SSIDは1～32バイト。パスワードは空、8～63バイト、または64桁の16進PSK。
日本語などはUTF-8のバイト数で数える。空白はそのまま保持する。
Backspaceで修正、Ctrl-Cで中止できる。長すぎる入力や制御文字を含む行は全体を破棄して保存しない。
設定は専用NVS名前空間`network`の1つのblobとして保存し、RTC調整値の`rtc_tuning`とは独立する。
保存済み設定がある場合、menuconfigのWi-Fi・サーバー設定を変えて再ビルドしても保存値を優先する。
通常の書き込みでは保持され、フラッシュ全消去では失われる。NVSは既定で暗号化される
（前節「NVS暗号化とパーティション」を参照）。
保存方式はESP-IDFの[NVS API](https://github.com/espressif/esp-idf/blob/master/components/nvs_flash/include/nvs.h)に従い、
`nvs_set_blob`後に`nvs_commit`する。

入力検証とシリアル行処理のPCテストは`tests/network_settings_test.c`。
MSVCの開発者コマンドプロンプトでリポジトリ直下から実行できる。

```bat
cl /nologo /W4 /O2 /Feesp_webcam\.tools\network_settings_test.exe /Foesp_webcam\.tools\ esp_webcam\tests\network_settings_test.c esp_webcam\main\network_settings_values.c
esp_webcam\.tools\network_settings_test.exe
```

2026-11-02に上記PCテストと受信・描画・中継13テストが成功。
ESP-IDF v6.1 / ESP32-S3でビルド・リンク成功（app 1,262,080 bytes、0x134200、アプリ領域18%空き）。
同日、COM3のPico debugprobe UART経由でESP32-S3 rev.0.2へ書き込み、
bootloader・partition table・appのハッシュ照合に成功した。書き込み後はbootloaderで待機しているため、
IO0のGND接続を外してENリセットする。NVS保存・再起動後のWi-Fi/サーバー変更は未検証。

同日、ログで入力欄が流れる問題を修正。設定画面ではUARTログを止め、`logs on`とEnterで切り替える。
入力エコーとパスワードのマスク表示を追加した。改修後はビルド・リンク成功
（app 1,262,816 bytes、0x1344e0、アプリ領域18%空き）。
`tests/test_network_console.py`は実際のコンソール処理をUART/NVSのPC用アダプターで動かし、
ログ切替、CRLF、通常入力の表示、秘密入力のマスク、キャンセル、NVSからの再読込みを検証する。
MSVC開発者コマンドプロンプトで`python esp_webcam/tests/test_network_console.py`を実行し、成功した。

Device Authorization とクラウド設定のPCテストは次のとおり。

```bash
cc -std=c11 -Wall -Wextra -Werror -I esp_webcam/main \
  -I esp_webcam/managed_components/espressif__cjson/cJSON \
  esp_webcam/tests/cloud_auth_test.c esp_webcam/main/cloud_settings_values.c \
  esp_webcam/main/device_auth_values.c esp_webcam/main/cloud_auth_logic.c \
  esp_webcam/managed_components/espressif__cjson/cJSON/cJSON.c -lm -o /tmp/cloud_auth_test
/tmp/cloud_auth_test
python3 esp_webcam/tests/test_serial_protocol.py
```

`cloud_auth_test`はcloud設定v1/v2移行、IdP URL検証、event_accessのクレーム検証、
Device Authorization応答の解析、承認・Refreshローテーション・再承認の状態遷移、
event_accessが更新されたときのFirebase再サインイン判断、Firestore 401/403の再発行判断、
JSON出力にトークンを含めないことを確認する。`test_serial_protocol.py`はJSON Linesの
`config`/`status`/`device_authorization`を疑似UARTで実行し、検証エラー、JSONのみの応答、
Wi-Fiパスワードの非表示、設定保存と再起動を確認する。

シグナリング経路のトークン再発行順を確認する回帰テストは次のとおり。

```bash
cc -std=c11 -Wall -Wextra -Werror -I esp_webcam/tests/stubs -I esp_webcam/main \
  -I esp_webcam/managed_components/espressif__cjson/cJSON \
  esp_webcam/tests/cloud_signaling_test.c esp_webcam/main/cloud_auth_logic.c \
  esp_webcam/managed_components/espressif__cjson/cJSON/cJSON.c -lm \
  -o /tmp/cloud_signaling_test
/tmp/cloud_signaling_test
```

`cloud_signaling_test`は`cloud_signaling.c`をホスト用スタブとリンクし、
event_access更新後の再発行で`firebase_refresh`を呼ばないこと、
IDトークン期限切れ間際は`firebase_refresh`を使うこと、
Firestore 401/403でカスタムトークンを再発行して同じ要求を1回だけ再試行すること、
トークン値がログに出ないことを確認する。

2026-11-03のPhase 6変更では、`cloud_auth_test`と`test_serial_protocol.py`、
既存の`network_settings_test.c`が成功した。
DockerのESP-IDF v6.1（`espressif/idf:v6.1`）で`idf.py build`が成功し、
appは1,423,424 bytes（0x15b840）、アプリ領域0x177000に対して0x1b7c0（7%）空き。
実機の書き込み・Device Authorizationの実IdP接続は未実施。
ログ表示修正版も同じCOM3経由で書き込み、全イメージのハッシュ照合に成功。
IO0のGND接続を外してENリセットした実機で、次を確認した。

- `help`応答とコマンド・SSIDの入力表示。
- 既定のログ停止中に3秒間のUART追加出力0バイト。
- `logs on`の応答、Enterでのログ停止・設定画面復帰、再停止後3秒間の追加出力0バイト。
- Wi-Fi・サーバーの入力欄、パスワードの`*`表示、Ctrl-Cでのキャンセル。

検証ではネットワーク設定を保存していない。確認後はログ停止状態に戻し、COM3を解放した。

## 開発中のWebRTC高速化版（2026-11-01）

最新目標は1280×720カラー・静止時も6fps以上。既定は10fps受信・10fps送信。改善版はWebRTC DataChannelで映像を送り、
HTTP 80/81でカメラ診断とMJPEGを同時に提供する。起動元は`mjpeg_main.c`で、
同じカメラの最新JPEGを独立したシーケンスで購読するため、MJPEGとWebRTCがフレームを奪い合わない。
WebRTCは`main.c`の`webrtc_start()`から起動する。診断用MJPEGを同時再生するとWi-Fi帯域を使うため、
WebRTCが開通するとMJPEG送信は自動で休止して帯域を優先する。

ESPはビューアー画面を配信しない。PCなどで`viewer-server`を起動し、ブラウザはそこから
画面とスクリプトを受け取る。ESPとビューアーはそのNodeサーバーを介してSDPを交換し、
ICEはSTUNでサーバー再帰候補を集める。シグナリングはHTTPポーリングで、ESPが
`/api/esp/offer`を取得してanswerを`/api/esp/answer`へPOSTする。

- JPEG内部のマーカーと寸法・3色成分を検査し、EOIより後ろのUSB/DMA整列データは長さを問わず無視する。
  返すJPEGはEOIまでとする。JPEG本体の欠損やセグメント長異常は修復扱いで通さない。
  破棄理由は`/status`の`invalid_*`カウンタで確認できる。
- 同一CコードをMSVCでPC用DLLにビルドし、実JPEG・0〜16バイト末尾・破棄理由・APP内の偽EOI・切断・寸法違い・長さ異常でテスト済み。
- 公式JPEG例と同様にESP側から`jpeg` DataChannelを作る。ブラウザは`control`でSDPを用意し、
  `ondatachannel`で映像チャンネルを受け取る。unordered / maxRetransmits=0（Kconfigで0〜3）。
- 静止時の低fps制御・同一JPEG抑制・CRC32計算・ブラウザ動き検出を撤去した。
  フレームは位相を維持して10fps目標で送り、遅れが次の周期へ累積しないようにする。
- 最大4断片/4msのバースト送信と、送信満杯時のパケット再利用で待機とコピーを減らす。
  下位UDPの20ms待機はプロジェクト側weakフックで1ms×最大2回に短縮し、失敗は上位へ返す。
- 解像度は**1280×720（HD）固定**。受信は最大3フレーム分の断片順序入れ替えに対応し、
  デコード待ちは最新1枚のみ保持する。ACKは実際にcanvasへ描画したフレームだけ返す。
- 速度優先コンパイル、lwIP IRAM配置、受信mailbox拡大、Wi-Fi TXバッファ48・BA窓12を設定。
  `/rtc-status`の`send_fps`・`display_fps`は直近約5秒の実測カウンタ差分、
  `effective_fps`は設定値。実機測定の最新結果は下記のプロファイル記録を参照。
  詳細とメモリ負担は[高速化メモ](../docs/webrtc-performance-20261101.md)を参照。
- 実機比較によりChaCha20-Poly1305優先（AES-GCMへフォールバック）、WebRTCタスクをCPU1へ配置、SCTP送信キャッシュ128KiB、
  HTTP/ICE併用のためソケット上限20。再圧縮はまだ追加していない。
- 直接LAN接続では、`.local`ホスト候補を到達可能なIPv4へ展開する。ビューアーをlocalhostで
  開いた場合はESPと同一サブネットのPC側IPv4を使う。`a=candidate:`行の接頭辞は保持する。
  ブラウザのプライバシー設定は変更しない。リバースプロキシやLAN外接続はこの処理の対象外。

2026-10-31にDataChannel経由のカラーHD映像を実機で確認した。`/rtc-status`は
`peer_state:10`、`channel_open:true`、`sent_frames:105`、`acked_frames:79`、
`last_ack_rtt_ms:120`、`effective_fps:2`、`client_motion:3`。ブラウザ表示も確認済み。

変更前の2026-10-31版はESP-IDF v6.1でビルド・リンク成功（app 1,188,272 bytes、0x1221b0）。
JPEG解析実コードのPCテスト（末尾0〜16バイト・破棄理由を含む）と
DataChannel受信・断片復元・動きスコア・シグナリング中継・候補展開のテスト9件が成功。
libwebrtc M106ベースの`@roamhq/wrtc` 0.10.0による検証クライアントも用意し、起動を確認した。

2026-11-01版もESP-IDF v6.1 / ESP32-S3でビルド・リンク成功。
appは1,161,584 bytes（0x11b970）、アプリ領域24%空き。
最終ELFの`udp_socket_sendto`が独自の強いシンボル（`T`）であることも確認した。
受信・描画・中継テスト13件と、実コードのC送信周期／UDP再試行テストが成功した。
この時点では実機不在だった。後述の同日実機確認・30秒測定を実施したが、長時間動作は未検証。

同日追記：COM3の`Debugprobe on Pico (CMSIS-DAP)`のUART経由で、
ESP32-S3 rev.0.2へ上記1,161,584-byte版を書き込み、bootloader・partition table・appの
ハッシュ照合に成功した。WindowsではDTR有効が必要で、esptoolの読み取りタイムアウト変更に
伴うCDC UART再設定時に同期応答が破損した。物理ポートのタイムアウトを固定し、
ソフト側で読み取り期限を管理する`tools/flash_debugprobe.py`で接続・書き込みに成功した。
IO0のGND保持解除後のENリセットで通常起動、Wi-Fi接続（今回のDHCP IPは
`192.168.137.145`）、1280×720 MJPEG受信10fps開始、診断API・WebRTC開通を確認した。
診断時は受信633枚すべて有効JPEG、USB転送エラー0・バッファ超過0だった。
その後30.048秒間のカウンタ差分では送信208枚・描画ACK208枚、平均6.922fps。
約5秒ごとの描画ACK fpsは7.715 / 7.792 / 7.582 / 7.830 / 5.206 / 5.400。
期間中の送信エラー・送信途中破棄は0、DataChannelは開通を維持したが、
**5秒区間で6fpsを下回り、常時6fps以上の要件は未達**。長時間性能も未確認。
このfpsはブラウザのcanvas描画後ACKをESPで数えた値で、物理ディスプレイの走査の測定ではない。

Picoの標準debugprobe UARTはGP4=TX→ESP GPIO44/RXD0、GP5=RX←ESP GPIO43/TXD0、GND共通。
IO0をGNDに保持してENリセット後、ESP-IDFのPython環境で次を実行する。

```powershell
python tools/flash_debugprobe.py --port COM3 --check
python tools/flash_debugprobe.py --port COM3
```

既存`build/`のイメージを使用するため、ソース変更後は先に`idf.py build`を行う。
書き込み後はIO0のGND接続を外してENリセットする。補助ツールはeFuseを変更しない。

同日追加の実機プロファイルでは、近いJPEGサイズでAES-GCMが平均6.51fps、
ChaCha20-Poly1305が平均6.98fpsだった。暗号ライブラリへ明示的に速度優先の`-O3`を適用し、
4断片/4msの設定で3分測定した結果は平均6.74fps、最低5秒区間6.16fps。
大きなバーストは速度が上がらず内部RAMの余裕を減らしたため採用しない。
この条件での測定結果であり、全被写体・電波条件での6fps維持を保証するものではない。
続くUDP受信nowait版の3分測定は平均6.82fps・固定5秒の最低6.20fpsだったが、
ローリング5秒では最低5.4fps、最大描画間隔617msがあり、常時6fpsの要件は未達。
詳細・処理別の時間・実験設定は[実機プロファイル](../docs/webrtc-profile-20261101.md)に記載した。

`/perf`で実際の暗号方式・CPU周波数・FW SHA・内部RAM・TLS/UDP/送信処理時間を取得できる。
`POST /rtc-config?cipher=2&chunks=4&burst_us=4000&fast_rx=1`で比較設定を変更し、
`&persist=1`でNVSへ保存する。暗号変更は次の接続から反映する。
`/rtc-status`は`cipher_pref`・`burst_chunks`・`burst_us`・`fast_rx`・`tuning_saved`も返す。
UVCの受信停止監視を追加し、復旧試行数を`/status`の`stream_restarts`へ出す。

製品と同じ受信コードで実デコード・canvas描画を測るWindows用Chrome測定ツールを追加した。
viewer-serverを起動し、リポジトリ直下から実行する。

```powershell
$env:ESP_URL='http://192.168.137.145'
$env:SECONDS='180'
$env:RESULT_FILE='esp_webcam/.tools/benchmark.json'
node viewer-server/tools/benchmark-browser.mjs
```

UARTログはESP-IDFのPythonから
`python esp_webcam/tools/monitor_debugprobe.py --port COM3 --seconds 60 --output esp_webcam/.tools/uart.log`
で取得する。DTRを有効に保ち、ローカルsdkconfigのWi-Fi認証情報を伏せて保存する。
フラッシュとログ監視でCOM3を同時に開かない。

## 現在の動作：HTTP MJPEGとカメラ診断

2026-10-31、カメラ受信の切り分けのため既定ビルドを`main/mjpeg_main.c`へ切り替えた。
WebRTCの送信処理は`main/main.c`にあり、HTTP 80の診断サーバーと同じビルドから起動する。
受信画面はプロジェクトルートの`viewer-server/`（Node.js、追加パッケージ不要）から配信する。
Wi-Fiのローカル設定、既定720p MJPEG・10fps受信、
VID:PID=349C:3307は引き継ぐ（fpsはKconfigで変更可能、解像度はHD固定）。

- 画面：`http://<ESPのIP>/`（**httpsではなくhttp**）
- カメラ診断JSON：`http://<ESPのIP>/status`
- USB・UVCの直近ログ：`http://<ESPのIP>/usb-log`（USB系のみ）
- WebRTC/DTLSの直近ログ：`http://<ESPのIP>/log`（`DTLS`・`esp_peer`・`SCTP`・`ICE`・`webcam:`）
- MJPEG映像：`http://<ESPのIP>:81/stream`

`/status`はUSB機器数、UVC検出回数、オープン結果、JPEG受信件数・寸法・サイズ・受信fps・
送信目標fps・動きしきい値、不正JPEGの理由別カウンタ、バッファ超過・転送エラーを返す。
`/rtc-status`はDataChannel接続、送信数、重複排除・stale破棄・ACK統計に加え、
`effective_fps`（送信目標fps）、`client_motion`（旧動き検出の互換フィールド、現在-1）を返す。
USB機器数とUVC検出は別の段階である。
UVC検出回数は起動後の累計なので、現在接続されているかはUSB機器数・状態・最終受信時刻も見る。
MJPEG用と画面・診断用にHTTPサーバーを分け、配信中も診断を取得できる。
映像閲覧は1台を想定。カメラがなくても診断画面は開き、映像は待機する。
カメラを途中で外すと最後の画像が残ることがあるが、状態欄は受信停止を示す。
以前の「ICE/DTLS成功後にSCTP/DataChannelが開かない」問題は、未送信バグとサーバーの
候補展開バグを修正して解消済み。切り分けには`/log`を使う。

### ビューアーとシグナリングサーバー（旧方式・現在は非使用）

以下は旧方式の記録である。現在は tolo-edge-bridge の Firestore シグナリングを使うため、`viewer-server/` は起動しない。

`viewer-server/`をPCで起動し、ブラウザで`http://<PCのIP>:8080/`を開く。
サーバーは画面・`receiver.js`・SDP中継・STUN設定を提供する。

```powershell
Set-Location <リポジトリ>\viewer-server
$env:STUN_URL='stun:stun.l.google.com:19302'
npm start
```

ESPのmenuconfigでは`ESP webcam`の`Node signaling server base URL`に
`http://<PCのIP>:8080`、`STUN server URL`に同じSTUNを設定する。空にするとWebRTCを無効化できる。
HTTPSを使う場合は`https://<ホスト>`を指定する（ポート省略時は443、CAバンドルで検証）。
保存済みのシリアル設定がある場合、サーバー変更・無効化は上記の`signaling`コマンドで行う。
ビューアーの`.local`ホスト候補は、サーバーが到達可能なPC側IPv4へ置換してESPへ渡す
（localhost接続時はESPと同一サブネットのアドレスを選ぶ）。`a=candidate:`行は保持される。
詳しいAPIと環境変数は[viewer-server/README.md](../../viewer-server/README.md)を参照。

この確認版はESP-IDF v6.1でビルド・リンク成功（app 921,296 bytes）、
COM11のESP32-S3へ書き込み、bootloader・partition table・appのハッシュ照合に成功した。
ブラウザ用JavaScriptの構文確認も実施済み。カメラの認識・実受信結果は通常起動後に確認する。

通常起動後、Wi-Fi接続と`/status`の応答を確認した。複数回の確認でUSB機器数0、
UVC検出0、JPEG受信0、オープン結果`ESP_ERR_NOT_FOUND`だった。
現在はUSB機器の列挙が成立しておらず、カメラの給電・実際の接続先・データ線の確認が必要。
カメラ形式の不一致やJPEG転送帯域が原因と確定した状態ではない。
実機の接続先はユーザー確認により自作基板のJ1（USB-A）。KiCad CLI 10.0.8で
回路図からXMLネットリストを再出力し、現行PCBのパッドとも照合した。
J1.1=J2のVBUS由来の+5V、J1.4=GND、J1.2=D−→U1.13（GPIO19）、
J1.3=D+→U1.14（GPIO20）。ソフトウェアの内蔵USB PHY割り当てと一致する。
設計ファイル上の接続確認であり、実装品の導通・VBUS電圧・はんだ状態を確認したものではない。
モジュールの端子根拠は[公式データシートv1.8、表3-1](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)。
J1フットプリントの端子配置は[GCT USB1046図面（Mouser配布）](https://www.mouser.com/datasheet/2/837/USB1046-1948751.pdf)を参照した。
ユーザーの実測ではカメラ接続時のJ1 VBUS-GND間は4.97Vだった。
ソフトウェア設定を再確認すると、USB制御転送上限が既定の256 bytesのままだった。
採用ドライバの`host/usb/src/enum.c`はconfiguration descriptorがこの上限を超えると
`ESP_ERR_INVALID_SIZE`で列挙を拒否する。公式UVC basic_uvc_stream例に合わせ、上限を
4096 bytes、ハードウェアFIFOをBias INへ修正した。
根拠：[固定コミットの公式UVC既定設定](https://github.com/espressif/esp-usb/blob/2e2032bb861f40b3d9e1e513e03856c8daa337a1/host/class/uvc/usb_host_uvc/examples/basic_uvc_stream/sdkconfig.defaults)。
対象カメラのdescriptor全長と、この設定が認識失敗の原因だったかは修正版での再確認が必要。
修正版の`/status`には`usb_control_max`と`usb_fifo_bias`を出し、実機へ反映された設定も確認できる。
4096-byte / Bias IN修正版もビルド・リンク、COM11への書き込みと全領域のハッシュ照合に成功した
（app 922,080 bytes）。その後の通常起動でUSB機器1台・UVC検出1件、
ストリーム開始`ESP_OK`と1280×720 JPEG受信を確認した。MJPEGストリームから受け取った
3枚をPCでデコードし、1280×720・yuvj420pとして正常に展開できた。
ユーザーもブラウザ上の映像表示を確認した。

10秒間の測定依頼に対し、診断APIのフレームカウンタ差分を実測10.137秒で計測した。
USB受信101枚（9.964fps）、有効JPEG25枚（2.466fps）、無効判定で破棄76枚。
期間内のUSB転送エラー0、バッファ超過0。この値はESPでの受信・検査カウンタであり、
ブラウザの描画イベントを直接計測した値ではない。
無効判定の多くは、EOI後の整列パディングを3バイトまでしか許さなかった検査によるものと推定した。
USB/DMAが16バイト境界へ整列すると末尾は0〜15バイトになり、3バイトまでなら4/16だけが通るため、
有効率がほぼ25%になる説明がつく。現在のソースはEOI後を長さ無制限で無視するよう修正済みだが、
実機での有効率改善は未測定。

書き込み時はカメラを外し、IO0をGNDにしてENリセット後にPCのUSBへ接続する。
書き込み終了後は**IO0のGND接続を外し、通常起動する**。USBホスト開始後は同じ内蔵PHYの
USB Serial/JTAGポートがPCから見えなくなるため、再書き込みには再び書き込みモードへ入る必要がある。
動作中の詳細ログはUART0で確認する。電源・GND・カメラVBUSの注意点は以下の基板条件を参照。

以下は、保持しているWebRTC版の構成・開発記録である。現在の有効なエントリーポイントとURLは上記を使用する。

## WebRTC版の構成と開発記録

ESP32-S3-WROOM-1-N8R8（Flash 8MB / Octal PSRAM 8MB）用。
USB UVCカメラのカラーMJPEGからJPEGを取り出し、同一LANのブラウザ1台へ送信する。
目標は **1280×720カラー・6fps以上の維持**。6fpsは上限ではなく、余裕があれば高速化する。
カメラのJPEGを再圧縮せず送るため、カラーはそのまま保持する。
対象カメラはWindowsで「HD video」と認識された **349C:3307**。
PCでFull Speed・720p MJPEGのdescriptorを確認済み。ESP上での動作と6fpsの達成は未確認。

## 構成

```text
USB UVC MJPEG camera → USB Full Speed host → latest JPEG in PSRAM
 → JPG1分割パケット → SCTP / DTLS / ICE → RTCDataChannel
 → JPEG復元 → ブラウザJPEGデコード → canvas
```

映像トラック、H.264エンコード、音声は使わない。ブラウザがofferを作り、Nodeの
`viewer-server`がESPへ中継し、ESPがanswerを返す。ESPはHTTPポーリングでofferを取得する。
ICE収集完了後のSDPを交換し、trickle ICEは使わない。
ICEはSTUNでサーバー再帰候補を収集する。TURN中継は未実装で、同一LAN・
端末間通信が可能なWi-Fiが前提。インターネット越しの接続、認証付きの
複数ユーザーサービスは対象外。STUNが使えない場合はホスト候補のみで接続する。

## カメラ・基板の成立条件

- **Full Speed（12 Mbit/s）で1280×720 MJPEGを広告・送信できるUVCカメラ**が必要。
  USB 2.0/HD対応の表示だけでは判断できない。High Speed専用720pモードは利用できない。
  対象カメラは1280×720の10fps/15fpsを広告する。
- 解像度は**1280×720（HD）固定**で、低解像度へ落とす設定は持たない。
  初期設定は**カメラ受信10fps・Wi-Fi送信10fps**。
  VID/PIDも349C:3307を初期値とし、別カメラではmenuconfigで変更する（0はワイルドカード）。
  `UVC capture FPS`はカメラが実際に広告する値（10または15）へ合わせる。
  採用ドライバは離散frame intervalの完全一致を要求するため、7や8など広告にない値は開けない。
  送信目標は`Send target FPS`（既定10、設定範囲6〜30）で決める。
  静止時も送信を下げず、実際のfpsは受信・描画カウンタで確認する。
  解像度や形式を自動で下げず、720p MJPEGを開けなければログに失敗を出す。
- JPEGは1枚256KiBまで。例えば100KiB×6fpsは約615kB/s、JPEGだけで約4.9Mbit/s。
  200KiB×6fpsは約1.23MB/s、約9.8Mbit/s。USB/UVC、SCTP/DTLS、Wi-Fiのオーバーヘッドが加わる。
  256KiBは安全なメモリ上限で、6fpsの性能保証ではない。細かい模様・暗所ノイズでサイズが増える。
  対象カメラの720pのframe descriptorは最大1,843,200 bytesを申告するが、これは実際の圧縮JPEGサイズの測定値ではない。
  10fpsのUSB入力では50KiB/枚でも約512kB/sになる。出力を6fpsへ間引いても、USB入力帯域は減らない。
  このため、descriptorの対応だけではS3での成立を確定できない。
- JPEG品質のカメラ制御は未実装。大きすぎる場合はカメラ側設定や別カメラで調整する。
  YUY2/RGBをUSB受信してESP上でJPEG圧縮する経路は実装していない。
- GPIO19=D−、GPIO20=D+。内蔵USB PHYをホストが使うため、**書き込み・ログはUART0**を使う。
  3.3VロジックのUSB-UARTをTXD0/RXD0/GNDに接続し、GPIO0とENでダウンロードモードに入る。
  自作基板にはこれらの操作・端子経路が不足するため、先に開発ボードまたは治具を用意する。
- カメラVBUSには5V電源が必要。既存基板のJ1は5V直結で電流制限・逆流保護が未整備。
  電源容量、3.3V低下、レギュレータ発熱、カメラ突入電流を確認する。
  J1にA-AケーブルでPCを接続する書き込みは行わない。
  回路の残課題は[基板レビュー](../docs/pcb-review-20261026.md)を参照。

## 固定した開発環境

2026-10-31に取得した公式ソースを使用する。

| 依存 | 版 / コミット |
| --- | --- |
| ESP-IDF | v6.1.0 / `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Xtensa GCC | `esp-15.2.0_20251204` |
| esp_peer | 1.5.7 / esp-webrtc-solution `38b175077967109e764d434d680e641d9e87b23c` |
| usb_host_uvc | 2.6.0 / esp-usb `2e2032bb861f40b3d9e1e513e03856c8daa337a1` |
| usb | 1.6.0 / 同じesp-usbチェックアウト |
| esp_libsrtp | 1.0.0、`dependencies.lock`で固定 |

`vendor/` の公式ソース・プリビルドライブラリとライセンスをそのまま参照する。
空白を含むパス用のリンク引数補正はプロジェクト側の`CMakeLists.txt`で行う。
`vendor/`の2リポジトリはルートの設計リポジトリでsubmoduleとして固定する。

初回取得は設計リポジトリのルートで以下を実行する。

```powershell
git clone --recursive <リポジトリURL> esp_webcam
# 既存チェックアウトの場合
git submodule update --init --recursive
```

`.tools/esp-idf`はsubmoduleにせず、通常のIDFインストールまたはローカル取得を使う。

### リポジトリ管理

- ルートの設計リポジトリにはKiCad設計、`docs/`、`exports/`、ファームウェアをコミットする。
- `vendor/`の2リポジトリはsubmodule。親リポジトリが記録するのはコミットSHAのみなので、
  依存を更新するときはsubmodule側でcheckoutし、親で`git add esp_webcam/vendor/...`する。
- `.tools/`（ESP-IDFローカル取得）、`build/`、`managed_components/`、`sdkconfig`、`certs/`、
  KiCadのバックアップ・自動保存・ロックファイルは追跡しない。
- Wi-Fi認証情報は`sdkconfig`にのみ置き、共有しない。

## ビルドと起動

1. ESP-IDF v6.1のPowerShell環境を開く。ターゲットは`esp32s3`。
2. このプロジェクトパスには空白・括弧がある。Windowsでは空いているドライブ文字で
   プロジェクトを割り当てるとツールのパス問題を避けやすい。
   例（Q:が空いていることを確認する）：

   ```powershell
   subst Q: 'C:\Users\haruj\ownCloud (2)\Personal\ProjectWS3\esp_webcam\esp_webcam'
   Set-Location Q:\
   $env:PYTHONUTF8='1'
   $env:KCONFIG_REPORT_VERBOSITY='quiet'
   ```

3. ターゲットを設定しmenuconfigを開く。

   ```powershell
   idf.py set-target esp32s3
   idf.py menuconfig
   ```

4. `ESP webcam`でSSID・パスワード・カメラ対応fps・送信目標fps・
   `Edge Bridge base URL`・`STUN server URL`を入力する。Firebase・IdP・edgeDeviceId は
   書き込み後に Web Serial またはシリアル（`signaling` / `cloud`）で設定する。
   event_access は Device Authorization で自動取得する。
   保存済みNVS設定があればそちらを優先する。
   認証情報はローカルの` sdkconfig `とビルド成果物に含まれるため、これらを共有しない。
   ソース・既定値・実行ログには認証情報を記載しない。
5. ビルドして書き込む。COM番号は実際のポートへ置き換える。

   ```powershell
   idf.py build
   python tools/flash_serial.py --port COM5 --check
   python tools/flash_serial.py --port COM5
   ```

   Pico debugprobe の UART 経由は `tools/flash_debugprobe.py` を使う。
   `idf.py -p COM5 flash monitor` でも書き込める。

6. 給電・USBホスト配線を確認した上で、対象カメラをPCから外してESP側のホストポートへ接続する。
   観測ページ `.../observation/<観測ページのedgeDeviceId>?camera=<ESPのedgeDeviceId>` を開き、
   映像ソースを開始する。ESP は起動後にセッションを検出して answer を返す。
   再接続は観測ページの「停止」→「開始」。同時接続は1台。
7. 作業後はQ:以外へ移動して`subst Q: /D`で割り当てを解除できる。

今回のビルド用に`.tools/esp-idf`も取得した。通常はインストール済みIDF v6.1を使ってよい。
ローカル確認補助`.tools/build-local.ps1`はこのPCの既存ツール配置向けで、配布用手順ではない。
このPCでは上記のQ:割り当て後、プロジェクト直下で
`& ./.tools/build-local.ps1 menuconfig`、`& ./.tools/build-local.ps1 build`、
`& ./.tools/build-local.ps1 -p COM5 flash monitor`でも操作できる。
`certs/`、`sdkconfig`、`build/`、`.tools/`はGit除外対象。
シグナリングは event_access で認可するが、ESP の HTTP 診断エンドポイントと
DataChannel 自体には追加の視聴者認可を実装していない。利用対象は信頼できるLAN内とする。

## 分割送信と遅延制御

- JPEGの最大長256KiB、1メッセージは最大12,020 bytes（ヘッダー20＋JPEG断片12,000）。
- ヘッダーは`JPG1`、uint32 LEのframe ID、JPEG全長、断片offset、ESP送信開始時刻ms。
  端末時計は同期していないため、この時刻をブラウザとの片道遅延として表示しない。
- ESPが作る`jpeg`はunordered / maxRetransmits=0（既定）。`DataChannel retransmits`で0〜3に変更できる。
  受信側は断片の順序入れ替え・重複を許容し、不正サイズ・未完成フレームを破棄する。
- UVCコールバックは最新1枚をPSRAMへコピーして直ちにバッファを返す。
  送信中もカメラ受信を止めず、古い待機フレームを最新フレームで置き換える。
- 送信は`Send target FPS`（既定10fps、最低6）で位相を維持してペーシングする。
  ACKは描画数と応答時間の統計に使い、ACK待ちで送信を止めない。
  未送信・送信途中のJPEGは250msを超えたら破棄し、最新フレームへ移る。
- 各イベントループで最大4断片/4msを送り、ICE/SCTPの処理を継続する。
  WOULD_BLOCKではパケットを再コピーせずに再試行する。
- 静止時も同じ送信目標を使う。同一フレーム抑制・CRC32計算・動きスコア画像読み戻しは行わない。
- ブラウザの断片組立は最大3フレーム（最大768KiB）まで。新しいフレームの断片が先着しても
  直前のフレームを即座には捨てず、完成済みIDより古いものは表示しない。
  デコードは同時に1枚、待ちは最新1枚だけとし、canvas描画後に`ACK:<frame ID>`を返す。
- ブラウザは受信JPEG（1280×720）をcanvasへ描画する。追加のデコードライブラリは不要。
- ESPはACKを参考統計として扱い、2秒以上応答がなければ警告を出すが、送信は止めない。
- UVCフレーム3枚＋最新1枚＋送信用1枚で約1.25MiB、ほかにUSB URB・TLS・SCTP・Wi-Fi等が必要。
  起動時に4MiB以上のPSRAMを確認する。最大JPEGの全バッファはPSRAMへ明示的に配置する。

## 検証手順

2026-10-31のPC上での確認結果：ESP-IDF v6.1 / ESP32-S3向けビルド・リンク成功、
`build/esp_webcam.bin`生成（1,172,576 bytes）。空のSSID設定でもUSB/UVC・WebRTCの実装が
リンクされていることをELFのシンボルでも確認した。受信側テスト4件成功。
カメラのFull Speed・対応モードはUSB descriptorを読み取り確認済み。
書き込み、ESP上のUVC受信、ブラウザとのWebRTC接続、カラー映像、実測fpsは未検証。
依存側CMakeの非推奨・private include警告は残るが、コンパイル・リンクのエラーはない。

同日追記：接続されたESP32-S3 rev.0.2（PSRAM 8MB、Flash 16MB）へ、COM11の
USB Serial/JTAG経由で上記ファームウェアを書き込み、bootloader・partition table・appの
書き込み後ハッシュ照合に成功した。Flash設定は8MBのままで、実容量の範囲内を使用する。
Wi-Fi設定は空のため、配信を開始するにはローカルで設定して再ビルド・再書き込みが必要。
この記録は接続された開発機の情報であり、KiCad上のN8R8モジュールの仕様変更ではない。
実行時ログのUART確認、UVC受信とWebRTC接続・実測fpsは引き続き未確認。

さらに同日、指定された2.4GHz Wi-Fi設定をローカルsdkconfigに反映して再ビルドし、
COM11へ再書き込み・全領域のハッシュ照合に成功した（app 1,172,608 bytes）。
PCのホットスポットが有効、2.4GHz、SSIDとパスワードがローカル設定と一致することも確認。
接続・IP取得とHTTPS応答は現時点では未確認。認証情報そのものは本書へ記載しない。

その後、ユーザーがIO0のGND接続を解除して通常起動し、対象MACのWi-Fi接続を確認した。
取得IP `192.168.137.139`で`/`と`/status`がHTTP 200を返し、診断画面の内容も一致した。
このIPは今回のDHCP割り当てであり固定ではない。GPIO0をLowに保持したまま起動すると
書き込みモードになるため、通常運用ではLowに保持しない。

2026-10-31の最終確認では、USB機器1台・UVC検出1件、`frames_received`と`valid_jpegs`が
615枚で一致し、`transfer_errors:0`・`buffer_overflows:0`。`/rtc-status`は
`peer_state:10`（DataChannel開通）、`channel_open:true`、`sent_frames:105`、
`acked_frames:79`、`last_ack_rtt_ms:120`、`effective_fps:2`、`client_motion:3`。
ブラウザでカラーHD映像を確認した。開通しなかった原因は、`peer_task`で`size = n;`が
抜けてJPEGを送っていなかったことと、Nodeサーバーが`.local`展開時に
`a=candidate:`接頭辞を落として`127.0.0.1`へ置換していたこと。両方修正済み。
長時間のfps・遅延・再接続は未測定。

Windowsでのカメラdescriptor確認結果は[カメラ調査メモ](../docs/camera-349c-3307.md)を参照。
再確認は`python tools/probe_windows_camera.py`。USBハブ情報を読めるWindows端末で実行する。
映像の撮影・取得、ドライバ変更、USBポートのリセットは行わない。

ブラウザの受信処理とシグナリング中継はPC単体で検証できる。

```powershell
Set-Location <リポジトリ>\viewer-server
node --test tests/*.test.mjs
```

実機で次を順に記録する。

1. 起動ログでFlash・PSRAM、カメラのUSB descriptorと`<width>x<height> MJPEG capture started`を確認。
   カメラ未接続・非対応なら5秒間隔で再試行する。1280×720がFull Speedで出ない場合は
   解像度を落とさず、カメラを再選定する。
2. 色のある被写体でカラー、1280×720の寸法、向き、破損がないことを確認する。
3. 明所・暗所・動き・細かい模様を含めて各10分、平均fps、JPEG bytes/枚、Mbit/s、欠落を記録する。
   静止時も動きがある間も6fps以上を維持することをブラウザの直近fpsと
   `/rtc-status`の`display_fps`で確認する。`effective_fps`は設定値であり達成判定には使わない。
   256KiB上限超過なし、リセットなし。性能に余裕があれば`Send target FPS`を上げて再測定する。
4. 画面に時計を映して撮影し、入力から表示までの遅延を外部から測る。
   ブラウザの「分割受信ms」は断片の組立時間のみで、エンドツーエンド遅延ではない。
   直近5秒ごとの描画数が30枚以上であることも記録する。
5. Wi-Fi電波低下、ブラウザ切断・再接続、カメラ抜き差しで、古い映像の蓄積・クラッシュがないか確認。
6. ESPの3.3V瞬時低下、レギュレータ温度、5V電流とカメラ突入電流を測定する。

接続不能時はAPの端末分離・VLAN・ファイアウォール・同一LANを確認する。
`viewer-server/last-offer.sdp`でESPへ渡した候補を確認し、`/log`でICE/DTLS/SCTPの
失敗段階を切り分ける。`peer_state`が4から8へ即座に進む場合は、有効なホスト候補が
ESPへ届いていない。LAN外はTURN/STUN等の追加設計が必要。

## 根拠資料（2026-10-31参照）

- [esp_peer 1.5.7 API・設定](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/components/esp_peer)：ICE/DTLS/SCTP、DataChannelイベント、WOULD_BLOCK。
- [UVC 2.6.0 API](https://github.com/espressif/esp-usb/blob/2e2032bb861f40b3d9e1e513e03856c8daa337a1/host/class/uvc/usb_host_uvc/include/usb/uvc_host.h)：MJPEG設定、フレーム所有権、切断イベント。
- [公式JPEG DataChannel例](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/solutions/local_jpeg_stream)：JPEG分割送信・ブラウザ表示方式の参考。DVP用の例なのでUVC動作の保証には用いない。
- [ESP32-S3-WROOM-1/1Uデータシート](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)：既存基板レビューで参照したv1.8のN8R8構成とGPIO19/20。
- [USB Host / PHY共有制約](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#external-phy-configuration)。

基板・回路図は変更していないためERC/DRCは今回のソフトウェア変更では実行しない。

### 2026-11-04: 認可処理とHTTPSの回帰修正

Web Serialの`device_authorization`は長さ1のキューへ要求を入れて直ちに戻る。
HTTP/TLS・認可処理・user_codeのJSON出力はcloudタスクで行う。
キュー満杯またはcloudワーカー未起動なら、JSONエラー
`authorization_queue_full_or_unavailable`を返す。認可開始の大きな作業領域はヒープへ置く。

Refreshの`invalid_grant`、grant期限切れ、再承認が必要な状態では、送信をブロックして
peerタスクへ切断を通知する。peerタスクが接続を閉じ、送信途中のJPEG・キュー内のoffer・
保留answerを破棄する。認可タスクからpeer APIは呼ばない。再承認後はFirebaseセッションと
SDPを引き継がず、新しい接続を待つ。

`CONFIG_WEBCAM_ALLOW_INSECURE_HTTP`の既定値は`n`。
IdP・Edge Bridge・Firestore/Auth emulatorはHTTPS URLを設定する。
既存NVSやKconfigにHTTP設定が残っていても、資格情報送信の直前に拒否する。
裸のemulatorホストやsignalingホストもHTTP扱いなので拒否する。
ローカル開発でHTTPが必要な場合だけmenuconfigでこのフラグを明示的に有効にする。
HTTPではRefresh Tokenも平文になる。HTTPSからHTTPへの自動リダイレクトは行わない。

次のコマンドで主要ホスト回帰テストを実行する（C11対応`cc`とPython 3が必要）。

```bash
python3 esp_webcam/tests/run_host_tests.py
```

cloud_auth・network_settingsは本番用と開発用の両設定で実行する。
追加テストは実際のdevice_auth/Firebase HTTP入口とpeerループをホスト用アダプターで動かし、
キュー経由の開始・満杯エラー・user_codeの出力回数・invalid_grant時の送信停止とSDP破棄・
NVS保存失敗時の失効・grant期限切れ・HTTP拒否／開発ビルド時のみ許可を確認する。
実機でのWeb Serial操作、TLS時のスタック余裕、再承認後の映像再接続は別途確認が必要。
