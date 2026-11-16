# ESP32-S3 UVC カメラファームウェア

USB UVC カメラの MJPEG を ESP32-S3 で受け取り、WebRTC の `jpeg` DataChannel で
観測ページへ送る。HTTP MJPEG は診断用で、ESP はビューアーを配信しない。
基板と回路は `../board/`、全体像は `../README.md` を参照する。

## 現在の方式

- エントリーポイントは `main/mjpeg_main.c`。WebRTC の送信処理は `main/main.c`。
- シグナリングは tolo-edge-bridge の Firestore。`viewer-server/` の HTTP 中継は使わない。
- 認可は Device Authorization Grant。`event_access` を取得し、
  IssueFirestoreToken → Firebase Auth → Firestore REST をポーリングする。
- Wi-Fi・Edge Bridge・IdP・Firebase・edgeDeviceId は UART0 または Web Serial から
  NVS に保存する。NVS は既定で XTS-AES 暗号化する。
- 映像は 1280×720 MJPEG の JPEG を再圧縮せず送る。目標 10fps、最低 6fps。
- HTTP の診断（80 番の `/status` など）と MJPEG（81 番の `/stream`）は診断用。
  WebRTC の DataChannel が開いている間は MJPEG 送信を止める。

## 固定した開発環境

| 依存 | 版 / コミット |
| --- | --- |
| ESP-IDF | v6.1.0 / `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Xtensa GCC | `esp-15.2.0_20251204` |
| esp_peer | 1.5.7 / esp-webrtc-solution `38b175077967109e764d434d680e641d9e87b23c` |
| usb_host_uvc | 2.6.0 / esp-usb `2e2032bb861f40b3d9e1e513e03856c8daa337a1` |
| usb | 1.6.0 / 同じ esp-usb チェックアウト |
| esp_libsrtp | 1.0.0（`dependencies.lock` で固定） |

`vendor/` の esp-usb と esp-webrtc-solution は submodule。コミットを固定し、
直接編集しない。`.tools/esp-idf` はローカル取得用で Git 対象外。

## ビルド

`esp_webcam/` で ESP-IDF v6.1 の環境から実行する。Docker では
`espressif/idf:v6.1` を使える。

```sh
git submodule update --init --recursive
idf.py set-target esp32s3        # 初回のみ
idf.py reconfigure               # Kconfig の項目を追加・変更したとき
idf.py build
```

- `sdkconfig`、`sdkconfig.fast`、`build/`、`build-fast/`、`managed_components/`、
  `certs/`、`.tools/` は Git 対象外。コミットしない。
- `sdkconfig` にはローカルの Wi-Fi 認証情報が入り得る。共有しない。
- セキュアビルドは `sdkconfig.secure.defaults` を使う。eFuse の HMAC 鍵は
  やり直せない。詳細は [`../docs/nvs-encryption.md`](../docs/nvs-encryption.md)。

## 書き込み

`PORT` は環境に合わせたシリアルポート名に置き換える。先に `idf.py build` を通す。

```sh
python tools/flash_serial.py --port PORT --check
python tools/flash_serial.py --port PORT
python tools/flash_serial.py --port PORT --erase      # NVS も消える
python tools/flash_debugprobe.py --port PORT
```

- 通常の書き込みでは NVS の設定・証明書・トークンは保持される。
  `--erase` や `erase-flash` はそれらを消す。
- Pico debugprobe は GP4→GPIO44（RX）、GP5→GPIO43（TX）、GND 共通。
  書き込み時は IO0=GND、終了後は IO0 を外して EN リセット。
- カメラ接続中は内蔵 USB PHY が USB ホストになり、USB Serial/JTAG が
  見えなくなる。書き込み時はカメラを外す。
- NVS 暗号化へ移行するときは全消去して再プロビジョニングする。
  `nvs_keys` だけを消してはいけない。

## 初期設定

設定は NVS に保存し、menuconfig の値より優先する。書き込み後に UART0
（115200bps、GPIO43=TX / GPIO44=RX）または Web Serial から設定する。

### Web Serial（JSON Lines）

管理 UI などからは次の形式でまとめて書き込む。1 行 1 JSON、`\n` 終端。

```json
{"type":"config","version":1,"wifi_ssid":"...","wifi_password":"...","signaling_url":"http://host:8787","idp_url":"https://auth.example","firebase_project_id":"...","firebase_api_key":"...","firestore_emulator_host":"","firebase_auth_emulator_host":"","edge_device_id":"0123456789abcdef"}
```

成功時は `{"type":"config_result","ok":true}` を返し、約 500ms 後に再起動する。
`status`、`device_authorization`、`reboot` も JSON で応答する。
Wi-Fi パスワードとトークンは JSON にもログにも出さない。

### シリアルコマンド

| コマンド | 内容 |
| --- | --- |
| `wifi` | SSID・パスワードを保存。空パスワードはオープンネットワーク |
| `eap` | 802.1X/EAP の方式・Identity・認証情報・サーバー検証を設定 |
| `protocols` | ビルドで使用できる EAP 方式と Enterprise 対応を表示 |
| `credentials` | CA・クライアント証明書・秘密鍵・PAC の保存有無を表示 |
| `credential-clear <種類>` | 保存した資材を削除 |
| `clock` | 起動中の UTC Unix 秒を設定。証明書の期限検証に使う |
| `signaling` | Edge Bridge の base URL を設定。`off` で WebRTC を無効化 |
| `cloud` | edgeDeviceId・IdP issuer・Firebase project/API key・emulator を設定 |
| `token` | 復旧用。event_access をマスク入力で保存（通常は Device Authorization） |
| `show` | 現在の IP、設定、認可状態を表示。秘密値は有無だけ |
| `ping [IPv4]` | IPv4 へ 3 回の ICMP 疎通確認。省略時はゲートウェイ |
| `check signaling` | Edge Bridge の `/healthz` へ読み取り専用の HTTP(S) 確認 |
| `check stun` | 設定済み STUN へ UDP Binding Request を 1 回送信 |
| `reboot` | 再起動して保存設定を反映 |
| `logs on` / `logs off` | UART の診断ログ表示を開始／停止 |

- `edgeDeviceId` は 16 桁小文字 hex。認証情報ではない。
- 観測ページは
  `.../observation/<観測ページのedgeDeviceId>?camera=<ESPのedgeDeviceId>` を開く。
- STUN は `WEBCAM_STUN_URL` で設定する。TURN の中継・認証は未実装。
- 時刻は SNTP（`pool.ntp.org`）で合わせる。オフラインでは `clock` を使う。
  TLS と Firestore の都合上、時計が大きくずれると接続できない。
- 資格情報を送る経路は HTTPS を必須にする。`CONFIG_WEBCAM_ALLOW_INSECURE_HTTP`
  はローカル開発時だけ明示的に有効にする。開発環境をインターネットへ公開する
  トンネルは使わない。

Device Authorization の流れ:

1. 管理 UI で ESP カメラを登録し、Web Serial で `edge_device_id` と `idp_url` を書く。
2. ESP が IdP の `/oauth2/device_authorization` を呼び、`user_code` を JSON Lines で出す。
3. スタッフが管理 UI で `user_code` を承認する。
4. ESP が `/oauth2/token` をポーリングし、`event_access` と Refresh Token を保存する。
5. ESP が IssueFirestoreToken → Firebase Auth → Firestore REST の順で接続する。

契約とエラー時の扱いは
[`../docs/tolo-edge-bridge-20261102.md`](../docs/tolo-edge-bridge-20261102.md) を参照する。

## 動作確認

`<ESP_IP>` は ESP が取得した IPv4 に置き換える。

| URL | 内容 |
| --- | --- |
| `http://<ESP_IP>/` | 診断画面 |
| `http://<ESP_IP>/status` | USB・UVC・JPEG のカウンタと目標 fps |
| `http://<ESP_IP>/usb-log` | USB/UVC の直近ログ |
| `http://<ESP_IP>/log` | WebRTC/DTLS/SCTP/ICE の直近ログ |
| `http://<ESP_IP>/rtc-status` | DataChannel、送信・描画 ACK、処理時間 |
| `http://<ESP_IP>/perf` | 暗号方式、CPU、RAM、処理別の時間 |
| `http://<ESP_IP>:81/stream` | MJPEG 映像（診断用） |

- `/status` は `usb_devices`、`uvc_detected`、`frames_received`、`valid_jpegs`、
  `invalid_*`、`transfer_errors`、`buffer_overflows`、`stream_restarts` を返す。
- `/rtc-status` は `channel_open`、`sent_frames`、`acked_frames`、`send_fps`、
  `display_fps`、`mean_send_ms`、`would_block` を返す。
  `effective_fps` は設定値であり、実測値ではない。
  `client_motion` は旧クライアントとの互換用で現在は `-1`。
- `POST /rtc-config?cipher=2&chunks=4&burst_us=4000&fast_rx=1` で実験設定を変更できる。
  `&persist=1` で NVS に保存する。暗号方式の変更は次の接続から反映する。
- WebRTC 接続中は MJPEG が休止する。MJPEG だけ見たい場合は `signaling off` にする。

## テスト

リポジトリ直下で実行する。C11 の `cc` と Python 3 が必要。
`managed_components/` を使うため、先に一度 `idf.py build` を通しておく。

```sh
python3 esp_webcam/tests/run_host_tests.py
```

主な個別テスト:

| テスト | 対象 |
| --- | --- |
| `test_network_console.py` | 設定コンソール、EAP 入力、ログ切替、NVS 再読込み |
| `test_network_diagnostics.py` | IP 表示、ping、切断時の扱い |
| `test_network_probes.py` | `check signaling` / `check stun` の読み取り専用動作 |
| `test_serial_protocol.py` | JSON Lines の `config` / `status` / `device_authorization` |
| `test_wifi_auth.py` | 通常版・FAST 版の EAP 割当てと秘密の扱い |
| `test_credential_upload.py` | 証明書・鍵・PAC の分割転送 |
| `test_device_auth_worker.py` | Device Authorization のワーカー |

`viewer-server/` は旧方式のため、変更する必要がある場合だけ
`cd viewer-server && npm test` を実行する。

## 設計上の制約

### カメラ・USB

- Full Speed（12Mbit/s）で 1280×720 MJPEG を広告・送信できる UVC カメラが必要。
  720p の 10/15fps は descriptor の広告値で、実測値ではない。
- 解像度は HD 固定。低解像度へ落とす設定は持たない。UVC ドライバは広告された
  frame interval との完全一致を要求するため、`WEBCAM_CAMERA_FPS` は 10 か 15 にする。
- 受信は広告 fps の帯域を使う。送信を 6fps に落としても USB 入力帯域は減らない。
- JPEG は 1 枚 256KiB まで。超過フレームは破棄する。
- 内蔵 USB PHY は GPIO19=D−、GPIO20=D+。書き込み・ログは UART0 を使う。
- カメラの VBUS には 5V 給電が必要。電流制限・逆流保護・突入電流を確認する。
  PC と基板を A-A ケーブルで接続して書き込まない。
- 同時接続は 1 台。ESP は 1 つの peer しか持たない。

### 分割送信

- 1 メッセージは最大 12,020 bytes（`JPG1` ヘッダー 20 + JPEG 断片 12,000）。
- ヘッダーは `JPG1`、uint32 LE の frame ID、JPEG 全長、断片 offset、送信開始 ms。
- DataChannel は unordered、maxRetransmits=0 が既定（Kconfig で 0〜3）。
- ブラウザは最大 3 フレーム分の断片を並行して組み立て、デコード待ちは最新 1 枚だけ保持する。
- ACK は実際に canvas へ描画したフレームだけ返す。250ms を超えた未送信・送信途中の
  JPEG は破棄して最新フレームへ移る。
- ESP は ACK を統計に使うが、ACK 待ちで送信を止めない。

### 性能

- 目標は 1280×720 カラーで最低 6fps。既定の送信目標は 10fps。
- 実測は平均 6.7〜6.8fps 程度。ローリング 5 秒の最低は 5.4fps、最大描画間隔は
  617ms で、常時 6fps は未達。
- 判定には `/rtc-status` の `display_fps` を使う。`effective_fps` は設定値。
- 全被写体・電波条件・長時間での 6fps 維持は保証しない。
- 詳細は [`../docs/webrtc-performance-20261101.md`](../docs/webrtc-performance-20261101.md)
  と [`../docs/webrtc-profile-20261101.md`](../docs/webrtc-profile-20261101.md)。

### 認可・HTTPS

- `CONFIG_WEBCAM_ALLOW_INSECURE_HTTP` の既定は `n`。IdP・Edge Bridge・
  Firestore/Auth は HTTPS を必須にする。HTTP へ自動リダイレクトしない。
- `invalid_grant`、grant 期限切れ、再承認が必要な状態では送信を止め、peer を切断し、
  送信途中の JPEG・offer・answer を破棄する。再承認後は新しい接続を待つ。
- `device_authorization` のキューが満杯、または cloud ワーカー未起動の場合は
  `authorization_queue_full_or_unavailable` を返す。
- トークンと Refresh Token は NVS にだけ保存する。`show` とログには本文を出さず、
  有効期限と有無だけを出す。

### NVS 暗号化

- 既定ビルドは `nvs_keys` パーティションの鍵で NVS を XTS-AES 暗号化する。
- 鍵が無い・読めない・初期化に失敗した場合は、警告を出して平文へフォールバックする。
- 本番は `sdkconfig.secure.defaults` の `CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION` で
  平文起動を禁止する。鍵は eFuse の HMAC 鍵から導出する。
- フラッシュ暗号化を有効にしない既定ビルドでは、鍵自体はフラッシュ上に平文で残る。
- 既存の平文 NVS 端末を暗号化へ移すときは、設定を控えて全消去・再プロビジョニングする。
  `nvs_keys` だけを消すと復号できず、設定・トークンを失う。
- アプリの NVS 初期化は必ず `nvs_boot_init()` を通す。`nvs_flash_init()` を直接呼ばない。
- 詳細とパーティション表は [`../docs/nvs-encryption.md`](../docs/nvs-encryption.md)。

## トラブルシューティング

| 症状 | 確認すること |
| --- | --- |
| USB 機器数が 0 | カメラの給電、J1 の VBUS/GND、D+/D−、`/usb-log`。制御転送上限と FIFO 設定は反映済み |
| UVC 検出後も JPEG が来ない | `/status` の `invalid_*`、`stream_restarts`、カメラの形式・帯域 |
| DataChannel が開かない | `/log` と `/rtc-status` で ICE/DTLS/SCTP の段階を確認。観測ページで停止→開始 |
| 映像が 6fps を下回る | `display_fps`、JPEG サイズ、RSSI、`/perf` の処理時間。目標値では判定しない |
| 認可が必要と表示される | `show` で状態を確認し、Web Serial の `device_authorization` から再承認する。時計も確認 |
| 設定が消えた | NVS 暗号化の変更時に全消去したか確認。再プロビジョニングする |
| MJPEG しか出ない | `signaling` が `off`、または WebRTC 未接続。MJPEG は診断用 |
| PC からカメラが見えない | USB ホスト起動後は USB Serial/JTAG が見えない。書き込み時はカメラを外す |

## 根拠資料

- [esp_peer 1.5.7 の API・設定](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/components/esp_peer)
- [UVC 2.6.0 の API](https://github.com/espressif/esp-usb/blob/2e2032bb861f40b3d9e1e513e03856c8daa337a1/host/class/uvc/usb_host_uvc/include/usb/uvc_host.h)
- [公式 JPEG DataChannel 例](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/solutions/local_jpeg_stream)
- [ESP32-S3-WROOM-1/1U データシート](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [USB Host / PHY 共有の制約](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#external-phy-configuration)

## 関連ドキュメント

- [`../docs/README.md`](../docs/README.md) — ドキュメント一覧。
- [`../docs/tolo-edge-bridge-20261102.md`](../docs/tolo-edge-bridge-20261102.md) —
  Firestore シグナリング、認可、Firestore 契約。
- [`../docs/nvs-encryption.md`](../docs/nvs-encryption.md) — NVS 暗号化、移行、セキュアビルド。
- [`../docs/wifi-enterprise-20261102.md`](../docs/wifi-enterprise-20261102.md) — 802.1X/EAP。
- [`../docs/camera-349c-3307.md`](../docs/camera-349c-3307.md) — カメラの descriptor 調査。
- [`../docs/pcb-review-20261026.md`](../docs/pcb-review-20261026.md) — 基板の未解決事項。
