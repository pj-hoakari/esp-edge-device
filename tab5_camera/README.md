# tab5_camera

M5Stack Tab5 の内蔵カメラの映像を、P4 のハードウェアエンコーダで H.264 にして、WebRTC の video track（P2P）で推論デバイスの観測ページへ送るファームウェア。
観測ページは連続観測でこの映像を推論する。

認可（RFC 8628）、Edge Bridge 経由の Firestore シグナリング、シリアルでのネットワーク・クラウド設定は `../esp_webcam/main/` のソースをそのまま使う。
Tab5 固有のコードは `main/` の 4 ファイルと、`components/m5stack_tab5/` に置いた BSP である。

| ファイル | 役割 |
|---|---|
| `main/tab5_main.c` | 起動順序（NVS、シリアル設定、IO エキスパンダ、ESP32-C6、カメラ、Wi-Fi、WebRTC） |
| `main/camera_h264.c` | BSP でカメラと Wi-Fi の電源を入れ、MIPI-CSI の SC202CS から V4L2 で取り込み、H.264 デバイスでエンコードする |
| `main/status_display.c` | 画面に Wi-Fi の接続状態（SSID と IP アドレスを含む）と、観測ページに映像を送っているか（Observing / Not observing）を出す。カメラ映像は出さない |
| `main/webrtc_h264.c` | `webrtc.h` の実装。観測ページの offer に `sendonly` の H.264 で応答し、エンコード済みフレームを送る |

## 対象と前提

- ボード: M5Stack Tab5（ESP32-P4 rev v1.x、Flash 16MB、PSRAM 32MB、Wi-Fi は SDIO 接続の ESP32-C6）
- ESP-IDF v6.1
- 映像: 1280×720、既定 15fps、2Mbps、GOP 15 フレーム（`menuconfig` の Tab5 camera で変更する）
- 観測ページは offer に `recvonly` の video transceiver を含める（tolo-web の連続観測）。
  バースト観測の offer には video m-line がないので、このファームウェアとは映像を交換できない。

## ビルドと書き込み

```sh
cd tab5_camera
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

初めて書き込む Tab5 では、工場出荷時のデータが `nvs` と `nvs_keys` に残っていて NVS の暗号化鍵を読めない。
一度だけ両方を消去する。

```sh
esptool --port /dev/cu.usbmodem101 erase-region 0x9000 0x6000
esptool --port /dev/cu.usbmodem101 erase-region 0x10000 0x1000
```

設定は USB-C の USB Serial/JTAG で行う。
コマンドは `esp_webcam` と同じで、`help` で一覧を出す。
Web Serial による管理 UI からのプロビジョニングも同じポートで行う。

## ESP32-C6 のファームウェア

C6 には、P4 側と同じ esp_hosted 3.1.0 のコプロセッサファームウェアが要る。
工場出荷時の C6（esp_hosted 1.4 系）のままでは、起動時に `major version mismatch` が出る。接続から 20〜40 秒で SDIO の受信が止まり（`SDIO RX read (3072 bytes) exceeds staging target (1536 bytes)`）、Wi-Fi が戻らない。
P4 側の SDIO 受信モードを変えても回避できなかった。

C6 のファームウェアは `c6_coprocessor/` でビルドする。
esp_hosted の `examples/wifi/enterprise/cp` をもとに esp_hosted を `==3.1.0` に固定し、802.1X/EAP を S3 版と同じ設定で有効にしてある（`ESP_WIFI_ENTERPRISE_SUPPORT`、`ESP_WIFI_MBEDTLS_TLS_CLIENT`、`ESP_WIFI_SUITE_B_192`、証明書バンドル）。

```sh
cd c6_coprocessor
idf.py set-target esp32c6
idf.py build
```

C6 への書き込みは、P4 から SDIO 経由の OTA（begin / write / end / activate）で行う。
Wi-Fi が使えない状態でも書き込めるよう、イメージは P4 のフラッシュの空き領域（`0x620000` 以降）に置いて読み出す。
工場出荷時の 1.4 系の C6 は `activate` に対応しないが、`end` の後の再起動で新しいファームウェアに切り替わる。
起動ログに `host=3.1.0 coprocessor=3.1.0 (match)` と `eh_wifi_ent: wifi enterprise ext init` が出れば更新済みである。

## 802.1X/EAP

EAP の設定と接続は C6 が行い、P4 は `wifi_auth.c`（S3 版と共有）の設定を RPC で C6 に渡す。
P4 側は `CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_EXT_ENT` でこの RPC を有効にし、C6 側は `c6_coprocessor/` のファームウェアが要る。
シリアルの `eap`、`credential`、`protocols` は S3 版と同じで、`protocols` は TLS、PEAP、TTLS の各方式と、WPA/WPA2/WPA3/Suite-B を表示する。
EAP-FAST は S3 版と同じく通常版には入らない。

esp_hosted 3.1.0 は、EAP の RPC を呼ぶ関数（`esp_eap_client_remote_*`）を静的ライブラリ `esp_wifi_remote_glue` に入れているが、リンカに残す指定をしていない。
そのままでは esp_wifi_remote の弱い定義（`ESP_ERR_NOT_SUPPORTED` を返す）が使われる。
`CMakeLists.txt` でこのライブラリを `--whole-archive` でリンクしている。

C6 が EAP の各設定（identity、username、TTLS の phase 2、証明書バンドル、Suite-B）を受け付けることは確かめた。
802.1X のアクセスポイントへの接続は確かめていない。

## BSP

`components/m5stack_tab5/` は Espressif の Tab5 BSP 1.3.2（Apache-2.0）をそのまま置いたもので、`idf_component.yml` の `esp_video` だけを `~2.3.0` に変えている。
配布版の BSP は `esp_video ~2.0` に固定されていて、このプロジェクトの依存と両立しないためである。
LCD はボードの版ごとに異なる（ILI9881C、ST7123、ST7121）。BSP がタッチコントローラーの応答から版を判定して初期化する。
LVGL の描画バッファは PSRAM に置く。BSP 既定の内部 DMA メモリに置くと、H.264 エンコーダが参照フレーム用のメモリを確保できず、カメラが起動しない。

## rev v1.x 向けの調整

Tab5 の P4 は rev v1.3 で、rev v3 の命令（Zba/Zbb）を実行できない。

- `sdkconfig.defaults` で `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` を選ぶ
- `esp_video` は `~2.3.0` に固定する。2.4 以降が使う `esp_ipa` 2.3 以降の IDF 6.x 向けビルド済みライブラリは Zba/Zbb を含み、起動時に Illegal instruction で止まる
- `esp_capture` は使わない。依存する音声コンポーネントの新しい版が rev v3 を要求し、ビルドが止まる
- `sc202cs_ipa_rev1.json` はセンサー既定の IPA 設定から色補正行列（CCM）を外したもの。IPA が算出する CCM が rev v1.x の ISP の範囲（±4）を超え、毎フレーム設定に失敗するため

ビルド後の ELF に Zba/Zbb の命令が残っていないことは次で確かめる。

```sh
riscv32-esp-elf-objdump -d build/tab5_camera.elf \
  | grep -cE '	sh[123]add|	andn	|	orn	|	clz	|	ctz	|	cpop	|	minu?	|	maxu?	|	bclr|	bset|	sext\.[bh]	|	zext\.h	'
```

## 制約

- esp_video の H.264 デバイスはキーフレームの強制に対応していない。
  接続直後と PLI の後は、次の I フレーム（既定で最大 1 秒）まで観測ページに映像が出ない。
- CCM を外しているので、色は既定の IPA 設定より不正確になる。
- 画面の文字は英語だけである。LVGL 組み込みの Montserrat フォントに日本語の字形がないため、日本語を含む SSID は正しく表示されない。
- 音声とタッチは使わない。
