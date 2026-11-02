# WebRTC実機プロファイル（2026-11-01）

カラー1280×720、UVC入力10fps、送信目標10fps、再送0のDataChannelで測定した。
JTAGは使用せず、COM3のPico debugprobe UART、HTTP診断、専用のChrome headlessを使用した。
Chromeでは製品の受信コードでJPEGをデコードし、video canvasのdrawImage完了を数える。
ESPの送信API成功数や設定fpsを描画fpsとして代用しない。

## 暗号とバーストの比較

| 暗号・設定 | 測定秒 | 描画fps平均 | 最低5秒区間fps | JPEG平均bytes |
| --- | ---: | ---: | ---: | ---: |
| AES-128-GCM、SDK既定の暗号ビルド | 40.238 | 6.511 | 6.193 | 45,927 |
| ChaCha20-Poly1305、同ビルド | 60.208 | 6.976 | 6.590 | 45,908 |
| ChaCha20、8断片/16ms | 60.188 | 6.928 | 6.456 | 46,161 |
| AES-128-GCM、暗号ライブラリ-O3 | 45.224 | 6.545 | 5.933 | 44,171 |
| ChaCha20-Poly1305、暗号ライブラリ-O3 | 60.204 | 7.126 | 6.712 | 44,145 |
| ChaCha20、8断片/64ms、-O3 | 60.402 | 6.970 | 6.123 | 44,599 |
| ChaCha20、4断片/4ms、-O3、連続測定 | 180.326 | 6.738 | 6.165 | 45,640 |

数値と処理別差分は[集計JSON](webrtc-profile-20261101.json)にも保存した。

被写体を固定する治具はなくJPEGサイズも変動するため、暗号以外の差をすべて
最適化の効果とみなすことはできない。特に以前の60秒ベースラインは約55.9KB/枚で、
上表より大きい。単純な改善率には使用しない。
近いJPEGサイズで比較できた暗号切替ではChaCha20が速く、既定の優先方式に採用した。
これは簡易暗号ではなく、WebRTCのDTLS認証付き暗号化を維持する変更である。
相手が対応しない場合はAES-GCMへフォールバックする。

8断片/64msではSCTP WOULD_BLOCKとUDP失敗が増え、内部RAMの起動後最低残量が756 bytesまで減った。
平均fpsも改善しないため4断片/4msを採用する。
最低残量は起動以来の値なので、その後小さいバーストへ戻しても756のまま残る。
これを現在の空きRAMやメモリリーク量とは扱わない。
さらにIDFの最低空きAPIは各ヒープ領域の最低値を合計するため、
全領域を同時に観測した空きRAMの厳密な最小値ではない。低い値だけからOOM発生とは断定しない。

3分測定の各5秒区間は6fpsを超えたが、あらゆる被写体・電波条件での常時6fps保証は未達。
暗所ノイズ・細かな模様・大きいJPEG、長時間、異なるブラウザ、撮影から表示までの遅延は別途検証が必要。

## UDP受信待ちの比較と短い停滞

同じ1,244,816-byte版（ELF SHA先頭`f4e6654dd627d8be`）で比較した。
ブラウザの準備完了を待ってから接続し、描画時計はブラウザのperformance.nowを使用した。

| 設定 | 秒 | 平均fps | 固定5秒の最低fps | ローリング5秒の最低fps | 最大描画間隔ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| fast_rx=0 | 60.393 | 6.656 | 6.016 | 5.8 | 677.4 |
| fast_rx=1 | 180.368 | 6.825 | 6.195 | 5.4 | 616.7 |

UDP受信の経過時間は1枚あたり21.09→17.29ms、peer main loopは56.41→53.04msに減った。
JPEG平均は45,178 / 45,091 bytesで近い。平均値の改善は小さく、短い停滞は解消していない。
固定した5秒の全区間が6fps以上でも、区切り位置をずらすと6fps未満になるため、
**常時6fps維持の要件は未達**と判定する。UART監視中の自発的リセットは検出されなかった。
後者のUSB受信1,746枚は全数有効JPEGで、送信APIエラー0、期限超過による送信途中破棄5枚。

この小バーストでも内部RAMの起動後最低残量616 bytesを記録したため、
DTLS受信バッファを16,384→4,096 bytesへ変更して再ビルドする。
DTLS MTUは固定ソースで1,500 bytes、ブラウザからの制御メッセージは小さく、
接続ごとに12KiBの内部RAMを節約する狙い。暗号化や送信画質は変更しない。
設定は他のTLS用途にも影響するので、大きいTLSレコードを必要とする将来の用途では再検討する。

再ビルド中に2断片/32msも120.726秒試したが、平均6.610fps、固定5秒最低3.430fps、
ローリング5秒最低3.4fps、最大描画間隔2,107msで採用しなかった。
PC側ではSDK全体をビルドしていたため、低下を送信設定だけの因果と断定しない。
最終確認ではビルドと測定を並行させない。

## どこに時間が掛かっているか

SDK既定の暗号ビルド・同程度のJPEGサイズで、送信1枚あたりの累積経過時間を比較した。

| 計測箇所 | AES-GCM ms/枚 | ChaCha20 ms/枚 |
| --- | ---: | ---: |
| peer main loop | 54.84 | 52.46 |
| send API | 88.28 | 82.14 |
| PSRAMからのフレーム取得 | 2.49 | 2.49 |
| Mbed TLS write | 77.82 | 72.30 |
| Mbed TLS read | 21.01 | 20.13 |
| UDP sendto | 18.11 | 16.22 |
| UDP送信再試行の待機 | 0.20 | 0.09 |

値は入れ子の経過時間で、加算してCPU占有率にはできない。
TLS writeにはUDP送信が含まれ、main loopにはTLS readや再送処理が含まれる。
UDPバッファ不足時の待ちより、TLS/SCTP送信処理が大きい。

`COMPILER_OPTIMIZATION_PERF`でもIDF 6.1の暗号ターゲットが後ろに`-Os`を追加していたため、
プロジェクトのCMakeでmbedtls / mbedx509 / tfpsacrypto / builtinの末尾へ`-O3`を追加した。
compile_commands.jsonで`-O2 -Os -O3`の順を確認済み。vendorとSDKソースは編集していない。
この変更単独の速度差はJPEGサイズや測定揺らぎから十分に分離できていない。

## 診断と再現

- `GET /perf`：実際のDTLS cipher、CPU周波数、FW ELF SHA先頭16桁、稼働時間、リセット理由、
  RSSI、内部RAMの現在・最低・最大連続空き、各処理の呼出数・合計us・bytes・最大us・失敗数。
  処理統計は新しいpeerを開く際にリセットする。RAM最低値とUSB統計は起動以来の値。
  TLSのWANT_READ / WANT_WRITEは通常動作なので失敗数から除く。
- `POST /rtc-config?cipher=2&chunks=4&burst_us=4000&fast_rx=0`：実験設定。
  cipherは0=自動、1=AES-GCM限定、2=ChaCha20優先。
  cipherは次の接続から、バーストとfast_rxは即時反映。
  chunksは1〜16、burst_usは1000〜64000、fast_rxは0/1。
  `&persist=1`を付けるとNVSへ保存し、起動時に復元する。フラッシュ保存は測定の外で行う。
- `/rtc-status`：設定値、保存済みフラグ、実送信・描画ACKのfpsと件数。
- `/status`：UVC受信、JPEG検査、USBエラー、`stream_restarts`。

`fast_rx=1`はDataChannel開通中だけ、既存UDP受信関数をnowaitで呼ぶ。
ICE/DTLSの接続処理中は元の待ち方を維持する。peerタスクの1ms yieldも維持する。
リンク時のwrapで既存関数を呼び出すため、IPv4/IPv6・ソケット利用数・受信処理を複製しない。

リポジトリ直下でviewer-serverを起動した状態から、Node.js（組込みWebSocket対応）で実行する。
Windowsの標準Chromeパスを使用し、ユーザーのブラウザとは別の専用プロファイルを作成する。

```powershell
$env:ESP_URL='http://192.168.137.145'
$env:SECONDS='180'
$env:RESULT_FILE='esp_webcam/.tools/benchmark.json'
node viewer-server/tools/benchmark-browser.mjs
```

5秒ごとのcanvas描画、描画ACK、送信、JPEGサイズ、ESP診断と全体集計を出力する。
新しい版はブラウザ内の単調時計で描画fpsを計算し、ローリング5秒の最低fpsと最大描画間隔も記録する。
headlessでのデコード・canvas描画を確認する検査であり、物理ディスプレイの走査は測定しない。
実際の設置環境でも同じviewerでの確認が必要。

UARTログはESP-IDFのPython環境から次で保存できる。DTR有効、物理タイムアウト固定、115200bps。
sdkconfigにあるWi-Fi認証情報は保存前に伏せる。

```powershell
python esp_webcam/tools/monitor_debugprobe.py --port COM3 --seconds 60 --output esp_webcam/.tools/uart.log
```

UVCが無応答のまま止まった実例があったため、3秒間フレームなし、または5秒間有効JPEGなしで
ストリーム停止・close・再openする監視を追加した。再open前の既存5秒待ちを維持する。
無応答を注入する復旧試験は未実施。USB抜き差しと電源起因の停止をすべて解消するものではない。
