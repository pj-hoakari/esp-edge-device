# esp_webcam 開発ガイド

## 目的と現在の構成

USB UVCカメラの映像をESP32で受信し、WebRTCで配送する機器を作る。
このディレクトリはKiCadによるハードウェア設計が中心。説明・作業報告は日本語で行う。

- `esp_webcam.kicad_pro`: プロジェクト設定・設計ルール。
- `esp_webcam.kicad_sch`: 回路図。回路接続と部品情報の変更元。
- `esp_webcam.kicad_pcb`: 基板。配置・配線・基板外形。
- `esp_webcam-backups/`: KiCadのバックアップ。通常の編集対象にしない。

設計ファイルはKiCad 10形式で、ESP32-S3-WROOM-1-N8R8、USB1046、
AZ1117CH-3.3TRG1を使用する。全11部品にJLCPCB/LCSC番号とメーカー型番を設定済み。
U2は秋月116155（1個30円）、JLCPCB C92102。出力C4は22µFに変更済み。
U2は1=GND、2とタブ=OUT、3=IN。以前のAP7361Cとはピン配列が異なる。
専用シンボルは `esp_webcam.kicad_sym` と `sym-lib-table` で管理する。
配置されている部品・接続を検証済みの設計とみなさない。
電源入力にはJ2（TYPE-C-31-M-12 / JLCPCB C165948）を使用し、CC1・CC2を
R2・R3（各5.1kΩ、1%）で個別にGNDへ接続する。選定根拠と基板への反映状況は
`docs/power-input.md` を参照する。
全BOMは `exports/BOM_JLCPCB.csv`、選定根拠は `docs/jlc-bom.md`、
レギュレータの調査結果は `docs/regulator-review.md` にある。
SMD化でパッド位置と電源ピン配列が変わったため、流用できない旧配線は除去済み。
2026-10-26の再配線後は未配線0件、ERC0件、回路図との不一致0件。
穴径違反12件・シルク警告2件が残る。書き込み経路、電源・GND、USB配線と給電の見直しが必要。
カメラは「HD video」、VID:PID=349C:3307（メーカー型番未特定）。PCでFull Speed接続と
720p MJPEGの10/15fps広告を確認済み（`docs/camera-349c-3307.md`）。
2026-10-31の最新依頼で、目標はカラーHD（1280×720）・6fps以上の維持（余裕があれば高速化）、
JPEGをWebRTC DataChannelで送る方式に変更した。一般的な映像トラックは使用しない。
構成の成立条件と検証結果は `docs/pcb-review-20261026.md` を参照する。
交換前は `esp_webcam-backups/pre-jlc-smd-20261026.zip` に保存した。
`esp_webcam/` にESP-IDF v6.1のUVC MJPEG受信・DataChannel送信ファームウェア、
ブラウザ受信画面とシグナリングサーバーはルートの`viewer-server/`へ分離し、
ESPはビューアーを配信しない。ビルド設定・実機検証手順は`esp_webcam/README.md`にある。
対象カメラに合わせ既定値は1280×720・10fps受信・送信目標10fps（`Send target FPS`で変更）。
2026-11-01の依頼により静止時も6fps以上を必須とし、Idle FPS・同一画像抑制を撤去した。
解像度はHD 1280×720固定で、Kconfigにも解像度を落とす項目は持たない。
USB受信は広告fpsの帯域を消費する。
ESP-IDF v6.1 / ESP32-S3向けビルド・リンク成功、受信処理とシグナリング中継のテスト7件成功。
接続された実機へ書き込み・ハッシュ照合成功。IO0のGND保持解除後、Wi-Fi接続と
診断画面・`/status`のHTTP 200応答を確認済み。UVC・DataChannel・映像は未検証。
その後の切り分けでICE・DTLS成功、SCTP/DataChannel未開通を再現した。
現在はユーザー依頼により、`esp_webcam/main/mjpeg_main.c`をエントリーポイントとする
HTTP MJPEG確認版へ切り替えている。画面はHTTPの80番、診断は`/status`、映像は81番の`/stream`。
`main.c`のWebRTC（DataChannel）も同じビルドから起動する。受信画面は`viewer-server/`から配信し、
ESPはビューアーを配信しない。シグナリングはHTTPポーリング（ESPが`/api/esp/offer`を取得し
`/api/esp/answer`へPOST）、ICEはSTUN利用。URLはKconfigの`WEBCAM_SIGNALING_URL`・
`WEBCAM_STUN_URL`で設定する。カメラ検出・受信の確認状況はREADMEの現在動作を参照する。
MJPEG確認版も書き込み・ハッシュ照合、通常起動後のWi-Fiと診断API応答を確認済み。
初回はUSB機器数0だったが、USB制御転送上限を256→4096 bytes、FIFOをBias INへ修正後、
USB機器1台・UVC検出1件、720p JPEG受信・PCでの3枚のデコード・ブラウザ表示を確認した。
J1 VBUSはユーザー実測4.97V。10.137秒のカウンタ測定ではUSB受信9.964fps、有効JPEG2.466fps、
無効判定破棄76枚、USB転送エラー・バッファ超過0。有効率約25%は、EOI後の整列パディングを
最大3バイトしか許さず16バイト境界の0〜15バイトのうち4/16だけ通ったためと推定した。
長時間性能は未確認。設計ソース側はGitリポジトリを初期化済みで、`esp_webcam/vendor/`の
esp-usbとesp-webrtc-solutionはsubmoduleとしてコミットを固定している。
最新ソースは6fps以上の常時維持を目標にした送信高速化版。JPEG構造検査とEOI後の
整列データ除去、位相を維持する10fpsペーシング、最大4断片/4msのバースト送信、
送信満杯時のパケット再利用、CRC32計算撤去、ACK非ゲート化を実装した。
DataChannel再送0（Kconfig 0〜3）、ESP側生成チャンネル、ChaCha20-Poly1305優先、CPU1配置、
独立フレーム購読、ソケット上限20を維持する。解像度はHD 1280×720固定。
ブラウザは3フレーム分の断片順序入れ替えに対応し、デコード待ちは最新1枚だけ保持する。
動き検出の画像読み戻しを撤去し、実際にcanvasへ描いたフレームだけACKする。
HTTP MJPEGも目標10fpsで、WebRTC接続中はMJPEG送信を休止する。
`/status`は破棄理由別カウンタと目標fps、`/rtc-status`は送信・描画ACKの直近約5秒fps、
送信処理時間・WOULD_BLOCK件数を返す。`effective_fps`は設定値であり実測fpsではない。
UDP送信の既定20ms再試行待機を`main/udp_fast.c`のweakフック置換で1ms×最大2回へ短縮した。
送信失敗の-200規約は維持し、vendor/submodule自体は変更しない。
速度優先コンパイル、lwIP IRAM配置、TCPIP mailbox 64、UDP mailbox 32、
Wi-Fi TX buffer 48、TX BA window 12を` sdkconfig.defaults `とローカル設定へ反映した。
詳細・メモリ負担・再測定手順は`docs/webrtc-performance-20261101.md`を参照する。
`/log`は`DTLS`・`esp_peer`・`SCTP`・`ICE`・`webcam:`の直近32行を返し、
Wi-Fi経由でハンドシェイク失敗を切り分けられる。
JPEG解析の同一CコードをMSVCでPC実行するテスト（末尾0〜16バイト・破棄理由を含む）と
2026-10-31時点の受信側テスト5件、シグナリング中継・候補展開テスト4件、ESP-IDFビルド・リンクは成功
（app 1,188,272 bytes、0x1221b0）。
2026-10-31にDataChannel経由のカラーHD映像を実機で確認した。`/rtc-status`は
`peer_state:10`、`channel_open:true`、`sent_frames:105`、`acked_frames:79`、
`last_ack_rtt_ms:120`、`effective_fps:2`、`client_motion:3`。ブラウザ表示もユーザーが確認した。
開通しなかった原因は2つ。`peer_task`のフレーム開始分岐で`size = n;`が抜けてJPEGを
一度も送っていなかったこと、Nodeサーバーの候補展開が`a=candidate:`を落とし
`.local`を`127.0.0.1`へ置換していたこと。両方修正済み。
2026-11-01版は当初実機不在だったが、同日にCOM3のPico debugprobe UART経由で
ESP32-S3 rev.0.2へ書き込み、bootloader・partition table・appのハッシュ照合に成功した。
WindowsのCDC UARTではDTR有効とタイムアウト変更時のUART再設定回避が必要だった。
再現用の`esp_webcam/tools/flash_debugprobe.py`は生成済みbuildを書き込み、`--check`で接続のみ確認する。
Pico GP4→ESP GPIO44/RXD0、GP5←GPIO43/TXD0、GND共通。書き込み時はIO0=GNDでENリセット、
書き込み後はIO0のGND接続を外してENリセットする。通常起動、Wi-Fi接続（今回のDHCPは
192.168.137.145）、720p MJPEG受信10fps、診断API応答、WebRTC開通を確認済み。
30.048秒の測定で送信208枚・描画ACK208枚（平均6.922fps）。5秒区間の最低は5.206fpsで、
期間中の送信エラー・送信途中破棄0。常時6fps以上の要件は未達、長時間性能は未検証。
同版はESP-IDF v6.1 / ESP32-S3でビルド・リンク成功（app 1,161,584 bytes、0x11b970、
アプリ領域24%空き）。最終ELFで高速UDPフックの強いシンボルへの置換を確認済み。
受信・描画・中継の13テストとC送信周期／UDPバックプレッシャーのテストが成功した。
同日、UARTとHTTPで実機の処理時間を測る`/perf`と、実験設定をNVSへ保存できる
`POST /rtc-config`を追加した。近いJPEGサイズでAES-GCM平均6.51fpsに対し
ChaCha20-Poly1305平均6.98fpsだったため、ChaCha20優先・AES-GCMフォールバックを採用。
暗号ライブラリの末尾に-O3を適用し、4断片/4msでChrome実デコード・canvas描画を
180.326秒測った結果は平均6.738fps、最低5秒区間6.165fps。大バーストは採用しない。
全被写体・電波条件での常時6fps維持は未保証。計測器・処理時間・制約は
`docs/webrtc-profile-20261101.md`、再現ツールは`viewer-server/tools/benchmark-browser.mjs`。
UDP受信nowait版の180.368秒測定は平均6.825fps・固定5秒最低6.195fpsだったが、
ローリング5秒では最低5.4fps、最大描画間隔617msがあり、常時維持の要件は未達。
同期間のUSB受信1,746枚は全数有効JPEG、送信エラー0・送信途中破棄5、自発リセット検出0。
UVC受信停止時のstop/close/reopen監視と`stream_restarts`を追加したが、無応答注入試験は未実施。
`esp_webcam/vendor/` の2リポジトリはsubmodule、`.tools/esp-idf` はGit除外のローカルSDK取得である。
2026-11-02、UART0（115200bps）にネットワーク設定コマンドを追加した。
`wifi`でSSID・パスワード、`signaling`でサーバーIPv4/ホスト名/HTTP URLを対話入力し、
専用NVS名前空間`network`へ保存する。`show`で現在値・保存値を表示し、`reboot`で反映する。
パスワード本文はエコー・表示しない。Wi-Fi未設定・未接続でも設定できる。
保存値はmenuconfigより優先し、RTC調整値とは独立する。STUNは従来通りKconfigで設定する。
実装は`main/network_settings.c`・`network_settings_values.c`・`serial_input.h`、操作手順はREADMEを参照。
入力検証・シリアル行処理のPCテスト、受信・描画・中継13テスト、ESP-IDF v6.1のビルド・リンクに成功。
appは1,262,080 bytes（0x134200）、アプリ領域18%空き。
同日、ユーザー指示によりCOM3のPico debugprobe UART経由でESP32-S3 rev.0.2へ書き込み、
bootloader・partition table・appのハッシュ照合に成功した。書き込み後はbootloaderで待機。
実機での設定保存・再起動後の接続先変更は未検証。
同日、UARTログで入力欄が流れる問題を修正し、設定画面ではログ表示を停止する。
`logs on`で表示を再開し、Enterまたは任意の入力で停止して設定へ戻る。HTTP用ログ記録は継続する。
コマンド・SSID・サーバー入力はエコーし、パスワードは`*`でマスクする（端末のローカルエコーはOFF）。
`tests/test_network_console.py`で実コンソール処理のログ切替・CRLF・マスク・キャンセル・NVS再読込みが成功。
修正版もESP-IDFビルド・リンク成功（app 1,262,816 bytes、0x1344e0、アプリ領域18%空き）。
COM3経由で書き込み・全イメージのハッシュ照合後、IO0解除・ENリセットで通常起動。
実機でログ停止中・再停止後の各3秒間のUART追加出力0バイト、`help`・ログ切替・Enter復帰・
SSID入力表示・パスワードのマスク・Wi-Fi/サーバー入力のキャンセルを確認した。
確認用入力は保存していない。COM3は確認後に解放済み。
続く依頼で802.1X/EAPを追加。`eap`でTLS、PEAP/MSCHAPv2、TTLSのEAP/MSCHAPv2/MSCHAP/PAP/CHAPを選ぶ。
WPA/WPA2/WPA3-Enterprise・Suite-B用設定、CA/サーバー名/期限の検証設定、`clock`を追加した。
`protocols`はビルド時の対応方式、`credentials`は資材の保存有無を表示する。
証明書・秘密鍵・PACは`tools/upload_wifi_credential.py`で転送し、`wifi_certs`へ保存する。
SDKの制約でFASTはMbedTLS Wi-Fi TLSクライアントと両立しないため、`sdkconfig.fast.defaults`の別ビルド。
通常版は`esp_webcam/build/`、FAST版はルート`build-fast/`。両方ビルド・リンク成功。
appは通常版1,374,928 bytes（0x14fad0、10%空き）、FAST版1,357,040 bytes（0x14b4f0、12%空き）。
設定blobはv2。v1の328バイトblobからSSID・PSK・シグナリングを保持して移行し、RTC調整は維持する。
EAP設定・資材転送・移行・認証APIのPCテストと既存13テスト成功。802.1X実接続・映像性能は未確認。
通常版はCOM3経由で書き込み・全イメージのハッシュ照合、IO0解除後の通常起動に成功。
実機でEAP方式一覧・入力キャンセル・ログ停止、SDK公開サンプルCA/クライアント証明書/秘密鍵の
解析・NVS保存・削除に成功した。サンプルは全削除し、接続プロファイルは変更していない。
FAST版の実機書き込み・RADIUSでの認証は未実施。COM3は解放済み。
詳細・SDK根拠・実機の検証状況は`docs/wifi-enterprise-20261102.md`を参照する。
続くユーザー報告で802.1X Wi-Fiへの接続成功を確認（全EAP方式・検証設定の実証ではない）。
現在IPを`show`に追加し、`ping [IPv4]`を追加。pingは3回・1秒間隔/応答待ち、宛先省略時はゲートウェイ。
未接続時は古いIPを表示・使用しない。`check signaling`は状態を変えない`GET /ice`のHTTP応答を確認、
`check stun`は設定済みSTUNへUDP Bindingを1回だけ送信し、要求IDに合う応答を検証する。
ユーザー指定により、確認コマンドは疎通確認だけを行い、設定保存・SDP交換・ICE/WebRTC接続を開始しない。
実装は`main/network_diagnostics.c`・`network_probes.c`・`stun_probe.h`。
IP/ping・疎通専用API・異常応答のPCテストと、`GET /ice`の状態不変を含む14テストが成功した。
診断版の通常ビルド成功（app 1,382,560 bytes、0x1518a0、アプリ領域10%空き）。
FAST互換版もビルド成功（app 1,364,720 bytes、0x14d2f0、アプリ領域11%空き）。
診断版の通常版はCOM3経由でESP32-S3へ書き込み、全イメージのハッシュ一致を確認済み。
NVSの設定・証明書は保持し、書き込み後はbootloaderで待機。IO0解除・ENリセットが必要。
診断版の通常起動後のIP表示・疎通結果は実機未確認。
2026-11-02、シグナリングをCloudflare TunnelのHTTPSへ接続できるようにした。
`https://host`は443を補完し、裸ホストは`http://host:8080`のまま。パス・クエリ・
ユーザー情報・IPv6は拒否する。CAはesp_crt_bundleを使用し、Cloudflareの
`WE1`→`GTS Root R4`（GlobalSign Root CAによるクロス署名）を検証するため
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`と
`CONFIG_MBEDTLS_X509_TRUSTED_CERT_CALLBACK`を有効にした。`check signaling`は
`status=200`で成功。映像接続中はofferポーリングを停止し、DataChannel切断または
10秒ACK途絶で再開する。実機の`/rtc-status`で`channel_open:true`、送信6.394fps、
描画6.394fps、送信エラー0、破棄5を確認した。診断用の相手IP表示は実装後に撤去し、
書き込んでいない。`viewer-server/tools/benchmark-browser.mjs`は`VIEWER_URL`で
Tunnel先を指定できる。appは1,383,056 bytes（0x151a90、アプリ領域10%空き）。
2026-11-02、シグナリングを tolo-edge-bridge の Firestore 方式へ移行した。
ESP は viewer-server を使わず、`signaling`（Edge Bridge の base URL）、`cloud`
（Firebase project/API key/emulator ホスト、edgeDeviceId）、`token`（事前発行 event_access）
をシリアルで設定する。ESP は IssueFirestoreToken → Firebase Auth → Firestore REST の
ポーリングで sessions/signals を読み書きし、観測ページ（console 役）と `jpeg` DataChannel を
確立する。観測ページは `?camera=<ESPのedgeDeviceId>` で ESP を映像ソースにする。
viewer-server の HTTP ポーリング経路は非使用。Firestore/Auth のエミュレータは
`host:port` のほか `https://host` も指定でき、HTTPS のリバースプロキシ構成に対応する。
通常の COM ポート書き込みは `tools/flash_serial.py`、Pico debugprobe は
`tools/flash_debugprobe.py` を使う。設定・手順は README の「tolo-edge-bridge 接続」と
`tolo-web/README.md` の「ESP32 カメラを観測ページにつなぐ」を参照する。
2026-11-03のPhase 11で、8MB用のカスタム`partitions.csv`（`nvs_keys`付き）と
`main/nvs_boot.c`を追加し、`CONFIG_WEBCAM_NVS_ENCRYPTION`（既定`y`）で
NVSをXTS-AES暗号化するようにした。鍵が無い・読めない・暗号化初期化に失敗した場合は
警告を出して暗号化なしへフォールバックする。既存の暗号化なし端末は、全消去して
再プロビジョニングするまで設定が読めない場合がある。暗号化の詳細、消去手順、
パーティション表はREADMEの「NVS暗号化とパーティション」を参照する。
DockerのESP-IDF v6.1でビルド成功（app 1,428,736 bytes、0x15cd00、アプリ領域7%空き）。
ホストのNVS判定テスト、cloud認証テスト、network設定テスト、シリアルプロトコルテストが成功した。
実機の書き込み・暗号化NVSの再起動確認は未実施。

作業開始時に実際の構成を確認し、構成変更に合わせてこの文書を更新する。

## 作業の進め方

1. 対象ファイルと関連する回路・部品・ネットを読み、変更範囲を把握する。
2. 電気仕様に関わる判断は、採用する正確な型番のメーカー資料で確認する。
   資料のURL・版と判断根拠を報告または設計メモに残す。推測で部品定数やピンを決めない。
3. 依頼範囲に絞って変更し、既存の配置・配線・注記を尊重する。
4. 変更内容に応じてERC、DRC、回路図と基板の一致、見た目を確認する。
5. 変更理由、確認結果、残る問題、実機でしか確認できない事項を報告する。

不明点があっても既存資料から確認できる範囲は進める。
電源方式、目標映像仕様、採用部品など、結果を大きく左右する未決事項は明示する。

## KiCadファイルの扱い

- KiCad 10と互換性のあるツールを使う。不要な形式変換・全体整形をしない。
- 回路変更は回路図を起点にし、基板へ反映する。基板だけを変更して接続の不一致を残さない。
- 部品番号、UUID、ネット名をむやみに変更しない。交換時はシンボルのピン番号と
  フットプリントのパッド番号、実部品の端子対応を確認する。
- 直接テキスト編集する場合は対象を限定し、編集後にKiCadで読み込めることも確認する。
- `*.kicad_prl` はローカル表示設定、`~*.lck` は編集中のロックとして扱う。
  依頼と無関係に変更・削除しない。ロックがある場合はGUIとの同時編集を避ける。
- 一時レポートは一時ディレクトリに出す。製造データは出力先を分け、設計ソースを上書きしない。
- Git操作はリポジトリの存在を確認してから行う。依頼なしに初期化しない。

## 設計時に確認すること

### ESP32・電源

- 「ESP32」という名称だけで機能を判断せず、SoC、モジュールの末尾型番、
  Flash・PSRAM容量と使用可能なピンを確認する。
- 3.3V電源の許容電圧、ピーク電流、デカップリング、EN・起動ストラップ、
  リセット・書き込み・デバッグ経路を確認する。
- レギュレータの入力条件、ドロップアウト、損失・発熱、安定動作に必要な容量とESR、
  パッケージ・ピン配列を確認する。回路図の値とフットプリント名だけで適合を判断しない。
- カメラと無線通信を含む電流予算を見積もり、給電元・GND・保護回路まで追う。

### USB UVC入力

- ESP32側はカメラを接続するUSBホストとして設計する。
  データ線だけでなく、カメラへのVBUS給電、電流制限、逆流・ESD対策を検討する。
- 選択したSoCのUSBポート、ピン割り当て、対応速度、ホスト機能を公式資料で確認する。
  書き込み・デバッグとUSBホスト利用の競合も確認する。
- D+/D−の極性と端子対応を確認する。配線は製造する基板の層構成に基づいて
  インピーダンス・リターンパスを設計し、分岐や不要なビアを抑える。
- カメラのフォーマット、解像度、fps、転送方式、消費電流と、採用するUSBスタックの
  対応範囲を確認する。「UVC対応」だけで任意のカメラが動くと扱わない。

### WebRTC・ファームウェア

- 目標の解像度・fps・遅延・同時接続数を明らかにして、USB帯域、Wi-Fi帯域、
  RAM・PSRAM、フレームバッファ、処理負荷を見積もる。
- UVC入力フォーマットとWebRTCで使用するコーデックの関係を確認する。
  MJPEG等の入力をそのままブラウザに配信できる、またはESP32上で変換できると仮定しない。
- シグナリング、ICE/STUN/TURN、DTLS-SRTP、接続先との相互運用を設計に含める。
  ハードウェアの成立とWebRTC配信の成立は、それぞれ検証する。
- ファームウェアを追加する際は、SDK・依存ライブラリ・バージョン・対象ボードを明記し、
  ビルド、書き込み、UVC受信、WebRTC接続の再現手順を残す。
- Wi-Fi認証情報や秘密鍵をソース・設計ファイル・ログへ埋め込まない。

### 基板

- モジュールのアンテナ配置・銅箔禁止領域はメーカー推奨に従い、全層と周囲の部品を確認する。
- USBコネクタの機械寸法、基板端との位置関係、固定方法を確認する。
- 電源配線幅、GNDの連続性、クリアランス、基板外形、製造条件を確認する。

## 検証

回路または基板を変更する前後に、関係するチェックを実行して既存問題と新規問題を区別する。
以下はKiCad CLI 10で使用できる例。プロジェクト直下から実行する。

```sh
kicad-cli version
report_dir=$(mktemp -d /tmp/esp_webcam-check.XXXXXX)
kicad-cli sch erc --exit-code-violations --output "$report_dir/erc.rpt" esp_webcam.kicad_sch
kicad-cli pcb drc --schematic-parity --exit-code-violations --output "$report_dir/drc.rpt" esp_webcam.kicad_pcb
```

- CLIがPATHにない場合はインストール先を確認する。macOSでは
  `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` も候補。
- 終了コードとレポートの内容を確認する。ERC/DRCを通すためだけに違反を除外したり、
  根拠なく未接続マークやPWR_FLAGを追加したりしない。
- 基板の変更後は必要に応じてKiCadでゾーンを再充填し、その状態でDRCを確認する。
- ERC/DRCだけで部品選定、信号品質、熱設計、映像配送の動作確認まで完了したとしない。
- 文書だけの変更ではERC/DRCは不要。記載したパス・コマンド・現状との整合を確認する。
- ツールや実機がなく検証できない場合は、未実行の項目と理由を報告する。
