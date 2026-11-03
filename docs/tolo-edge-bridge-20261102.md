# ESP32-S3 を tolo-edge-bridge / tolo-web につなぐ（2026-11-02）

## 構成

```text
[管理UI (console)] --WebRTC/Firestore 既存--> [観測ページ (edge)] --WebRTC/Firestore 新規--> [ESP32-S3 (edge)]
                                                     |  JPEG DataChannel (JPG1)
                                                     +--> canvas -> 人検出 -> 管理UIへ再配信
```

- 管理UI ↔ 観測ページのシグナリングは既存のまま（onSnapshot）
- ESP ↔ 観測ページのシグナリングは Firestore REST のポーリング
- ESP は映像だけを送る。検出は観測ページのブラウザで行う
- `viewer-server/` は使わない

## 認証とトークン

1. 管理UIで ESP カメラを登録し、Web Serial で `edge_device_id` と `idp_url` を書き込む
2. ESP は `{idp_url}/oauth2/device_authorization` を呼ぶ
   - `client_id`: `esp-camera`
   - `scope`: `events.stream`
   - `event_id` と `edge_device_id` は送らない
3. IdP は `device_code` と `user_code` を返す。ESP は `user_code` を次の JSON Lines で出力する

   ```json
   {"type":"device_authorization","user_code":"ABCD-EFGH","verification_uri":"https://auth.example/device","expires_in":600}
   ```

4. スタッフが管理UIで `user_code` を承認する。管理UI/BFF が、選択したカメラの
   `event_id` と `edge_device_id` を IdP の `POST /api/device-authorization/approve` へ送る
5. ESP は `{idp_url}/oauth2/token` を polling し、承認後に `event_access` と Refresh Token を受け取る
   - `grant_type=urn:ietf:params:oauth:grant-type:device_code`
   - `event_access` のクレーム: `scope=events.stream`、`token_use=event_access`、
     `tenant_id`、`event_id`、`edge_device_id`、有効期間10分
6. ESP は `IssueFirestoreToken` を呼ぶ
   - `POST {signaling}/tolo.edgebridge.v1.SignalingService/IssueFirestoreToken`
   - `Authorization: Bearer <event_access>`
   - `Content-Type: application/json`
   - `Connect-Protocol-Version: 1`
   - body `{}`（Edge Bridge は event_access の `edge_device_id` を使う）
7. 返った `firebaseCustomToken` で Firebase Auth にサインインする
   - エミュレータ: `http://<auth-host>/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=<apiKey>`
   - 本番: `https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=<apiKey>`
8. 以後の Firestore REST は `Authorization: Bearer <idToken>`

event_access は応答の `expires_in` の70〜80%で更新する。更新は一度に1つだけ行い、
送信前に `refresh_in_progress` を NVS に記録する。新しい Refresh Token を保存してから
旧トークンを破棄し、`invalid_grant` なら再承認が必要な状態にする。更新中に再起動した場合も
再承認が必要になる。ネットワーク障害はバックオフして再試行する。
Firebase の ID トークンは従来どおり `securetoken` で更新する。
Refresh Token の期限は24時間で、承認から7日を超えない。7日を過ぎたら再承認する。

トークンと Refresh Token は `cloud` 名前空間に保存し、UART・HTTPログ・`show` には出さない。
`show` で確認できるのは種別と有効期限、認可状態だけとする。

## Firestore の契約

パス:

```text
tenants/{tenantId}/events/{eventId}/edgeDevices/{edgeDeviceId}/sessions/{sessionId}/signals/{signalId}
```

- セッションは観測ページ（console）が BFF 経由の `RequestConnection` で作る
- ESP は `sessions` を `?pageSize=10` で読み、`status == "requested"` を探す
- ESP は `signals` を `?pageSize=20` で読み、`from == "console"` の description/offer を探す
- ESP は answer を `from: "edge"` で書く
  - `createdAt` は `batchWrite` の `updateTransforms` で `REQUEST_TIME` を設定する
  - ルールが `createdAt == request.time` を要求するため
- 観測ページは `from: "console"` で offer を書く（既存の `serverTimestamp()`）
- ICE は non-trickle。観測ページは gathering complete まで待って offer を 1 回だけ書く
- ESP は answer に自分の候補を含める。`jpeg` DataChannel は接続後に ESP が作る

ESP が書く answer の形:

```json
{
  "writes": [{
    "update": {
      "name": "projects/<project>/databases/(default)/documents/tenants/.../signals/<id>",
      "fields": {
        "from": { "stringValue": "edge" },
        "payload": { "mapValue": { "fields": {
          "kind": { "stringValue": "description" },
          "description": { "mapValue": { "fields": {
            "type": { "stringValue": "answer" },
            "sdp": { "stringValue": "v=0..." }
          }}}
        }}}
      }
    },
    "currentDocument": { "exists": false },
    "updateTransforms": [
      { "fieldPath": "createdAt", "setToServerValue": "REQUEST_TIME" }
    ]
  }]
}
```

## TryCloudflare で2ドメインを使う（ESP 用）

ホスト PC から ESP へ直接届かない場合、TryCloudflare のトンネルを2本立てられる。
先に通常の compose（firebase-emulator / fakeidp / edge-bridge / web）を起動しておき、
そのうえで ESP 用のオーバーレイを重ねる。tolo-web 側に ESP 用のオーバーレイがある。

```bash
cd tolo-web
task esp-tunnel          # 2本のトンネルを起動して URL を表示
# または
docker compose -f compose.yaml -f compose.esp-tunnel.yaml up -d \
  esp-tunnel-proxy esp-emulator-tunnel esp-edge-tunnel
docker compose -f compose.yaml -f compose.esp-tunnel.yaml logs -f \
  esp-edge-tunnel esp-emulator-tunnel
task esp-tunnel:stop     # 停止
```

出力は2つ:

- `esp-edge-tunnel`: tolo-edge-bridge のシグナリング用
  - 例: `https://abcd-....trycloudflare.com`
- `esp-emulator-tunnel`: Firestore と Auth を1つのドメインにまとめたもの
  - 例: `https://wxyz-....trycloudflare.com`
  - `/v1/*` は Firestore、`/identitytoolkit...` と `/securetoken...` は Auth へ Caddy が振り分ける

ESP の設定は次のようになる。

```text
signaling
  -> https://<esp-edge-tunnel の URL>

cloud
  -> Firestore: https://<esp-emulator-tunnel の URL>
  -> Auth:      https://<esp-emulator-tunnel の URL>
```

`cloud` の host は `host:port` だけでなく `http://host[:port]` / `https://host[:port]` を
受け付ける。TryCloudflare は HTTPS なので `https://...` をそのまま入力する。
URL はトンネルを再起動するたびに変わる。変わったら `signaling` / `cloud` を再設定して
`reboot` する。

注意: ESP 用のエミュレータトンネルはメール認証を付けていない開発用の入口である。
Firestore/Auth のエミュレータを外部に公開したくない場合は、ESP とホストを同一 LAN に置き、
`192.168.1.20:8080` のような LAN IP を使う。edge-bridge のトンネルは `event_access` を
検証するが、公開範囲は必要最小限にする。

## ESP の設定

UART0（115200bps、GPIO43=TX / GPIO44=RX）で設定する。

```text
signaling   Edge Bridge の base URL（例: http://192.168.1.20:8787）
cloud       Edge device ID（16 桁小文字 hex）
            IdP issuer（http(s)://host[:port]、パス・末尾スラッシュなし）
            Firebase project ID / Web API key
            Firestore emulator host:port（本番は空）
            Auth emulator host:port（本番は空）
show        active/saved の設定と認可状態
reboot      反映
```

通常はこの対話操作ではなく、管理UIの Web Serial から JSON Lines を書き込む。
JSON Lines は `config` / `status` / `device_authorization` / `reboot` に対応し、
設定成功時は `{"type":"config_result","ok":true}` の約500ms後に再起動する。
Wi-Fi パスワードとトークンは JSON にもログにも出さない。

- トークンと Refresh Token は NVS の `cloud` 名前空間に平文で保存する。NVS 暗号化は未対応。
  EAP の証明書を保存している場合、`cloud` の blob と合わせて NVS の空きを確認する。
  足りない場合は NVS パーティションを広げる
- `cloud` の v1 blob は v2 へ移行する。未知の版・サイズは再解釈せず、ビルド既定値へ戻す
- エミュレータを使う場合、Firestore / Auth のホストは ESP から到達できる PC の LAN IP
  （例: `192.168.1.20:8080`）か、`https://host` 形式のトンネル URL を指定する。
  `127.0.0.1` は ESP 自身を指すため使えない。
  API key はエミュレータなら任意の非空文字列（例: `demo-key`）でよい
- 時刻は SNTP（`pool.ntp.org`）で合わせる。オフラインでは `clock` で手動設定する
- `tenant_id` / `event_id` / `edge_device_id` は event_access のクレームから ESP が読む
- `signaling` の URL は既存の `network` 設定を再利用する

## 観測ページ

`.../observation/<観測ページのedgeDeviceId>?camera=<ESPのedgeDeviceId>` を開く。

- `camera` があると ESP を既定の映像ソースにする
- 観測ページは ESP セッションの console 用 Firestore トークンを BFF から取得する
- 管理UIから観測ページの edgeDeviceId に接続すると、検出オーバーレイ付きで映像が届く
- ESP は Observation に登録しない。Heartbeat も持たない

## 既知の制約

- 観測ページに console 相当の Firestore 書き込み権限が渡る（同一イベント内の
  セッション削除が可能）。PoC の割り切り。将来は edge-bridge にカメラ専用ロールを
  追加するか、BFF でセッション ID を限定する
- Firestore/Auth への HTTPS ポーリングは TLS ハンドシェイクの負荷が大きい。
  接続中は signals のポーリングを止め、sessions の間隔を空ける
- 同時接続は 1 台。ESP は 1 つの peer しか持たない
- 再接続は観測ページの「停止 → 開始」で新しいセッションを作る
- 実機での確認は別 PC の ESP-IDF 環境で行う

## 確認手順

1. tolo-web の compose で firebase-emulator / IdP / edge-bridge / web を起動
2. 管理UIで ESP カメラを登録し、Web Serial で Wi-Fi・signaling・IdP・Firebase・
   `edge_device_id` を書き込む
3. ESP が出力した `user_code` を使って管理UIで承認する
4. 観測ページを `?camera=` 付きで開いて開始
5. 管理UIから観測ページの edgeDeviceId に接続
6. ESP の `/rtc-status` で `channel_open`、`sent_frames`、`acked_frames` を確認
7. `/status` で `valid_jpegs` と `transfer_errors` を確認
8. `show` で `Authorization: approved` と event_access / Refresh Token の有無を確認

## COM ポートからの書き込み

通常の USB-UART / USB Serial/JTAG ポートから書き込む場合は `tools/flash_serial.py` を使う。
`build/flasher_args.json` があればオフセットと flash 設定をそこから読む。

```bash
cd esp_webcam
# ESP-IDF の Python 環境で
python tools/flash_serial.py --port COM5 --check   # 接続確認
python tools/flash_serial.py --port COM5           # 書き込み
python tools/flash_serial.py --port COM5 --erase   # 全消去してから書き込み
python tools/flash_serial.py --port COM5 --build-dir build-fast
```

`--after no_reset` などが必要なボードでは指定する。Pico debugprobe の UART 経由は
従来どおり `tools/flash_debugprobe.py` を使う（no-reset と CDC のタイムアウト制御が
必要なため）。

### macOS から Pico debugprobe 経由で書き込む例

`/dev/cu.usbmodem*` を `--port` に指定する。ESP-IDF 5.5 同梱の esptool 4.12 では
接続できない場合があるため、esptool 5.6（`uvx`）で `--before no-reset` を使う。

```bash
cd esp_webcam/build
uvx --from esptool esptool --chip esp32s3 --port /dev/cu.usbmodem3102 \
  --baud 115200 --before no-reset --after no-reset write-flash @flash_args
uvx --from esptool esptool --chip esp32s3 --port /dev/cu.usbmodem3102 \
  --baud 115200 --before no-reset --after no-reset verify-flash @flash_args
```

書き込み後は bootloader に留まる。IO0 の GND 接続を外し、EN をリセットするか
再給電して通常起動する。

## ビルド（ESP-IDF v6.1）

新しいファイル（`device_auth.c` / `device_auth_values.c` / `cloud_settings_values.c`）は
`esp_webcam/main/` に追加済みで、`CMakeLists.txt` の `SRCS` も更新してある。
ESP-IDF 側では次を実行する。

```bash
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py reconfigure   # 新しい Kconfig（WEBCAM_EDGE_DEVICE_ID など）を反映
idf.py build
```

`idf.py reconfigure` を省くと、既存の `sdkconfig` に新しい `CONFIG_WEBCAM_*` が
入らずコンパイルに失敗する。設定を Kconfig で与える場合は `idf.py menuconfig` の
`ESP webcam` で Edge Bridge URL・edgeDeviceId・Firebase project/API key・
emulator ホストを入力する。シリアルで設定する場合は空のままでよい。

ビルド後は通常どおり UART 経由で書き込む。Pico debugprobe の配線と IO0/EN の
操作は `esp_webcam/README.md` を参照する。書き込み後、Web Serial またはシリアルで
Wi-Fi・signaling・IdP・Firebase・`edge_device_id` を設定し、`reboot` する。
