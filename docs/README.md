# ドキュメント一覧

このディレクトリは、設計判断・調査・測定・実機検証の記録を置く。
現行のビルド・設定・運用は [`../esp_webcam/README.md`](../esp_webcam/README.md)、
プロジェクト全体は [`../README.md`](../README.md) を参照する。

## 現行の設計・仕様

| ファイル | 内容 |
| --- | --- |
| [`nvs-encryption.md`](nvs-encryption.md) | NVS 暗号化、パーティション、移行、セキュアビルド |
| [`tolo-edge-bridge-20261102.md`](tolo-edge-bridge-20261102.md) | Firestore シグナリング、Device Authorization、認可失敗時の扱い |
| [`power-input.md`](power-input.md) | USB-C 5V 電源入力、CC 抵抗、J2 |
| [`regulator-review.md`](regulator-review.md) | 3.3V レギュレータ U2 の選定とピン配列 |
| [`wifi-enterprise-20261102.md`](wifi-enterprise-20261102.md) | 802.1X/EAP、証明書・鍵・PAC の登録 |
| [`pcb-review-20261026.md`](pcb-review-20261026.md) | 基板の成立条件、製造前に直す箇所、未解決事項 |

## 調査・測定の記録

日付・条件・結果を残す記録。現在の仕様判断は上記の現行ドキュメントと
`esp_webcam/README.md` を優先する。

| ファイル | 内容 |
| --- | --- |
| [`camera-349c-3307.md`](camera-349c-3307.md) | 対象カメラの descriptor 調査（2026-10-31） |
| [`jlc-bom.md`](jlc-bom.md) | JLCPCB 部品表と SMD 化の履歴（2026-10-26） |
| [`webrtc-performance-20261101.md`](webrtc-performance-20261101.md) | WebRTC 送信高速化の設計と 30 秒測定（2026-11-01） |
| [`webrtc-profile-20261101.md`](webrtc-profile-20261101.md) | 暗号・バースト・UDP 受信待ちの実機比較（2026-11-01） |
| [`webrtc-profile-20261101.json`](webrtc-profile-20261101.json) | 上記の集計データ |

## 更新の方針

- 2026-11-16 に `esp_webcam/README.md` の日付付き作業ログを整理した。
  旧記録は git 履歴を参照する。
- 現行の仕様・手順を変えたら、`esp_webcam/README.md` と該当ドキュメントを同時に更新する。
- 測定記録は日付、ファームウェアの版、測定条件、未確認事項を残す。
- ビルド成果物、BOM、Gerber、証明書、認証情報はコミットしない。
- 過去の経緯は git 履歴を正とし、現行ドキュメントに長い作業ログを戻さない。
