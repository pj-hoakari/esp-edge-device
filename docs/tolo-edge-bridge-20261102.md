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

1. 運用者が fakeidp などで ESP 用の `event_access` を発行する
   - `client_id`: `edge-page`（tolo-edge-bridge の `EDGE_CLIENT_ID`）
   - scope: `events.report`
   - `tenant_id` / `event_id`: 観測ページを開くイベント
2. ESP はシリアル `token` で JWT を NVS に保存する
3. ESP は起動時に `IssueFirestoreToken` を呼ぶ
   - `POST {signaling}/tolo.edgebridge.v1.SignalingService/IssueFirestoreToken`
   - `Authorization: Bearer <event_access>`
   - `Content-Type: application/json`
   - `Connect-Protocol-Version: 1`
   - body `{}`
4. 返った `firebaseCustomToken` で Firebase Auth にサインインする
   - エミュレータ: `http://<auth-host>/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=<apiKey>`
   - 本番: `https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=<apiKey>`
5. 以後の Firestore REST は `Authorization: Bearer <idToken>`

`event_access` の `exp` が切れると再サインインできない。`token` で再投入して `reboot` する。
Firebase の ID トークンは 1 時間で切れるため、ESP は `securetoken` で更新する。
カスタムクレームの `expires_at` は event_access の `exp` のままなので、それを超えると
Firestore のルールで拒否される。

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

## ESP の設定

UART0（115200bps、GPIO43=TX / GPIO44=RX）で設定する。

```text
signaling   Edge Bridge の base URL（例: http://192.168.1.20:8787）
cloud       Edge device ID（16 桁小文字 hex）
            Firebase project ID / Web API key
            Firestore emulator host:port（本番は空）
            Auth emulator host:port（本番は空）
token       事前発行した event_access JWT（マスク入力）
show        active/saved の設定とトークンの有無
reboot      反映
```

- トークンは NVS の `cloud` 名前空間に平文で保存する。NVS 暗号化は未対応。
  EAP の証明書を保存している場合、`cloud` の blob と合わせて NVS の空きを確認する。
  足りない場合は NVS パーティションを広げる
- `event_access` は短命・イベント限定の資格情報を使う
- エミュレータを使う場合、Firestore / Auth のホストは ESP から到達できる PC の LAN IP
  （例: `192.168.1.20:8080`）にする。`127.0.0.1` は ESP 自身を指すため使えない。
  API key はエミュレータなら任意の非空文字列（例: `demo-key`）でよい
- 時刻は SNTP（`pool.ntp.org`）で合わせる。オフラインでは `clock` で手動設定する
- `event_access` の `tenant_id` / `event_id` は ESP が JWT から読む
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

1. tolo-web の compose で firebase-emulator / fakeidp / edge-bridge / web を起動
2. fakeidp で ESP 用 `event_access` を発行（`ttl_seconds` を長めにする）
3. tolo-web 用の `GRAPH_ACCESS_TOKEN`（console）と `OBSERVATION_ACCESS_TOKEN`（edge）を
   compose に渡して web を起動
4. ESP のシリアルで `signaling` / `cloud` / `token` を設定して `reboot`
5. 観測ページを `?camera=` 付きで開いて開始
6. 管理UIから観測ページの edgeDeviceId に接続
7. ESP の `/rtc-status` で `channel_open`、`sent_frames`、`acked_frames` を確認
8. `/status` で `valid_jpegs` と `transfer_errors` を確認

## ビルド（ESP-IDF v6.1）

新しいファイルは `esp_webcam/main/` に追加済みで、`CMakeLists.txt` の `SRCS` と
`REQUIRES`（`json`）も更新してある。ESP-IDF 側では次を実行する。

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
操作は `esp_webcam/README.md` を参照する。書き込み後、シリアルで
`signaling` / `cloud` / `token` を設定し、`reboot` する。
