# esp_webcam 開発ガイド

USB UVC カメラの映像を ESP32-S3 で受け取り、WebRTC の DataChannel でブラウザへ送る機器。
KiCad の基板設計と ESP-IDF ファームウェアを同じリポジトリで管理する。
説明・作業報告は日本語で行う。

## 重要

- Wi-Fi パスワード、秘密鍵、event_access、Refresh Token をソース・設計ファイル・
  ドキュメント・ログ・コミットへ書かない。保存先は暗号化 NVS。
- `esp_webcam/vendor/`、`board/esp_webcam-backups/`、`*.kicad_prl`、`~*.lck` は編集しない。
- この文書より、リポジトリの実際の構成と `esp_webcam/README.md` を優先する。
  構成を変えたらこの文書も更新する。

## 構成

- `README.md` — プロジェクト全体像とクイックスタート。
- `board/` — KiCad 10 の設計。`esp_webcam.kicad_sch` が回路接続の起点、
  `esp_webcam.kicad_pcb` が基板。
- `esp_webcam/` — ESP-IDF v6.1 ファームウェア。ビルド・書き込み・設定の手順は
  `esp_webcam/README.md`。
- `docs/` — 設計判断・調査・実機検証の記録。一覧は `docs/README.md`。
- `viewer-server/` — 旧 HTTP シグナリング方式。現在は未使用。変更対象にしない。
- `esp_webcam/vendor/` — esp-usb と esp-webrtc-solution の submodule。直接編集しない。

現行の前提:

- モジュールは ESP32-S3-WROOM-1-N8R8（Flash 8MB / PSRAM 8MB）。
  カメラは VID:PID `349C:3307`。
- 映像は 1280×720 MJPEG の JPEG を再圧縮せず、`jpeg` DataChannel で送る。
  目標 10fps、最低 6fps。
- エントリーポイントは `esp_webcam/main/mjpeg_main.c`。HTTP MJPEG
  （80 番の `/status`、81 番の `/stream`）は診断用で、WebRTC 接続中は送信を止める。
- シグナリングは tolo-edge-bridge の Firestore。ESP は Device Authorization Grant で
  `event_access` を取得し、IssueFirestoreToken → Firebase Auth → Firestore REST を
  ポーリングする。
- Wi-Fi・Edge Bridge・IdP・Firebase・edgeDeviceId は UART0（115200bps）の
  `wifi` / `signaling` / `cloud` で NVS に保存する。NVS は既定で XTS-AES 暗号化。
- 解像度は HD 固定。Kconfig にも解像度を下げる項目は置かない。
- `U2`（AZ1117CH-3.3TRG1）は 1=GND、2 とタブ=OUT、3=IN。旧 AP7361C と異なる。

## コマンド

### ファームウェア

`esp_webcam/` で ESP-IDF v6.1 の環境から実行する。Docker では
`espressif/idf:v6.1` を使える。

```sh
git submodule update --init --recursive
idf.py set-target esp32s3        # 初回のみ
idf.py reconfigure               # Kconfig の項目を追加・変更したとき
idf.py build
```

ビルド後の書き込み（ESP-IDF の Python 環境）。`PORT` は実際のポート名に置き換える。

```sh
python tools/flash_serial.py --port PORT --check
python tools/flash_serial.py --port PORT
python tools/flash_serial.py --port PORT --erase      # NVS も消える
python tools/flash_debugprobe.py --port PORT
```

- `--erase` や `erase-flash` は設定・証明書・トークンを消す。通常の書き込みでは
  NVS は保持される。
- NVS 暗号化へ移行するときは全消去して再プロビジョニングする。
  `nvs_keys` だけを消してはいけない。
- 本番相当は `sdkconfig.secure.defaults` を使う。eFuse の HMAC 鍵はやり直せない。
- Pico debugprobe は GP4→GPIO44（RX）、GP5→GPIO43（TX）、GND 共通。
  書き込み時は IO0=GND、終了後は IO0 を外して EN リセット。
- 詳細は `esp_webcam/README.md` と `docs/tolo-edge-bridge-20261102.md`。

### ホストテスト

リポジトリ直下で実行する。C11 の `cc` と Python 3 が必要。
`managed_components/` を使うため、先に一度 `idf.py build` を通しておく。

```sh
python3 esp_webcam/tests/run_host_tests.py
```

変更した領域に応じて `esp_webcam/tests/test_*.py` も個別に実行する
（EAP、診断、コンソール、認可）。`viewer-server/` を触った場合だけ
`cd viewer-server && npm test` を実行する。

### 基板

リポジトリ直下から実行する。

```sh
report_dir=$(mktemp -d)
kicad-cli sch erc --exit-code-violations \
  --output "$report_dir/erc.rpt" board/esp_webcam.kicad_sch
kicad-cli pcb drc --schematic-parity --exit-code-violations \
  --output "$report_dir/drc.rpt" board/esp_webcam.kicad_pcb
```

- `kicad-cli` が PATH にない場合は、インストール先を確認して PATH を通す。
- 2026-11-16 時点: ERC 0、未配線 0、回路図不一致 0、DRC 14 件
  （穴径 12・シルク 2）。既知分と新規分を区別する。
- 違反を隠すために除外設定を足したり、根拠なく未接続マークや PWR_FLAG を
  追加したりしない。

## 守るべきこと

### KiCad

- KiCad 10 と互換性のあるツールを使い、不要な形式変換・全体整形をしない。
- 回路変更は回路図を起点にする。基板だけを変えて接続の不一致を残さない。
- 部品番号・UUID・ネット名をみだりに変えない。交換時はシンボルのピン番号、
  フットプリントのパッド番号、実部品の端子対応を突き合わせる。
- 専用シンボルは `board/esp_webcam.kicad_sym` と `board/sym-lib-table` で管理する。
- 電気仕様は採用する正確な型番のメーカー資料で確認し、URL・版・根拠を残す。
  推測で定数やピンを決めない。
- BOM は JLCPCB/LCSC 番号とメーカー型番を持つ。選定根拠は `docs/jlc-bom.md`。
- 一時レポートは一時ディレクトリへ出す。製造データで設計ソースを上書きしない。
- アンテナの銅箔禁止領域、USB コネクタの機械寸法、電源配線幅、GND の連続性を
  確認する。
- 既知の未解決事項は `docs/pcb-review-20261026.md`。配置済み・接続済みを
  検証済みとみなさない。

### USB・電源

- ESP32-S3 はカメラを接続する USB ホスト。カメラへの VBUS 給電、電流制限、
  逆流・ESD 対策まで設計に含める。
- 内蔵 USB PHY は GPIO19/20。USB Serial/JTAG・書き込み・デバッグとの競合を
  確認する。カメラ接続中は USB Serial/JTAG が見えなくなるため、書き込み時は
  カメラを外す。
- 目標の解像度・fps・消費電流から USB 帯域、Wi-Fi 帯域、PSRAM、電源のピーク電流を
  見積もる。
- 「UVC 対応」だけで任意のカメラが動くと扱わない。フォーマット・解像度・fps・
  転送方式を確認する。

### ファームウェア

- ESP-IDF v6.1 と submodule の固定コミットを維持する。`vendor/` は変更しない。
- `sdkconfig`、`sdkconfig.fast`、`build/`、`build-fast/`、`managed_components/`、
  `certs/`、`.tools/` はコミットしない。
- HD 1280×720 と 6fps 以上を維持する。速度の判断は `/rtc-status` の実測
  `display_fps` を使い、`effective_fps`（設定値）を達成値として扱わない。
- 送信経路を変えたら、実際に canvas へ描画されたフレームの ACK で評価する。
- 設定・認可・シグナリングの契約を変えたら、`esp_webcam/README.md` と
  `docs/tolo-edge-bridge-20261102.md` を同時に更新する。

### セキュリティ

- 資格情報を送る経路は HTTPS を必須にする。`CONFIG_WEBCAM_ALLOW_INSECURE_HTTP` は
  ローカル開発時だけ明示的に有効にする。
- `show` やログにはトークン本文を出さず、有効期限と有無だけを出す。
- 既定ビルドは鍵が無い・読めない場合に平文へフォールバックする。本番は
  `sdkconfig.secure.defaults` の `CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION` で
  平文起動を禁止する。
- 既存の平文 NVS 端末を暗号化へ移すときは、設定を控えて全消去・
  再プロビジョニングする。

## 作業の進め方

1. 変更前に対象ファイルと関連する回路・部品・ネット・ドキュメントを読み、
   現状を確認する。
2. 依頼範囲に絞る。既存の配置・配線・注記・命名を尊重する。
3. 結果を左右する未決事項（電源方式、目標映像仕様、採用部品など）は明示する。
4. 変更内容に応じて検証し、既存問題と新規問題を区別する。
5. 変更理由、確認結果、残る問題、実機でしか確認できない事項を報告する。

不明点があっても、既存資料から確認できる範囲は進める。
Git 操作は既存リポジトリを前提にし、依頼なしに初期化しない。

## 完了条件

- 基板変更: ERC/DRC、回路図と基板の一致、必要ならゾーン再充填と目視を確認する。
- ファームウェア変更: ホストテストと `idf.py build` を通す。実機でしか確認できない
  UVC 受信・DataChannel・描画 fps は、未確認と明記する。
- 文書のみ: ERC/DRC は不要。記載したパス・コマンド・現状との整合を確認する。
- ツールや実機がなく検証できない場合は、未実行の項目と理由を報告する。
- 未検証の項目を検証済みとして報告しない。実機検証の現状は `esp_webcam/README.md`
  を正とする。

## 詳細ドキュメント

- `README.md` — プロジェクト全体像とクイックスタート。
- `docs/README.md` — ドキュメント一覧と更新方針。
- `esp_webcam/README.md` — ビルド、書き込み、シリアル設定、運用手順、
  トラブルシューティング。
- `docs/tolo-edge-bridge-20261102.md` — Firestore シグナリング、認可、Firestore 契約。
- `docs/nvs-encryption.md` — NVS 暗号化、パーティション、移行、セキュアビルド。
- `docs/pcb-review-20261026.md` — 基板の成立条件と未解決事項。
- `docs/power-input.md` — USB Type-C 給電と CC 抵抗。
- `docs/jlc-bom.md` — JLCPCB/LCSC の部品選定。
- `docs/regulator-review.md` — レギュレータ U2 の選定根拠。
- `docs/camera-349c-3307.md` — カメラの descriptor 調査。
- `docs/webrtc-performance-20261101.md`、`docs/webrtc-profile-20261101.md` — 性能測定。
- `docs/wifi-enterprise-20261102.md` — 802.1X/EAP。
