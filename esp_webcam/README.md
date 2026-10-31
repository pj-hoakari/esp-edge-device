# ESP32-S3 UVCカメラ / MJPEG確認ファームウェア

## 開発中の6fps WebRTC改善版

最新目標は1280×720カラー・6fps以上。改善版はHTTPS 443でWebRTC DataChannel、
HTTP 80/81でカメラ診断とMJPEGを同時に提供する。起動元は`mjpeg_main.c`で、
同じカメラの最新JPEGを独立したシーケンスで購読するため、MJPEGとWebRTCがフレームを奪い合わない。
WebRTCは`main.c`の`webrtc_start()`から起動する。診断用MJPEGを同時再生するとWi-Fi帯域を使うため、
WebRTCが開通するとMJPEG送信は自動で休止して帯域を優先する。

- JPEG内部のマーカーと寸法・3色成分を検査し、EOIより後ろのUSB/DMA整列データは長さを問わず無視する。
  返すJPEGはEOIまでとする。JPEG本体の欠損やセグメント長異常は修復扱いで通さない。
  破棄理由は`/status`の`invalid_*`カウンタで確認できる。
- 同一CコードをMSVCでPC用DLLにビルドし、実JPEG・0〜16バイト末尾・破棄理由・APP内の偽EOI・切断・寸法違い・長さ異常でテスト済み。
- 公式JPEG例と同様にESP側から`jpeg` DataChannelを作る。ブラウザは`control`でSDPを用意し、
  `ondatachannel`で映像チャンネルを受け取る。unordered / maxRetransmits=0（Kconfigで0〜3）。
- クライアントはデコードしたJPEGを64×36へ縮小して前フレームとの平均RGB差（0〜255）を計算し、
  `ACK:<frame ID>:<motion>`で返す。ESPは静止時に`Idle FPS`（既定2fps）、動き検出後は
  `Send target FPS`（既定6fps）へ自動で切り替える。動きスコアを送らない旧クライアントは従来どおり6fps。
- 解像度は**1280×720（HD）固定**。UVC設定で解像度を落とす項目は持たない。
  帯域はフレームレート（目標6fps、静止時2fps）と同一フレーム抑制で削減する。
- AES-128-GCM優先、WebRTCタスクをCPU1へ配置、SCTP送信キャッシュ128KiB、
  HTTP/HTTPS/ICE併用のためソケット上限20。再圧縮はまだ追加していない。
- 直接LAN接続では、HTTPシグナリング送信元IPv4で`.local`ホスト候補を展開する。
  ブラウザのプライバシー設定は変更しない。リバースプロキシやLAN外接続はこの処理の対象外。

6fpsを達成したという実機結果はまだない。書き込み後にUVC有効JPEG率・WebRTC受信fps・
静止時の実効fps・JPEGデコードを測定し、必要ならカメラ側のMJPEG品質制御を含めて次の改善を選ぶ。

改善版はESP-IDF v6.1でビルド・リンク成功（app 1,185,168 bytes）。
JPEG解析実コードのPCテスト（末尾0〜16バイト・破棄理由を含む）と
DataChannel受信・断片復元・動きスコアのテスト5件が成功。
libwebrtc M106ベースの`@roamhq/wrtc` 0.10.0による検証クライアントも用意し、起動を確認した。
現時点ではPCにESPのCOMポートが出ていないため、改善版の書き込みと実機測定は未実施。

## 現在の動作：HTTP MJPEGとカメラ診断

2026-10-31、カメラ受信の切り分けのため既定ビルドを`main/mjpeg_main.c`へ切り替えた。
WebRTCの送信処理は`main/main.c`、受信画面は`main/web/index.html`・`receiver.js`にあり、
同じビルドからHTTPS側で起動する。Wi-Fiのローカル設定、既定720p MJPEG・10fps受信、
VID:PID=349C:3307は引き継ぐ（fpsはKconfigで変更可能、解像度はHD固定）。

- 画面：`http://<ESPのIP>/`（**httpsではなくhttp**）
- カメラ診断JSON：`http://<ESPのIP>/status`
- USB・UVCの直近ログ：`http://<ESPのIP>/usb-log`（修正版。Wi-Fiログは含めない）
- MJPEG映像：`http://<ESPのIP>:81/stream`

`/status`はUSB機器数、UVC検出回数、オープン結果、JPEG受信件数・寸法・サイズ・受信fps・
送信目標fps・Idle FPS・動きしきい値、不正JPEGの理由別カウンタ、バッファ超過・転送エラーを返す。
`/rtc-status`はDataChannel接続、送信数、重複排除・stale破棄・ACK統計に加え、
`effective_fps`（現在の実効送信fps）、`client_motion`（直近の動きスコア）、`idle_fps`を返す。
USB機器数とUVC検出は別の段階である。
UVC検出回数は起動後の累計なので、現在接続されているかはUSB機器数・状態・最終受信時刻も見る。
MJPEG用と画面・診断用にHTTPサーバーを分け、配信中も診断を取得できる。
映像閲覧は1台を想定。カメラがなくても診断画面は開き、映像は待機する。
カメラを途中で外すと最後の画像が残ることがあるが、状態欄は受信停止を示す。
WebRTCのICE/DTLS成功後にSCTP/DataChannelが開かない問題は、今回のMJPEG確認とは分けて扱う。

この確認版はESP-IDF v6.1でビルド・リンク成功（app 921,296 bytes）、
COM11のESP32-S3へ書き込み、bootloader・partition table・appのハッシュ照合に成功した。
ブラウザ用JavaScriptの構文確認も実施済み。カメラの認識・実受信結果は通常起動後に確認する。

通常起動後、Wi-Fi接続と`/status`の応答を確認した。複数回の確認でUSB機器数0、
UVC検出0、JPEG受信0、オープン結果`ESP_ERR_NOT_FOUND`だった。
現在はUSB機器の列挙が成立しておらず、カメラの給電・実際の接続先・データ線の確認が必要。
カメラ形式の不一致やJPEG転送帯域が原因と確定した状態ではない。
実機の接続先はユーザー確認により自作基板のJ1（USB-A）。KiCad CLI 10.0.8で
回路図からXMLネットリストを再出力し、現行PCBのパッドとも照合した。
J1.1=J2のVBUS由来の+5V、J1.4=GND、J1.2=D−→U1.13（GPIO19）、
J1.3=D+→U1.14（GPIO20）。ソフトウェアの内蔵USB PHY割り当てと一致する。
設計ファイル上の接続確認であり、実装品の導通・VBUS電圧・はんだ状態を確認したものではない。
モジュールの端子根拠は[公式データシートv1.8、表3-1](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)。
J1フットプリントの端子配置は[GCT USB1046図面（Mouser配布）](https://www.mouser.com/datasheet/2/837/USB1046-1948751.pdf)を参照した。
ユーザーの実測ではカメラ接続時のJ1 VBUS-GND間は4.97Vだった。
ソフトウェア設定を再確認すると、USB制御転送上限が既定の256 bytesのままだった。
採用ドライバの`host/usb/src/enum.c`はconfiguration descriptorがこの上限を超えると
`ESP_ERR_INVALID_SIZE`で列挙を拒否する。公式UVC basic_uvc_stream例に合わせ、上限を
4096 bytes、ハードウェアFIFOをBias INへ修正した。
根拠：[固定コミットの公式UVC既定設定](https://github.com/espressif/esp-usb/blob/2e2032bb861f40b3d9e1e513e03856c8daa337a1/host/class/uvc/usb_host_uvc/examples/basic_uvc_stream/sdkconfig.defaults)。
対象カメラのdescriptor全長と、この設定が認識失敗の原因だったかは修正版での再確認が必要。
修正版の`/status`には`usb_control_max`と`usb_fifo_bias`を出し、実機へ反映された設定も確認できる。
4096-byte / Bias IN修正版もビルド・リンク、COM11への書き込みと全領域のハッシュ照合に成功した
（app 922,080 bytes）。その後の通常起動でUSB機器1台・UVC検出1件、
ストリーム開始`ESP_OK`と1280×720 JPEG受信を確認した。MJPEGストリームから受け取った
3枚をPCでデコードし、1280×720・yuvj420pとして正常に展開できた。
ユーザーもブラウザ上の映像表示を確認した。

10秒間の測定依頼に対し、診断APIのフレームカウンタ差分を実測10.137秒で計測した。
USB受信101枚（9.964fps）、有効JPEG25枚（2.466fps）、無効判定で破棄76枚。
期間内のUSB転送エラー0、バッファ超過0。この値はESPでの受信・検査カウンタであり、
ブラウザの描画イベントを直接計測した値ではない。
無効判定の多くは、EOI後の整列パディングを3バイトまでしか許さなかった検査によるものと推定した。
USB/DMAが16バイト境界へ整列すると末尾は0〜15バイトになり、3バイトまでなら4/16だけが通るため、
有効率がほぼ25%になる説明がつく。現在のソースはEOI後を長さ無制限で無視するよう修正済みだが、
実機での有効率改善は未測定。

書き込み時はカメラを外し、IO0をGNDにしてENリセット後にPCのUSBへ接続する。
書き込み終了後は**IO0のGND接続を外し、通常起動する**。USBホスト開始後は同じ内蔵PHYの
USB Serial/JTAGポートがPCから見えなくなるため、再書き込みには再び書き込みモードへ入る必要がある。
動作中の詳細ログはUART0で確認する。電源・GND・カメラVBUSの注意点は以下の基板条件を参照。

以下は、保持しているWebRTC版の構成・開発記録である。現在の有効なエントリーポイントとURLは上記を使用する。

## WebRTC版の構成と開発記録

ESP32-S3-WROOM-1-N8R8（Flash 8MB / Octal PSRAM 8MB）用。
USB UVCカメラのカラーMJPEGからJPEGを取り出し、同一LANのブラウザ1台へ送信する。
目標は **1280×720カラー・6fps以上の維持**。6fpsは上限ではなく、余裕があれば高速化する。
カメラのJPEGを再圧縮せず送るため、カラーはそのまま保持する。
対象カメラはWindowsで「HD video」と認識された **349C:3307**。
PCでFull Speed・720p MJPEGのdescriptorを確認済み。ESP上での動作と6fpsの達成は未確認。

## 構成

```text
USB UVC MJPEG camera → USB Full Speed host → latest JPEG in PSRAM
 → JPG1分割パケット → SCTP / DTLS / ICE → RTCDataChannel
 → JPEG復元 → ブラウザJPEGデコード → canvas
```

映像トラック、H.264エンコード、音声は使わない。ESP内蔵HTTPSサーバーが
画面とSDPシグナリングを提供する。ブラウザがofferを作り、ESPがanswerを返す。
ICE収集完了後のSDPを交換し、trickle ICEは使わない。
STUN/TURNは未設定で、同一LAN・端末間通信が可能なWi-Fiが前提。
インターネット越しの接続、認証付きの複数ユーザーサービスは対象外。
HTTPSシグナリングのサーバー証明書とWebRTC DTLS用証明書は別物。

## カメラ・基板の成立条件

- **Full Speed（12 Mbit/s）で1280×720 MJPEGを広告・送信できるUVCカメラ**が必要。
  USB 2.0/HD対応の表示だけでは判断できない。High Speed専用720pモードは利用できない。
  対象カメラは1280×720の10fps/15fpsを広告する。
- 解像度は**1280×720（HD）固定**で、低解像度へ落とす設定は持たない。
  初期設定は**カメラ受信10fps・Wi-Fi送信6fps**。
  VID/PIDも349C:3307を初期値とし、別カメラではmenuconfigで変更する（0はワイルドカード）。
  `UVC capture FPS`はカメラが実際に広告する値（10または15）へ合わせる。
  採用ドライバは離散frame intervalの完全一致を要求するため、7や8など広告にない値は開けない。
  送信上限は`Send target FPS`（既定6）、静止時の下限は`Idle FPS`（既定2）で決める。
  クライアントが動きを検出すると6fps、静止が2秒続くと2fpsへ戻る。
  解像度や形式を自動で下げず、720p MJPEGを開けなければログに失敗を出す。
- JPEGは1枚256KiBまで。例えば100KiB×6fpsは約615kB/s、JPEGだけで約4.9Mbit/s。
  200KiB×6fpsは約1.23MB/s、約9.8Mbit/s。USB/UVC、SCTP/DTLS、Wi-Fiのオーバーヘッドが加わる。
  256KiBは安全なメモリ上限で、6fpsの性能保証ではない。細かい模様・暗所ノイズでサイズが増える。
  対象カメラの720pのframe descriptorは最大1,843,200 bytesを申告するが、これは実際の圧縮JPEGサイズの測定値ではない。
  10fpsのUSB入力では50KiB/枚でも約512kB/sになる。出力を6fpsへ間引いても、USB入力帯域は減らない。
  このため、descriptorの対応だけではS3での成立を確定できない。
- JPEG品質のカメラ制御は未実装。大きすぎる場合はカメラ側設定や別カメラで調整する。
  YUY2/RGBをUSB受信してESP上でJPEG圧縮する経路は実装していない。
- GPIO19=D−、GPIO20=D+。内蔵USB PHYをホストが使うため、**書き込み・ログはUART0**を使う。
  3.3VロジックのUSB-UARTをTXD0/RXD0/GNDに接続し、GPIO0とENでダウンロードモードに入る。
  自作基板にはこれらの操作・端子経路が不足するため、先に開発ボードまたは治具を用意する。
- カメラVBUSには5V電源が必要。既存基板のJ1は5V直結で電流制限・逆流保護が未整備。
  電源容量、3.3V低下、レギュレータ発熱、カメラ突入電流を確認する。
  J1にA-AケーブルでPCを接続する書き込みは行わない。
  回路の残課題は[基板レビュー](../docs/pcb-review-20261026.md)を参照。

## 固定した開発環境

2026-10-31に取得した公式ソースを使用する。

| 依存 | 版 / コミット |
| --- | --- |
| ESP-IDF | v6.1.0 / `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Xtensa GCC | `esp-15.2.0_20251204` |
| esp_peer | 1.5.7 / esp-webrtc-solution `38b175077967109e764d434d680e641d9e87b23c` |
| usb_host_uvc | 2.6.0 / esp-usb `2e2032bb861f40b3d9e1e513e03856c8daa337a1` |
| usb | 1.6.0 / 同じesp-usbチェックアウト |
| esp_libsrtp | 1.0.0、`dependencies.lock`で固定 |

`vendor/` の公式ソース・プリビルドライブラリとライセンスをそのまま参照する。
空白を含むパス用のリンク引数補正はプロジェクト側の`CMakeLists.txt`で行う。
`vendor/`の2リポジトリはルートの設計リポジトリでsubmoduleとして固定する。

初回取得は設計リポジトリのルートで以下を実行する。

```powershell
git clone --recursive <リポジトリURL> esp_webcam
# 既存チェックアウトの場合
git submodule update --init --recursive
```

`.tools/esp-idf`はsubmoduleにせず、通常のIDFインストールまたはローカル取得を使う。

### リポジトリ管理

- ルートの設計リポジトリにはKiCad設計、`docs/`、`exports/`、ファームウェアをコミットする。
- `vendor/`の2リポジトリはsubmodule。親リポジトリが記録するのはコミットSHAのみなので、
  依存を更新するときはsubmodule側でcheckoutし、親で`git add esp_webcam/vendor/...`する。
- `.tools/`（ESP-IDFローカル取得）、`build/`、`managed_components/`、`sdkconfig`、`certs/`、
  KiCadのバックアップ・自動保存・ロックファイルは追跡しない。
- Wi-Fi認証情報は`sdkconfig`にのみ置き、共有しない。

## ビルドと起動

1. ESP-IDF v6.1のPowerShell環境を開く。ターゲットは`esp32s3`。
2. このプロジェクトパスには空白・括弧がある。Windowsでは空いているドライブ文字で
   プロジェクトを割り当てるとツールのパス問題を避けやすい。
   例（Q:が空いていることを確認する）：

   ```powershell
   subst Q: 'C:\Users\haruj\ownCloud (2)\Personal\ProjectWS3\esp_webcam\esp_webcam'
   Set-Location Q:\
   $env:PYTHONUTF8='1'
   $env:KCONFIG_REPORT_VERBOSITY='quiet'
   ```

3. IDFのPython環境でローカルHTTPS証明書を生成する。
   `cryptography`はIDFのPython依存に含まれる。秘密鍵は各環境で生成し、既存ファイルは上書きしない。

   ```powershell
   python tools/make_cert.py
   idf.py set-target esp32s3
   idf.py menuconfig
   ```

4. `ESP webcam`でSSID・パスワード・カメラ対応fps・送信目標fps・Idle FPSを入力する。
   認証情報はローカルの` sdkconfig `とビルド成果物に含まれるため、これらを共有しない。
   ソース・既定値・実行ログには認証情報を記載しない。
5. ビルドしてUART経由で書き込む。COM番号は実際のポートへ置き換える。

   ```powershell
   idf.py build
   idf.py -p COM5 flash monitor
   ```

6. 給電・USBホスト配線を確認した上で、対象カメラをPCから外してESP側のホストポートへ接続する。
   ログの`Open https://<IP>/`を同一LANのChrome/Edgeで開く。
   ローカル生成の自己署名証明書なので、対象機器のIPであることを確認して例外を許可する。
   「接続」を押すとJPEGを表示する。再接続は「切断」→「接続」。同時接続は1台で、新しいofferが前の接続を置き換える。
7. 作業後はQ:以外へ移動して`subst Q: /D`で割り当てを解除できる。

今回のビルド用に`.tools/esp-idf`も取得した。通常はインストール済みIDF v6.1を使ってよい。
ローカル確認補助`.tools/build-local.ps1`はこのPCの既存ツール配置向けで、配布用手順ではない。
このPCでは上記のQ:割り当て後、プロジェクト直下で
`& ./.tools/build-local.ps1 menuconfig`、`& ./.tools/build-local.ps1 build`、
`& ./.tools/build-local.ps1 -p COM5 flash monitor`でも操作できる。
`certs/`、`sdkconfig`、`build/`、`.tools/`はGit除外対象。
ファームウェア自体には接続認証を実装していないため、利用対象は信頼できるLAN内とする。

## 分割送信と遅延制御

- JPEGの最大長256KiB、1メッセージは最大12,020 bytes（ヘッダー20＋JPEG断片12,000）。
- ヘッダーは`JPG1`、uint32 LEのframe ID、JPEG全長、断片offset、ESP送信開始時刻ms。
  端末時計は同期していないため、この時刻をブラウザとの片道遅延として表示しない。
- ESPが作る`jpeg`はunordered / maxRetransmits=0（既定）。`DataChannel retransmits`で0〜3に変更できる。
  受信側は断片の順序入れ替え・重複を許容し、不正サイズ・未完成フレームを破棄する。
- UVCコールバックは最新1枚をPSRAMへコピーして直ちにバッファを返す。
  送信中もカメラ受信を止めず、古い待機フレームを最新フレームで置き換える。
- 送信は`Send target FPS`（既定6fps）でペーシングし、ACKは鮮度の統計にだけ使う。
  ACK待ちで送信を止めないため、応答の遅いビューアでも6fpsを維持し、古いフレームは捨てる。
  1枚の送信開始から実効周期の1.5倍を超えた残り断片は破棄し、次の最新フレームへ移る。
  送信バッファ満杯時はpeerのイベント処理を続けながらリトライする。
- クライアントはJPEGをデコードした後、64×36の縮小画像について前フレームとの平均RGB差を計算し、
  `ACK:<frame ID>:<0〜255>`を返す。しきい値（既定6）以上なら`Motion hold`（既定2秒）の間
  6fpsを維持し、その後は`Idle FPS`（既定2fps）へ下がる。ACKが3秒途絶えた場合も6fpsへ戻す。
  これにより、静止した監視映像では伝送を約1/3に削減し、動きが出たときだけ帯域を使う。
  さらに静止画が同一なら同一フレーム抑制が働き、実送信はKeepalive相当の約1fpsまで下がる。
- 同一JPEG（CRC32とサイズが一致）は再送しない。`Keepalive`（既定1000ms）経過時だけ
  同じフレームを1回送り、受信側の復帰を助ける。0で重複抑制を無効化できる。
  キャプチャから実効周期の1.5倍を超えた古いJPEGは送信しない。
- ブラウザは受信JPEG（1280×720）をcanvasへ描画する。追加のデコードライブラリは不要。
- ESPはACKを参考統計として扱い、2秒以上応答がなければ警告を出すが、送信は止めない。
- UVCフレーム3枚＋最新1枚＋送信用1枚で約1.25MiB、ほかにUSB URB・TLS・SCTP・Wi-Fi等が必要。
  起動時に4MiB以上のPSRAMを確認する。最大JPEGの全バッファはPSRAMへ明示的に配置する。

## 検証手順

2026-10-31のPC上での確認結果：ESP-IDF v6.1 / ESP32-S3向けビルド・リンク成功、
`build/esp_webcam.bin`生成（1,172,576 bytes）。空のSSID設定でもUSB/UVC・WebRTCの実装が
リンクされていることをELFのシンボルでも確認した。受信側テスト4件成功。
カメラのFull Speed・対応モードはUSB descriptorを読み取り確認済み。
書き込み、ESP上のUVC受信、ブラウザとのWebRTC接続、カラー映像、実測fpsは未検証。
依存側CMakeの非推奨・private include警告は残るが、コンパイル・リンクのエラーはない。

同日追記：接続されたESP32-S3 rev.0.2（PSRAM 8MB、Flash 16MB）へ、COM11の
USB Serial/JTAG経由で上記ファームウェアを書き込み、bootloader・partition table・appの
書き込み後ハッシュ照合に成功した。Flash設定は8MBのままで、実容量の範囲内を使用する。
Wi-Fi設定は空のため、配信を開始するにはローカルで設定して再ビルド・再書き込みが必要。
この記録は接続された開発機の情報であり、KiCad上のN8R8モジュールの仕様変更ではない。
実行時ログのUART確認、UVC受信とWebRTC接続・実測fpsは引き続き未確認。

さらに同日、指定された2.4GHz Wi-Fi設定をローカルsdkconfigに反映して再ビルドし、
COM11へ再書き込み・全領域のハッシュ照合に成功した（app 1,172,608 bytes）。
PCのホットスポットが有効、2.4GHz、SSIDとパスワードがローカル設定と一致することも確認。
接続・IP取得とHTTPS応答は現時点では未確認。認証情報そのものは本書へ記載しない。

その後、ユーザーがIO0のGND接続を解除して通常起動し、対象MACのWi-Fi接続を確認した。
取得IP `192.168.137.139`で`/`と`/receiver.js`がHTTP 200を返し、配信画面の内容も一致した。
このIPは今回のDHCP割り当てであり固定ではない。GPIO0をLowに保持したまま起動すると
書き込みモードになるため、通常運用ではLowに保持しない。
UVCカメラ受信・DataChannel接続・映像表示・実測fpsは引き続き未検証。

Windowsでのカメラdescriptor確認結果は[カメラ調査メモ](../docs/camera-349c-3307.md)を参照。
再確認は`python tools/probe_windows_camera.py`。USBハブ情報を読めるWindows端末で実行する。
映像の撮影・取得、ドライバ変更、USBポートのリセットは行わない。

ブラウザの受信処理はPC単体で検証できる。

```powershell
node --test tests/receiver.test.mjs
```

実機で次を順に記録する。

1. 起動ログでFlash・PSRAM、カメラのUSB descriptorと`<width>x<height> MJPEG capture started`を確認。
   カメラ未接続・非対応なら5秒間隔で再試行する。1280×720がFull Speedで出ない場合は
   解像度を落とさず、カメラを再選定する。
2. 色のある被写体でカラー、1280×720の寸法、向き、破損がないことを確認する。
3. 明所・暗所・動き・細かい模様を含めて各10分、平均fps、JPEG bytes/枚、Mbit/s、欠落を記録する。
   動きがある間は6fps以上、静止時は`Idle FPS`（既定2）へ落ちること、
   `/rtc-status`の`effective_fps`と`client_motion`が一致することを確認する。
   256KiB上限超過なし、リセットなし。性能に余裕があれば`Send target FPS`を上げて再測定する。
4. 画面に時計を映して撮影し、入力から表示までの遅延を外部から測る。
   ブラウザの「分割受信ms」は断片の組立時間のみで、エンドツーエンド遅延ではない。
   静止から動きへ切り替わったときの6fps復帰までの時間も記録する。
5. Wi-Fi電波低下、ブラウザ切断・再接続、カメラ抜き差しで、古い映像の蓄積・クラッシュがないか確認。
6. ESPの3.3V瞬時低下、レギュレータ温度、5V電流とカメラ突入電流を測定する。

接続不能時はAPの端末分離・VLAN・ファイアウォール・同一LANを確認する。
ESPのICE実装でブラウザmDNS候補が解決できない場合は、公式例に記載の
WebRTC mDNS候補設定を検証用ブラウザで確認する。LAN外はTURN/STUN等の追加設計が必要。

## 根拠資料（2026-10-31参照）

- [esp_peer 1.5.7 API・設定](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/components/esp_peer)：ICE/DTLS/SCTP、DataChannelイベント、WOULD_BLOCK。
- [UVC 2.6.0 API](https://github.com/espressif/esp-usb/blob/2e2032bb861f40b3d9e1e513e03856c8daa337a1/host/class/uvc/usb_host_uvc/include/usb/uvc_host.h)：MJPEG設定、フレーム所有権、切断イベント。
- [公式JPEG DataChannel例](https://github.com/espressif/esp-webrtc-solution/tree/38b175077967109e764d434d680e641d9e87b23c/solutions/local_jpeg_stream)：JPEG分割送信・ブラウザ表示方式の参考。DVP用の例なのでUVC動作の保証には用いない。
- [ESP32-S3-WROOM-1/1Uデータシート](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)：既存基板レビューで参照したv1.8のN8R8構成とGPIO19/20。
- [USB Host / PHY共有制約](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#external-phy-configuration)。

基板・回路図は変更していないためERC/DRCは今回のソフトウェア変更では実行しない。
