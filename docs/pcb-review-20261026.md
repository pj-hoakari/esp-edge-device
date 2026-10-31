# PCBと映像配信構成の再レビュー

> 2026-10-31追記：本書は10月26日時点のレビュー記録である。現在の依頼は
> カラーHD（1280×720）・2fps以上の維持を目指すJPEGをWebRTC DataChannelで配信する方式に変更された。
> `../esp_webcam/README.md` に新しいファームウェアと検証手順を記載した。
> 本書の「一般的な映像トラック」要件と「ファームウェアなし」は当時の状態を指す。
> 基板に関する未解決事項は引き続き残っている。

2026-10-26、KiCad CLI 10.0.7で確認した。対象は16:17保存の基板と16:03保存の回路図。
カメラ・解像度・fpsは未定。受信側は、ユーザーの指定により一般的なWebRTC映像トラックとする。

**未配線は解消しているが、このまま製造して目的の動作を期待できる状態ではない。**
初回書き込み経路、電源・GNDのレイアウト、USBホスト給電を修正し、
カメラからブラウザまでの映像経路を開発ボードで実証してから製造へ進む。
設計ソースの変更は行っていない。

## CLIでは未配線と回路図不一致が0件

| 項目 | 前回 | 今回 |
| --- | ---: | ---: |
| ERC | 0 | 0 |
| 回路図と基板の不一致 | 0 | 0 |
| 未配線 | 11 | 0 |
| 穴径違反 | 12 | 12 |
| 接続不足の配線・ビア警告 | 6 | 0 |
| 基板端のシルク警告 | 2 | 2 |

DRCは終了コード5、ERCは0。DRCの14件は穴径違反12件とシルク警告2件。
ゾーン再充填を指定し、保存オプションは使用していない。
U1パッド41の12穴は径0.20mmで、プロジェクト下限0.30mmに抵触する。
製造条件とランド設計を照合して解決する。ルールを下げるだけでは解決としない。

回路図から出したネットリストでは、次を確認した。

- J1 D−→U1パッド13、D+→14。モジュール資料のGPIO19/20と一致する。
- U2は1=GND、2とタブ=+3.3V、3=+5V。AZ1117CHのHパッケージと一致する。
- R1=10kΩでENを3.3Vへ引き上げ、C3=1µFでENをGNDへ接続している。
- CC1とCC2は独立した5.1kΩ抵抗でGNDへ接続している。
- N8R8のOctal PSRAMが使うGPIO35〜37に外部回路を割り当てていない。

根拠：[モジュールデータシートv1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)、
[AZ1117C DS36676 Rev.5-2、2022-09](https://www.diodes.com/assets/Datasheets/AZ1117C.pdf)。

## 製造前に直す回路とレイアウト

### 初回書き込みと復旧の経路がない

U1のGPIO0（パッド27）、RXD0（36）、TXD0（37）は未接続。
ENにも操作用のボタン・テストパッドがなく、J2のUSBデータ端子はNCになっている。
通常のコネクタや治具を使う書き込み手順を組めない。
モジュール端子への直接配線や事前書き込みを別途行う場合を除き、初回書き込みが支障になる。

GPIO0、EN、TXD0、RXD0、GNDと必要な電源参照を治具用端子へ出す。
GPIO0をLowにした状態でリセットし、UARTから書き込み・ログ取得できる構成を推奨する。
GPIO0未接続だけで通常起動不能とは判断しない。内部プルアップによるSPI起動と、
手動でダウンロードモードへ入る経路の有無は別の問題である。

J1はホスト用USB-Aで、VBUSはJ2の5Vと直結している。
PCとのUSB書き込みに流用するためA-Aケーブルで接続すると、VBUS同士を接続することになる。
その方法を通常の書き込み手順にしない。
USBホストとUSB Serial/JTAGは内蔵PHYを共有するため、UARTを独立して出す方が開発しやすい。
根拠：[起動ストラップとENの公式ガイド](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)、
[USB PHYの共有制約](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#external-phy-configuration)。

### 電源・GNDと出力コンデンサを再配置する

全配線幅は0.20mm。基板は2層、厚さ1.6mmで、銅箔ベタは0個。
フットプリント内にあるゾーンはアンテナ用の禁止領域であり、GNDベタではない。
DRCの接続成立は、低インピーダンスの電源・GNDが確保できたことを意味しない。

3.3Vの外付けコンデンサはC4=22µFだけで、C2=100nFは5V側にある。
C4の3.3Vパッド(106.95,114.10)は、U1電源パッド(104.655,92.210)から
直線距離で約22.0mm、U2出力タブ(101.85,96.10)から約18.7mm離れている。
配線と戻り経路を含む実際のループはさらに長くなる。
U2の安定動作に使う出力容量と、U1直近のデカップリングをそれぞれ配置し、
短い電源・GND経路で接続する。C4の実効容量が3.3V・温度・公差を含め10µF以上かも確認する。

U2のドロップアウトは800mA時に最大1.3V。3.3V出力の電気特性は入力差1.5V以上で規定される。
USB入力5Vからケーブル・配線の電圧降下を差し引く必要があり、低入力時の余裕は小さい。
損失の概算は300mAで0.51W、500mAで0.85Wである。
出力タブに放熱用の広い3.3V銅箔がなく、連続運転の温度を保証できない。
**このICのタブはGNDではなくOUTなので、放熱のためGNDベタへ接続してはいけない。**
低ドロップアウト品または降圧回路の採用も、電流予算・周囲温度とともに再検討する。

根拠：[AZ1117C資料の容量条件・電気特性](https://www.diodes.com/assets/Datasheets/AZ1117C.pdf)、
[Espressif電源・リセット設計ガイド](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)。
SoC用の各電源ピンへの指示を、そのままモジュール内部への追加部品要求として扱ってはいない。

### USBの信号経路と給電を見直す

D−の配線長は約14.68mm、D+は約21.77mmで、差は約7.09mm。
両方とも表層でビアはないが、全区間で並走する差動配線になっておらず、
連続したGND参照面もない。層構成に基づく90Ω差動インピーダンスの設計根拠もない。
この長さ差だけでFull Speed USBが必ず動かないとは断定しないが、現配置を検証済みとは扱えない。
リターンパスを確保して並走させ、モジュール近くに直列抵抗を実装・調整できる場所を設ける。
コネクタ近くの低容量ESD保護も検討する。

J2 VBUS、J1 VBUS、U2入力は同じ+5Vネットに直結している。
カメラ向けの電流制限スイッチ、逆流対策、ポート電源制御・過電流検出はない。
短絡・突入電流でESP32側の電源まで落ちる懸念があり、復旧時にカメラだけを再給電できない。
ホスト給電の保護と制御を設計し、入力電源にはESP32とカメラの両方の電流を見込む。
CC抵抗だけでは供給元の許容電流を検出できないため、任意のUSB電源で使えるとはしない。

アンテナ用の両銅箔層の禁止領域は存在する。アンテナは矩形基板上に残るため、
ベース基板の切り欠き・基板端への配置も検討し、筐体込みで通信距離を測る。
根拠：[USB・モジュール配置の公式ガイド](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html)、
[公式USBホスト基板の給電回路](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-usb-otg/user_guide.html)。

## 一般的なWebRTC映像トラックにはコーデックの選定が先

ESP32-S3-WROOM-1-N8R8はFlash 8MB・Octal PSRAM 8MBを持ち、USBホストとWi-Fiを備える。
UVC受信とネットワーク送信を組み合わせる土台にはなる。ただしUSBはFull Speedで、High Speedではない。
[公式UVC例](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/uvc)は
S2/S3の受信目安を約0.5MB/sとし、Full Speedでよく使える例に320×240/30fpsと640×480/15fpsのMJPEGを挙げる。
これは、この基板や任意のカメラでそのfpsが出るという保証ではない。

一般的なWebRTCブラウザの基本映像コーデックはVP8とH.264で、MJPEGをそのまま
映像トラックとして扱える前提は置けない。[RFC 7742 §5](https://www.rfc-editor.org/rfc/rfc7742.html#section-5)。
公式の[local_jpeg_stream](https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/local_jpeg_stream)は
JPEGをDataChannelで送って専用ページで表示する方式なので、今回の受信要件を満たす根拠には使わない。

| 映像経路 | 今回の判断 |
| --- | --- |
| MJPEGカメラ→S3→MJPEG映像トラック | 通常のブラウザとの互換性を見込めず、このままでは採用不可 |
| MJPEG→S3で展開・H.264再圧縮→WebRTC | 低解像度・低fpsなら検証候補。ただし性能は未実証 |
| H.264出力カメラ→S3で再圧縮せずWebRTCへ中継 | S3を維持するなら優先して実証する候補。カメラ選定が厳しい |
| USBカメラ→P4などH.264ハードウェアを持つ処理系→WebRTC | 高い解像度・fpsを狙う場合の再設計候補 |

S3にもソフトウェアH.264エンコーダはある。
[esp_h264の公開測定（v1.4.1対応表、master参照）](https://github.com/espressif/esp-h264-component/blob/master/esp_h264/README.md)
ではS3R8で320×240/YUYVが11.23fps、320×192/I420が17.48fps、使用メモリは約1MB。
JPEG展開・USB受信・Wi-Fi・WebRTC暗号化を含む同時実行の性能ではない。
従ってS3でH.264が絶対に作れないとはいわないが、VGA以上の滑らかな配信を裏付ける数値でもない。
YUYVをそのままUSB受信する場合も、320×240×2バイト×15fpsで2.304MB/sとなり、
上記受信目安を超える。USBとエンコードの両方に制約がある。

新規実装では保守終了のusb_streamより、[usb_host_uvc 2.6.0](https://components.espressif.com/components/espressif/usb_host_uvc/versions/2.6.0/readme)
を起点に検証する。[現行API](https://github.com/espressif/esp-usb/blob/master/host/class/uvc/usb_host_uvc/include/usb/uvc_host.h)
にはH.264形式があるが、APIの存在だけでは特定カメラのFull Speed H.264出力は保証されない。
S3を維持するには、Full SpeedでH.264を出せるカメラを実物で確認し、
ブラウザと合うプロファイル、SPS/PPS、IDR周期、タイムスタンプ、RTPパケット化、再送・キーフレーム要求を実装する。
カメラのH.264表記がHigh Speed時に限る場合、この案には使えない。

[ESP32-P4](https://www.espressif.com/en/products/socs/esp32-p4)はUSB High SpeedとH.264ハードウェアを持つが、
無線接続には別のWi-Fiチップが必要であり、U1だけの置き換えにはならない。
今回はSoCや採用部品を変更していない。

## 発注の前に確認する順序

1. S3維持なら、まずH.264出力カメラ候補を開発ボードでFull Speed接続し、形式・解像度・fps・消費電流を実測する。
2. 通常のブラウザで映像トラックを受信し、フレーム落ち・遅延・メモリ残量を測る。受信だけと配信までの負荷を比較する。
3. 目標に届かなければ、低解像度のソフトウェア変換か、P4等への再設計を選ぶ。シグナリングとICE/STUN/TURNの要否も接続範囲に合わせて決める。
4. UART書き込み経路、電源保護、GND面、デカップリング、放熱を回路図から修正し、基板へ反映する。
5. 再度ERC・DRCと製造条件を照合し、試作で3.3Vの瞬時低下、U2温度、カメラ抜き差し、Wi-Fi同時動作を確認する。

production/ と production.zip は今回の最終基板より古い時刻に生成されている。
内容の完全な照合はしていないため、現行基板の製造データとして流用せず、設計確定後に再生成する。

## 検証方法と限界

CLIによるERC・DRC・XMLネットリスト、PCBファイルからの配線長・位置抽出、
CLI出力PDFの表示確認を実施した。メーカー資料と公式ソフトウェア資料は2026-10-26に参照。
GitHubのmaster/mainとlatest文書は変わるため、実装時にはリリースとコミットを固定する。
コネクタの全機械寸法、コンデンサのバイアス曲線、製造先の穴加工仕様、
熱解析・インピーダンス解析・実機動作は今回未検証。ファームウェアも存在しないためビルド・実行はしていない。

```sh
kicad-cli version
report_dir=$(mktemp -d /tmp/esp-webcam-review.XXXXXX)
kicad-cli sch erc --exit-code-violations --output "$report_dir/erc.rpt" esp_webcam.kicad_sch
kicad-cli pcb drc --refill-zones --schematic-parity --exit-code-violations --output "$report_dir/drc.rpt" esp_webcam.kicad_pcb
kicad-cli sch export netlist --format kicadxml --output "$report_dir/net.xml" esp_webcam.kicad_sch
```

今回の一時レポートは `/tmp/esp-webcam-rereview/` に保存した。
確認対象PCBのSHA-256: `72645f23bee5017379e707370f39c43a8e358dafda44b348f0339a163bf089e4`。
