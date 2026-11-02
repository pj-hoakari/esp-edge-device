# WebRTC送信高速化（2026-11-01）

後続のUART/HTTP実機プロファイルと暗号方式の比較は
[実機プロファイル](webrtc-profile-20261101.md)に記録した。
以下は初回高速化と30秒測定の記録で、最新方式はChaCha20-Poly1305優先へ変更している。

要件はカラーHD 1280×720を維持し、静止時も描画6fps以上。既定はUVC受信10fps・
送信目標10fpsに変更した。当初は実機不在だったが、同日にCOM3のPico debugprobe UART経由で
ESP32-S3へ書き込み、全3領域のハッシュ照合に成功した。
通常起動とWebRTC開通も確認した。下記30秒測定では平均6fpsを超えたが、
5秒区間では6fpsを下回り、常時維持の要件は未達。
長時間性能、エンドツーエンド遅延、Wi-Fi電波条件別の性能は未検証。
目標値・送信APIの成功枚数だけで、描画6fps以上を達成したとは判定しない。

## 変更と根拠

| 箇所 | 変更 | 目的 |
| --- | --- | --- |
| 送信周期 | 既定6→10fps、設定下限6fps。Idle FPSと同一画像抑制を撤去 | 静止時2fps・同一画像約1fpsへの低下を廃止し、欠落への余裕を持つ |
| ペーシング | 毎回now＋周期にせず前回の周期位相から次回を算出 | タスク起床・処理時間の遅れが周期ごとに累積するのを防ぐ |
| 新着待ち | 新しいJPEGがなければ次のイベントループで再確認 | 以前の周期1/4（25〜42ms）の追加待機を除去 |
| DataChannel | 最大4断片/4msのバースト、受信待ち2→1ms | 断片ごとのイベント待ちとタスク切替を削減。バーストの合間にACK/SACK/ICEを処理 |
| 再試行 | WOULD_BLOCK中は作成済みパケットを保持 | PSRAMからの同じ12KBコピーを繰り返さない |
| UDP | バッファ不足時20ms＋selectの待機を1ms×最大2回へ短縮 | 下位送信で長時間止まらず、上位へバックプレッシャーを返す |
| メモリ | 送信用断片バッファを内部RAMへ明示配置。CRC32計算撤去 | 暗号化前の読み出しとUVCコールバックの処理量を削減 |
| シグナリング | 映像接続中のpollを4Hz→1Hz | HTTP接続生成と無線の競合を削減。接続差し替えの検出は最大約1秒遅くなる |
| ブラウザ組立 | 最大3フレームの並行組立 | unordered配送でN+1が先着してもNをすぐ捨てない。最大768KiBの組立バッファ |
| ブラウザ描画 | 動き検出とgetImageDataを撤去。デコード待ちは最新1枚 | 画像読み戻しをなくし、デコード完了後に次の受信周期を待たず描画を継続 |
| ACK | 実際にcanvasへ描いたフレームのみ | デコード中に捨てたフレームを表示済みとして数えない |

断片のワイヤ形式（JPG1、20-byteヘッダー、12,000-byte payload）は維持する。
送信済みと数えるのは全断片を送信APIへ受け渡せた時点であり、配送完了ではない。
古いフレームの送信打ち切りは250msで、遅いリンクでの未完成JPEG蓄積を制限する。
バーストの4ms判定は各断片の送信開始前に行うため、実行中の送信呼び出しの時間は追加される。

UDP変更は`main/udp_fast.c`によるweakシンボル`udp_socket_sendto`の置換。
固定したesp_peer 1.5.7のprivateヘッダーと、再試行要求の戻り値`-200`に依存する。
送信成功を偽装せず、ENOBUFS/ENOMEM/EAGAIN/EWOULDBLOCKのみ再試行し、
その他の失敗は直ちに返す。UDPソケットのユーザー数・IPv4/IPv6の扱いを維持する。
依存の更新時はABIと戻り値を再確認する。vendorとsubmoduleのコミットは変更していない。

## ESP-IDF／通信スタック設定

| 設定 | 変更 | 負担・意図 |
| --- | --- | --- |
| `COMPILER_OPTIMIZATION_PERF` | Debug→速度優先 | アプリ、lwIP、Mbed TLSのコンパイル対象に適用。既成のpeerバイナリは再最適化されない |
| `LWIP_IRAM_OPTIMIZATION` | 有効 | 通信の処理を内部命令RAMに置く。SDK説明では約10KiBのIRAMを使用 |
| `LWIP_TCPIP_RECVMBOX_SIZE` | 32→64 | TCPIPタスク待ちのパケット破棄を緩和 |
| `LWIP_UDP_RECVMBOX_SIZE` | 6→32 | バースト中に到着するSCTP SACK／ACK／ICEの受信余裕 |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 32→48 | UDP送信バーストを吸収。使用中のバッファに応じ内部RAM消費が増える |
| `ESP_WIFI_TX_BA_WIN` | 6→12 | AMPDUの送信窓を拡大。AP・電波条件により効果が異なるため実測が必要 |

AES-GCM優先、ハードウェアAES/SHA、240MHz、80MHz Octal PSRAM、Wi-Fi省電力無効は
既存で有効なことを確認した。TLS/DTLSの暗号化を維持する。映像の実経路は
JPEG→SCTP→DTLS→UDP→Wi-Fiで、シグナリングHTTPのTCP窓を増やしても映像の主経路には効かない。
TCP専用の追加IRAM最適化（SDK説明で約17KiB）は有効にしていない。

設定は`sdkconfig.defaults`にも残すが、既存の`sdkconfig`の値が優先される。
既存環境では上表と`WEBCAM_TARGET_FPS=10`をmenuconfigで合わせること。
Wi-Fi認証情報を含むローカルsdkconfig・ファームウェアを公開しない。

## PCでの確認と実機での判定

- `viewer-server`で`node --test tests/*.test.mjs`：13件成功。
  断片の交錯、uint32ラップ、組立メモリの上限、デコード待ち最新フレーム、
  描画のみACK、切断後の古いデコード結果破棄、中継APIとICE候補を検証。
- `esp_webcam/tests/sender_policy_test.c`：MSVCの`cl /W4 /O2`で実行成功。
  6〜15fpsの10,000周期ずつで起床遅れの非累積、停止後の周期スキップ、
  UDPの回復・最大2ms待機・再試行通知・即時エラーを実コードで確認。
- ESP-IDF v6.1 / ESP32-S3のビルド・リンク成功。app 1,161,584 bytes（0x11b970）、
  アプリ領域24%空き。IRAM text 99,047 bytes、DRAM data 22,083 bytes・bss 33,888 bytes。
  これらは静的セクションで、Wi-Fi／TLSの実行時ヒープ消費は含まない。
- 最終ELFの`nm`で`42012920 T udp_socket_sendto`を確認。`main`を`WHOLE_ARCHIVE`で
  リンクし、元のweak実装に解決されて独自フックが未採用になることを防いだ。

`/rtc-status`に`send_fps`（全断片をAPIへ渡せた枚数）、`display_fps`（描画ACK枚数）を追加した。
直近約5秒の区間値を約5秒ごとに更新する。`effective_fps`は設定目標値。
`mean_send_ms`・`max_send_ms`はPSRAMから読み出した後、全断片をAPIへ渡すまでの時間で、
ブラウザまでの遅延ではない。`would_block`はSCTP側の送信満杯回数。
旧ビューアーのACKは破棄フレームも含むため、最新ビューアーとセットで測定する。

実機を用意したら再ビルド・書き込みし、ビューアーを再読み込みして接続する。
接続安定後、静止・動き・暗所・細かな模様を各10分測り、ブラウザの直近fpsと
`display_fps`、`sent_frames`/`acked_frames`の差分、JPEGサイズ、drop/error、
内部RAM残量、リセット有無を記録する。各5秒区間で30枚以上の描画を必須とする。
画面のdrawImage呼び出し・ACKは物理ディスプレイの走査を直接計測するものではない。
撮影から表示までの遅延は外部時計などで別途測定する。

## 同日実機測定

COM3のPico debugprobe UART経由で書き込み・全3領域ハッシュ照合後、通常起動を確認。
DHCP IPは今回`192.168.137.145`。`/status`では720p・設定10fps、633枚受信すべて有効JPEG、
USB転送エラー0、バッファ超過0。WebRTCはpeer_state=10、channel_open=true。

稼働中のブラウザ接続を変えず、`/rtc-status`を約5秒ごとに読み、
`sent_frames`と`acked_frames`の差分をPCの単調時計で計測した。

| 区間 | 秒 | 送信fps | 描画ACK fps |
| --- | ---: | ---: | ---: |
| 1 | 5.055 | 7.912 | 7.715 |
| 2 | 5.005 | 7.792 | 7.792 |
| 3 | 5.012 | 7.582 | 7.582 |
| 4 | 4.981 | 7.629 | 7.830 |
| 5 | 4.994 | 5.206 | 5.206 |
| 6 | 5.000 | 5.400 | 5.400 |

全30.048秒で送信208枚、描画ACK208枚、平均6.922fps。
送信エラーと送信途中破棄の増加0、DataChannel開通は維持。
累積の平均送信処理時間127ms、累積最大245ms。
**5秒区間の最低5.206fpsなので、常時6fps以上の要件は達成していない。**
被写体・JPEGサイズ・Wi-Fi条件を固定した比較や長時間測定は行っていないため、
低下の原因や変更前との差はこの測定だけでは特定できない。

## 参照した固定ソース

- ESP-IDF v6.1.0 (`fff9895c82d744c7237be8847347bdd1b07c6643`)：
  `.tools/esp-idf/components/lwip/Kconfig`、`esp_wifi/Kconfig`、`mbedtls/Kconfig`。
- [esp_peer固定コミットのUDP実装](https://github.com/espressif/esp-webrtc-solution/blob/38b175077967109e764d434d680e641d9e87b23c/components/esp_peer/src/transport/udp.c)：weakフックと-200規約、元の20ms再試行。
- [同じ版のDTLS実装](https://github.com/espressif/esp-webrtc-solution/blob/38b175077967109e764d434d680e641d9e87b23c/components/esp_peer/src/dtls_srtp_v6.c)：Mbed TLS利用。今回はローカル固定ソースを確認した。
