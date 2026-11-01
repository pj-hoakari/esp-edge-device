# WebRTCビューアー / シグナリングサーバー

ESP32-S3ファームウェアとは別に動かすNode.jsサーバー。ブラウザへビューアー画面を配信し、
ESP32とブラウザのSDPを中継する。ESP32のHTTPサーバーはビューアーを配信しない。

## 起動

```powershell
cd viewer-server
npm start
```

ブラウザで `http://<PCのIP>:8080/` を開く。ESP32から到達できるアドレスを使う。
既定ポートは8080、待受は`0.0.0.0`。LAN内専用で、認証はない。

| 環境変数 | 既定値 | 用途 |
| --- | --- | --- |
| `PORT` | `8080` | HTTP待受ポート |
| `HOST` | `0.0.0.0` | 待受アドレス |
| `STUN_URL` | `stun:stun.l.google.com:19302` | ビューアーとESPで使うSTUN。カンマ区切りで複数指定可 |
| `OFFER_TTL_MS` | `120000` | offerを保持する時間 |

ESP32側の`Signaling server URL`にはこのサーバーのベースURLを設定する。
例: `http://192.168.137.1:8080`。ESP32側の`STUN server URL`も同じSTUNに合わせる。

## シグナリングAPI

| メソッド / パス | 送信元 | 内容 |
| --- | --- | --- |
| `POST /api/offer` | ビューアー | SDP offer本文。応答はoffer ID |
| `GET /api/answer?id=N` | ビューアー | ESPのanswerを取得。未生成は202、置換済みは409 |
| `GET /api/esp/offer?seen=N` | ESP32 | 未取得のofferを`<ID>\n<SDP>`で取得。新規なしは204 |
| `POST /api/esp/answer?id=N` | ESP32 | SDP answer本文を設定 |
| `GET /ice` | ビューアー | STUN設定JSON |
| `GET /api/status` | 確認用 | offer/answerの状態 |

ICEはSTUNでサーバー再帰候補を収集する。TURNの中継と認証は未実装。
STUNが使えないネットワークではホスト候補のみで接続する。

ブラウザの`.local`ホスト候補は、ESPから到達できるPC側IPv4へ置換してから渡す。
ビューアーをlocalhostで開いた場合は、ESPのポーリング元IPと同一サブネットの
インターフェースを選ぶ。`a=candidate:`行はそのまま保持する。
直近のofferは`last-offer.sdp`（Git除外）に受信前後ともに保存され、候補の確認に使える。

## テスト

```powershell
npm test
```

`public/receiver.js`のJPEG分割復元・動きスコア、シグナリングAPIの中継、
`.local`候補の展開を確認する。

## 2026-11-01の高速化

HDのまま常時6fps以上を目標とし、ESPの既定送信目標は10fps。
受信は最大3フレーム分のunordered断片を組み立て、デコード待ちには最新1枚だけ保持する。
ブラウザの動き検出と画像読み戻しは行わず、canvas描画後だけACKする。
画面は直近約1秒のfpsと接続後平均fpsを表示する。
実機不在のため達成fpsは未測定。[実測手順と変更内容](../docs/webrtc-performance-20261101.md)を参照。
