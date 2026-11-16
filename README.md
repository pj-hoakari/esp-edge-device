# esp_webcam

USB UVC カメラの MJPEG を ESP32-S3 で受け取り、WebRTC の `jpeg` DataChannel で
観測ページへ送る機器。KiCad による基板設計と ESP-IDF ファームウェアを同じ
リポジトリで管理する。

## 構成

| 項目 | 内容 |
| --- | --- |
| カメラ | USB UVC MJPEG、1280×720、VID:PID `349C:3307` |
| MCU | ESP32-S3-WROOM-1-N8R8（Flash 8MB / PSRAM 8MB） |
| 映像 | JPEG を再圧縮せず `jpeg` DataChannel で送信。映像トラックは使わない |
| シグナリング | tolo-edge-bridge の Firestore。Device Authorization Grant で認可 |
| 設定 | UART0 / Web Serial から NVS へ保存。NVS は既定で暗号化 |
| 診断 | HTTP の診断（80 番）と MJPEG（81 番）、`/rtc-status`、`/log`、`/perf` |

ESP はビューアーを配信しない。観測ページは tolo-web 側を使う。
`viewer-server/` は旧 HTTP シグナリング方式の記録で、現在は使用しない。

## ディレクトリ

| パス | 内容 |
| --- | --- |
| `board/` | KiCad 10 の回路図・基板・専用シンボル |
| `esp_webcam/` | ESP-IDF v6.1 ファームウェア |
| `docs/` | 設計・調査・測定の記録 |
| `viewer-server/` | 旧シグナリングサーバー（未使用） |

## クイックスタート

`PORT` は環境に合わせたシリアルポート名に置き換える。詳細は
[`esp_webcam/README.md`](esp_webcam/README.md) を参照する。

```sh
cd esp_webcam
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py build
python tools/flash_serial.py --port PORT --check
python tools/flash_serial.py --port PORT
```

書き込み後に UART0 または Web Serial で Wi-Fi・シグナリング・クラウド設定を保存し、
`reboot` する。観測ページは
`.../observation/<観測ページのedgeDeviceId>?camera=<ESPのedgeDeviceId>` を開く。

## ドキュメント

- [`docs/README.md`](docs/README.md) — ドキュメント一覧。
- [`esp_webcam/README.md`](esp_webcam/README.md) — ビルド、書き込み、設定、運用。
- [`docs/tolo-edge-bridge-20261102.md`](docs/tolo-edge-bridge-20261102.md) —
  Firestore シグナリングと認可。
- [`docs/pcb-review-20261026.md`](docs/pcb-review-20261026.md) — 基板の未解決事項。
- [`docs/nvs-encryption.md`](docs/nvs-encryption.md) — NVS 暗号化とセキュアビルド。
- [`AGENTS.md`](AGENTS.md) — 開発時のルールとコマンド。

## 現在の状態

- ビルドとホストテストは成功している。
- 実機で Wi-Fi 接続、UVC 受信、DataChannel 経由のカラー HD 映像を確認済み。
- 常時 6fps は未達。5 秒区間で下回る場合がある。詳細は
  [`docs/webrtc-profile-20261101.md`](docs/webrtc-profile-20261101.md)。
- 基板は ERC 0、未配線 0、回路図との不一致 0、DRC 14 件
  （穴径 12・シルク 2）。設計上の未解決事項は
  [`docs/pcb-review-20261026.md`](docs/pcb-review-20261026.md)。
- NVS 暗号化の実機再起動確認、802.1X の全方式、長時間性能は未確認。

## ライセンス

Apache License 2.0。詳細は [`LICENSE`](LICENSE) を参照する。
