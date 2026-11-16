# NVS 暗号化とパーティション

Wi-Fi 資格情報、EAP の証明書・秘密鍵、クラウド設定、`event_access`、
Refresh Token を NVS に保存する。既定ビルドは ESP-IDF の NVS 暗号化
（XTS-AES）を使う。

## パーティション

8MB フラッシュ用の `esp_webcam/partitions.csv` を使う。`factory` の
オフセットとサイズは従来の `partitions_singleapp_large.csv` と同じで、
既存のアプリ配置を変えない。

| 名前 | 種別 | オフセット | サイズ | 用途 |
| --- | --- | --- | --- | --- |
| `nvs` | data / nvs | `0x9000` | `0x6000` | 設定・証明書・トークン |
| `phy_init` | data / phy | `0xf000` | `0x1000` | PHY 初期化データ |
| `factory` | app / factory | `0x10000` | `0x177000` | アプリケーション |
| `nvs_keys` | data / nvs_keys | `0x187000` | `0x1000` | NVS 暗号化鍵 |

`sdkconfig.defaults` では `CONFIG_PARTITION_TABLE_CUSTOM=y`、
`CONFIG_NVS_ENCRYPTION=y`、`CONFIG_NVS_SEC_KEY_PROTECT_NONE=y`、
`CONFIG_WEBCAM_NVS_ENCRYPTION=y` を設定する。鍵の読み書きは
`main/nvs_boot.c` が `nvs_keys` を直接扱い、`nvs_sec_provider` の既定方式は使わない。

## 起動時の処理

1. `nvs_keys` パーティションを探す。無ければ警告を出し、暗号化なしで起動する。
2. `nvs_flash_read_security_cfg` で鍵を読む。空なら `nvs_flash_generate_keys` で生成する。
3. `nvs_flash_secure_init` で `nvs` を暗号化して初期化する。
4. 鍵が壊れている、または暗号化初期化に失敗した場合は、警告を出して平文へフォールバックする。
5. `ESP_ERR_NVS_NO_FREE_PAGES` と `ESP_ERR_NVS_NEW_VERSION_FOUND` は
   `nvs_flash_erase` 後に再試行する。

フォールバックを禁止するには `CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION=y` にする。
この設定では暗号化を初期化できない場合に平文へ降格せず、起動を停止する。

アプリの NVS 初期化は必ず `nvs_boot_init()` を通す。`nvs_flash_init()` を直接呼ぶと、
`CONFIG_NVS_ENCRYPTION` に従って別の鍵保護方式を試すため、フォールバックが
平文にならない。

## 既存端末の移行

暗号化へ切り替えると、既存の平文 NVS はそのままでは読めない。更新前に
Web Serial の設定値（Wi-Fi、IdP、Firebase、edgeDeviceId）を控え、
次のいずれかで全消去してから書き込み、再プロビジョニングする。
`PORT` は環境に合わせたポート名に置き換える。

```sh
idf.py -p PORT erase-flash
python -m esptool --chip esp32s3 --port PORT erase-flash
```

シリアルフラッシャーを使う場合も全消去を行う。`nvs_keys` だけを個別に消しては
いけない。鍵を失うと暗号化済み NVS を復号できず、設定・証明書・トークンを失う。

`idf.py flash` は通常、消去を伴わない。暗号化へ切り替える最初の書き込みでは
全消去を明示する。暗号化済みの端末で `CONFIG_WEBCAM_NVS_ENCRYPTION` を無効に
する場合も、全消去と再プロビジョニングが必要になる。

## セキュアビルド

既定ビルドはフラッシュ暗号化を有効にしていないため、`nvs_keys` に
`encrypted` フラグを付けていない。フラッシュ暗号化が無効のまま `encrypted` を
付けると、IDF が鍵の CRC を暗号化して書き込み、通常の読み出しと一致せず、
再起動後に NVS 暗号化を初期化できない。

既定ビルドでは NVS の内容は暗号化されるが、鍵自体はフラッシュ上に平文で残る。
これは開発・互換用であり、物理読み出しに対する資格情報保護にはならない。

本番・セキュアビルドでは `sdkconfig.secure.defaults` を使い、ESP32-S3 の
eFuse HMAC 鍵から NVS 鍵を導出する。HMAC 鍵を先に書き込み、
`CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION=y` で平文フォールバックを禁止する。
以下は `esp_webcam/` で実行する。`PORT` は環境に合わせる。

```sh
# 32 バイトの HMAC 鍵ファイルを用意し、BLOCK_KEY0 へ書き込む
idf.py efuse-burn-key BLOCK_KEY0 hmac-key.bin HMAC_UP
idf.py -B build-secure -DSDKCONFIG=build-secure/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure.defaults" build
idf.py -B build-secure -p PORT flash
```

eFuse はやり直せない。既存の平文 NVS 端末をセキュアビルドへ移す場合は、
設定を控えて全消去・再プロビジョニングする。HMAC 鍵が未設定の端末では、
暗号化必須ビルドは平文へ降格せず起動を停止する。`sdkconfig.secure.defaults` は
開発用の既定ビルドとは別のビルドディレクトリを使う。

## テスト

判定ロジックは `esp_webcam/tests/nvs_boot_test.c` でホスト実行できる。
`esp_webcam/tests/run_host_tests.py` にも含まれる。リポジトリ直下で実行する。

```sh
mkdir -p esp_webcam/build
cc -std=c11 -Wall -Wextra -Werror -I esp_webcam/main \
  esp_webcam/tests/nvs_boot_test.c esp_webcam/main/nvs_boot_values.c \
  -o esp_webcam/build/nvs_boot_test
esp_webcam/build/nvs_boot_test
```

## 検証状況

- 2026-11-03 時点で Docker の ESP-IDF v6.1（`espressif/idf:v6.1`）によるビルド成功。
- ホストの NVS 判定テスト、cloud 認証テスト、network 設定テスト、
  シリアルプロトコルテストが成功。
- 実機への書き込み、暗号化 NVS の生成・再起動後の読み出しは未確認（2026-11-16 時点）。
