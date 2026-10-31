# JLCPCB部品表とSMD化

2026-10-26にJLCPCB公式部品ページで型番、部品番号、パッケージを確認した。
全11部品、10種類。KiCadの実部品には `MPN`、`Manufacturer`、`LCSC`、
`JLCPCB Part #`、`SupplierURL` を設定した。
電源記号、PWR_FLAG、NCマークは購入部品に含めない。

| 部品 | 値・仕様 | メーカー型番 | JLCPCB番号 | 実装 |
| --- | --- | --- | --- | --- |
| C1 | 2.2µF、16V、X5R、±10% | CL10A225KO8NNNC | [C23630](https://jlcpcb.com/partdetail/CL10A225KO8NNNC/C23630) | 0603 |
| C2 | 100nF、50V、X7R、±10% | CL10B104KB8NNNC | [C1591](https://jlcpcb.com/partdetail/CL10B104KB8NNNC/C1591) | 0603 |
| C3 | 1µF、50V、X5R、±10% | CL10A105KB8NNNC | [C15849](https://jlcpcb.com/partdetail/C15849) | 0603 |
| C4 | 22µF、25V、X5R、±20% | CL21A226MAQNNNE | [C45783](https://jlcpcb.com/partdetail/CL21A226MAQNNNE/C45783) | 0805 |
| R1 | 10kΩ、±1%、100mW | 0603WAF1002T5E | [C25804](https://jlcpcb.com/partdetail/C25804) | 0603 |
| R2・R3 | 各5.1kΩ、±1%、100mW | 0603WAF5101T5E | [C23186](https://jlcpcb.com/partdetail/0603WAF5101T5E/C23186) | 0603 |
| J1 | USB-A、USB 2.0、1.5A | USB1046-GF-0190-L-B-A | [C6307429](https://jlcpcb.com/partdetail/7254738-USB1046_GF_0190_L_BA/C6307429) | SMD端子・固定脚はTHT |
| J2 | USB-C、CC付き、5V電源入力 | TYPE-C-31-M-12 | [C165948](https://jlcpcb.com/partdetail/C165948) | SMD端子・固定脚はTHT |
| U1 | Flash 8MB、Octal PSRAM 8MB | ESP32-S3-WROOM-1-N8R8 | [C2913201](https://jlcpcb.com/partdetail/3198299-ESP32_S3_WROOM_1N8R8/C2913201) | SMDモジュール |
| U2 | 3.3V固定、800mA級のLDO | AZ1117CH-3.3TRG1 | [C92102](https://jlcpcb.com/partdetail/AZ1117CH-3.3TRG1/C92102) | SOT-223 |

コンデンサ・抵抗はSamsung Electro-MechanicsとUNI-ROYAL製。
J1はGlobal Connector Technology、J2はKorean Hroparts Elec製。
全てKiCad標準フットプリントを割り当てた。コネクタの固定脚・位置決め穴、
ESP32モジュールの放熱用穴は、信号端子をSMD化しても残る。

## 発注ファイル

`exports/BOM_JLCPCB.csv` に同一品をまとめて出力した。JLCPCB番号に加えメーカー型番も照合できる。
U1は確認時点でJLCPCBのStandard Only部品。Basicのみでの実装にはならない。
在庫数・価格・実装条件は発注時に再確認する。
基板の配置・配線が未完了のため、製造用CPLやGerberは出力していない。

## 初回SMD化時の基板への反映（履歴）

- C1〜C3を0603、C4を0805の無極性セラミックへ交換した。
- U2をSOT-223、J1をUSB1046へ交換し、回路図と基板のピン・ネットを一致させた。
- R1〜R3、U1、J2はパッケージを維持し、型番・調達情報を揃えた。
- パッド位置とU2のピン配列が変わる6部品に関連する旧配線・ビア70個を除去した。
  影響ネットは+5V、+3.3V、GND、USB D+/D−、EN。
- 大きさが変わるJ1・U2は基板外へ仮置きした。その他の配置、基板外形は維持した。
- 交換前の回路図・基板・設定は `esp_webcam-backups/pre-jlc-smd-20261026.zip` に退避した。

ERCは0件、回路図と基板の等価性問題は0件。
DRCには既存のモジュール内穴径0.2mmとルール下限0.3mmの不一致12件が残る。
未配線は33件。基板内への配置、再配線、アンテナ禁止領域、放熱、入力容量の実効値と
実機動作の確認が必要。部品表の確定を製造可能な基板の完成とは扱わない。

U1のメモリ構成と端子制約は
[Espressifデータシート v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
で確認した。Octal PSRAMで使用するGPIO35〜37は外部用途へ割り当てない。

## 安価なレギュレータへの変更

秋月で単品を5個・10個購入する前提で、U2をAZ1117CH-3.3TRG1へ変更した。
[秋月116155](https://akizukidenshi.com/catalog/g/g116155/) は2026-10-26確認時点で税込30円/個。
5個150円、10個300円（送料別）。数量割引を仮定した価格ではない。
メーカー推奨回路と容量余裕を考慮し、C4を22µFへ変更した。

ユーザーが進めた配線を維持し、ピンの役割が変わるU2のパッド周辺に接触する
電源配線・ビア15個だけを外した。USB信号・CCを含む他の配線は維持した。
直前の基板は `esp_webcam-backups/pre-cheap-ldo-20261026.zip` に退避。
変更後はERC0件、回路図と基板の等価性問題0件。未配線11件、
既存の穴径・シルク関連14件と局所配線の切り離しによるダングリング警告6件が残る。
U2周辺は新しいピン配列で再配線する必要がある。
