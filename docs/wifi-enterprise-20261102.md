# シリアル設定と802.1X/EAP対応（2026-11-02）

ESP32-S3 / ESP-IDF v6.1向け。従来の`wifi`（通常のSSID・PSK設定）に加え、
公開APIで選択できる802.1X/EAP方式を`eap`から設定する。接続先の認証情報はシリアルで入力し、
NVSへ保存する。設定の変更は`reboot`から反映する。シグナリングの設定方法は従来どおり。

## 対応方式

| `eap`で入力する方式 | 内容 | 通常版 | FAST互換版 |
| --- | --- | --- | --- |
| `tls` | EAP-TLS、クライアント証明書＋秘密鍵 | 対応 | 対応（内部TLSの制約あり） |
| `peap` | PEAP / EAP-MSCHAPv2 | 対応 | 対応 |
| `ttls-eap` | EAP-TTLS / EAP | 対応 | 対応 |
| `ttls-mschapv2` | EAP-TTLS / MSCHAPv2 | 対応 | 対応 |
| `ttls-mschap` | EAP-TTLS / MSCHAP | 対応 | 対応 |
| `ttls-pap` | EAP-TTLS / PAP | 対応 | 対応 |
| `ttls-chap` | EAP-TTLS / CHAP | 対応 | 対応 |
| `fast` | EAP-FAST、事前発行のバイナリPAC使用 | 要別ビルド | 対応 |

通常版はMbedTLSのWi-Fi TLSクライアント、FAST互換版はSDKの内部TLSクライアントを使う。
FASTはSDKが`ESP_WIFI_MBEDTLS_TLS_CLIENT=n`を要求するため、同一イメージでの同時有効化はできない。
WebRTCのDTLS実装は両方ともMbedTLSのまま。SDKの内部TLSはサーバー名検証・CAバンドルに対応しないため、
FAST互換版のTLS/PEAP/TTLSではカスタムCAによるチェーン検証、または明示的な検証なしを選ぶ。
FAST自体は事前に発行されたPACを使用し、自動PACプロビジョニングは無効にする。

通常版ではEnterpriseのリンク方式として`wpa`、`wpa2`、`wpa3`、`suiteb`を選べる。
WPA3はPMFとサーバー証明書検証を必須にし、Suite-B 192-bitはEAP-TLSに限定する。
Suite-B用のCA・証明書・鍵は接続先の要件に合うものを用意する（SDK例ではRSA-3072またはP-384等）。
FAST互換版では`wpa`・`wpa2`のみを公開し、未対応の組み合わせを保存・接続時に拒否する。
`protocols`で実際にそのイメージで使用できる方式を確認できる。

TLSバージョン・内部EAPの詳細はSDKに従う。実験的な`ESP_WIFI_EAP_TLS1_3`は今回のビルドでは有効化していない。
本変更の「全方式」は上表の802.1X/EAP方式を指し、Wi-FiのDPP登録等は含めない。

確認したSDKコミットは`fff9895c82d744c7237be8847347bdd1b07c6643`。
API・制約の根拠は[esp_eap_client.h](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/wpa_supplicant/esp_supplicant/include/esp_eap_client.h)、
[Wi-Fi Kconfig](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_wifi/Kconfig)、
[公式Enterprise例](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/examples/wifi/wifi_enterprise/README.md)。

## 接続設定

115200bps / 8N1で接続し、端末のローカルエコーはOFFにする。
入力中は診断ログを止め、パスワードと秘密鍵の復号パスワードは`*`表示にする。

1. `eap`を入力し、上表の方式を入力する。
2. SSIDを入力する。TLS以外は内部認証のユーザー名・パスワードを入力する。
3. 外部Identityを入力する。空ならユーザー名を使う。TLSではIdentityを明示する。
4. TLSではクライアント秘密鍵の復号パスワードを入力する。暗号化していない鍵なら空にする。
5. Enterpriseリンク方式を選ぶ。空なら`wpa2`。
6. サーバーの信頼方法を選ぶ。空または`ca`はインポートしたCA、`bundle`はSDKのCAバンドル、
   `none`はサーバー証明書の検証なし。WPA3/Suite-Bでは`none`を拒否する。
7. サーバー証明書のDNS名を入力する。省略するとCAチェーンのみを検証する。
   認証サーバー名はネットワーク管理者の指定に合わせる。
8. 証明書の有効期間検証を`on`/`off`で選ぶ。空ならSDKの従来動作と同じ`off`。
9. 必要な証明書・鍵を登録してから`reboot`する。

FASTは手順5以降のTLS検証設定を使わず、WPA2と事前発行PACを使用する。
`show`は方式・Identity・ユーザー名・検証設定と、各パスワードの設定有無だけを表示する。
`wifi`で通常のPSK設定に戻すと、保存済みプロファイルからEAP用パスワードを消去する。
証明書資材は独立して保持するため、不要になったものは`credential-clear`で消去できる。

SSIDは最大32バイト、Identity・ユーザー名は最大127バイト、EAP・秘密鍵パスワードは最大128バイト。
EAPパスワードにはPSKの8文字以上という制約を適用しない。空のEAPパスワードは拒否する。

基板には日時を保持するRTCの実装がないため、起動直後の壁時計は信頼できない。
期間検証を`on`にした場合は、シリアルの`clock`に正しい現在のUTC Unix秒を入力する。
その起動中のEAP再試行から使用する。時計を合わせる前は有効期間検証で失敗し得る。
`off`でもCAチェーン・設定したサーバー名の検証は継続し、期限の検査だけを省略する。

## 証明書・鍵・PACの登録

資材は`ca`（認証サーバーのCA）、`client`（クライアント証明書）、`key`（秘密鍵）、`pac`。
`ca/client/key`はPEM、`pac`はバイナリ。各資材は専用NVS名前空間`wifi_certs`へ保存する。
CA・クライアント証明書のファイルにOpenSSLの可読メタデータが付いている場合は、転送ツールが
PEM証明書ブロックだけを抽出する。チェーン内の複数証明書は順序を保って転送する。
PEMは終端NUL込み最大4096バイト（ファイル本体4095バイト）、PACは512～4096バイト。
SDKのPAC APIは512バイト未満のデータを空PACに置換するため、小さなファイルは保存前に拒否する。

EAPプロファイルを先に設定してから端末を切断し、リポジトリ直下で実行する。
秘密鍵を暗号化している場合は、先に`eap`でその復号パスワードを保存しておく。

```powershell
python esp_webcam/tools/upload_wifi_credential.py --port COM3 ca C:\wifi-certs\ca.pem
python esp_webcam/tools/upload_wifi_credential.py --port COM3 client C:\wifi-certs\client.pem
python esp_webcam/tools/upload_wifi_credential.py --port COM3 key C:\wifi-certs\client.key
# FAST互換版のみ
python esp_webcam/tools/upload_wifi_credential.py --port COM3 pac C:\wifi-certs\client.pac
```

転送ツールは`credential <種類>`を実行し、48バイトずつBase64化して行単位で受領応答を待つ。
ESPはアップロード内容をエコーせず、`.`で完了するまでNVSへ保存しない。Ctrl-Cで中止できる。
PEM/秘密鍵の解析に失敗した場合は旧資材を置き換えない。NVSへの保存・commit失敗も報告する。
ファイル内容・秘密鍵・Base64の文字列はツールの出力へ表示しない。

端末へ再接続して`credentials`で保存有無・サイズを確認し、`reboot`する。
削除は`credential-clear ca`等。接続中のSDKが参照する資材は起動時に独立して読み込んで保持し、
アップロード・削除によって参照中の鍵を解放しない。変更は次回起動から反映する。

NVSは従来の24KiBを使う。大きな資材の複数保存・更新は空き不足で失敗することがある。
その場合は不要な資材だけを消去する。既存ネットワーク設定やRTC調整を全消去する処理は追加していない。
NVS暗号化は未追加なので、実機やフラッシュ読み出しデータの取り扱いには保存した認証情報が含まれる。

## ビルドと互換性

通常版は`esp_webcam/`で従来どおり`idf.py build`。
`sdkconfig.defaults`でEnterprise・MbedTLSのWi-Fi TLSクライアント・Suite-B・CAバンドルを有効化する。
既存のローカル`sdkconfig`でもSuite-Bを有効にした。

FAST互換版は同じディレクトリから次のように別の設定ファイル・出力先でビルドする。
現在の通常版の設定・成果物を上書きしない。

```powershell
idf.py -B ../build-fast -DSDKCONFIG=sdkconfig.fast '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.fast.defaults' build
```

`sdkconfig.fast.defaults`はWi-Fi TLSクライアントを内部実装に切り替え、Suite-Bを無効にする。
生成される`sdkconfig.fast`と`../build-fast/`はGit除外。実機へ書く場合はそのビルドの
bootloader・partition table・appをセットで使用する。
Pico debugprobeの安定化ラッパーでも出力先を指定できる。書き込みモードにして、リポジトリ直下で実行する。

```powershell
python esp_webcam/tools/flash_debugprobe.py --port COM3 --build-dir build-fast
```

ネットワーク設定blobはv2へ拡張した。稼働中のv1 blob（328バイト）は明示的に移行して読み、
SSID・PSK・シグナリングURLを保持する。読み込みだけではNVSを書き換えず、次の保存でv2になる。
v2に保存した後に旧ファームウェアへ戻すと、旧版はv2を読めずビルド時設定へ戻る。
RTC調整の`rtc_tuning`は変更しない。

コンソールのスタックは4096→8192バイトに増やした。設定構造体の拡張に加え、
使用するCA・クライアント証明書・鍵は各最大4096バイトを起動中に保持する。
EAPライブラリ自身のメモリと認証中のCPU負荷は実ネットワークで未測定。

## 検証結果

- 通常版：ESP-IDF v6.1 / ESP32-S3でビルド・リンク成功。app 1,374,928 bytes（0x14fad0）、領域10%空き。
- FAST互換版：ビルド・リンク成功。app 1,357,040 bytes（0x14b4f0）、領域12%空き。
- ELFのシンボルで、通常版はTLS・PEAP・TTLS、FAST版はそれらに加え`eap_peer_fast_register`を確認。
- `tests/network_settings_test.c`：EAP入力条件とシリアル行処理。
- `tests/test_network_console.py`：全公開EAP選択・秘密入力・転送の完了/中止・v1移行・再読込み。
- `tests/test_wifi_auth.py`：通常/FAST両API構成、内部方式の割当て、EAPパスワードをPSKへ渡さないこと、
  CA不足で検証なしへ切り替えないこと、起動時の鍵の保持、資材解析エラーの扱い。
- `tests/test_credential_upload.py`：分割された応答、PEM・バイナリPAC転送、OpenSSLテキスト付きチェーン、失敗時のポート解放の4テスト。
- 既存の受信・描画・中継13テスト成功。

通常版をCOM3のPico debugprobe UART経由でESP32-S3 rev.0.2へ書き込み、
bootloader・partition table・appのハッシュ照合に成功した。IO0解除・ENリセットで通常起動。
実機では次を確認した。

- `protocols`でTLS・PEAP・TTLSの全内部方式がavailable、FASTが別ビルドであることを表示。
- 設定中の3秒間のUART追加出力0バイト、`eap`の入力欄とCtrl-Cによる保存なしのキャンセル。
- 全資材スロットが空であることを確認してから、SDK同梱の公開サンプルCA・クライアント証明書・秘密鍵を
  転送ツールで登録。MbedTLS 4による実解析とNVS保存に成功した。
- SDKのクライアント証明書には可読メタデータが付属していたため、転送ツールのPEM抽出も実機で確認した。
- 確認用の資材をすべて消去し、全スロットが未保存状態へ戻ったことを確認。接続プロファイルは変更していない。

確認後はログ停止状態でCOM3を解放した。FAST互換版はビルド・ELF確認までで、実機へは書き込んでいない。
PCテストのTLS/NVS/UARTはアダプターで置き換えているため、認証サーバーとの相互運用確認ではない。
実ネットワークでの各EAP方式・証明書検証・再認証・WPA3/Suite-B・FAST・映像性能は未確認。
