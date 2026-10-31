# USB-C 5V電源入力

2026-10-26時点の設計。

## 部品と接続

- J2: Korean Hroparts Elec TYPE-C-31-M-12、JLCPCB/LCSC番号 `C165948`。
- シンボル: `Connector:USB_C_Receptacle_USB2.0_16P`。
- フットプリント: `Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12`。
- VBUS（A4・A9・B4・B9）を既存の+5Vネットへ接続する。
- GND（A1・A12・B1・B12）とシールドをGNDへ接続する。
- CC1（A5）をR2、CC2（B5）をR3経由でそれぞれGNDへ接続する。
  抵抗は各5.1kΩ、1%、0603。CC1とCC2を短絡・共用しない。
- 電源専用のためD+/D−・SBU1/SBU2にはNCマークを付ける。

Rd抵抗で5Vを受電する構成。USB PDの電圧交渉や供給可能電流の検出は行わない。
使用する電源が供給できる電流と、ESP32・カメラを含む電流予算は別途確認する。

## 反映・検証状況

回路図にJ2・R2・R3を追加し、ERCは0件。既存部品のネット接続は変更していない。
基板にも標準フットプリントとネットを反映した。既存部品の配置・配線を維持し、
新しい3部品は基板外に仮置きしてある。基板内への配置、外形の検討、配線は未実施。

その後の全BOMのJLCPCB化・SMD化でレギュレータやコンデンサも交換した。
現在の部品表・基板の状態は `jlc-bom.md`、電源ICの調査結果は `regulator-review.md` を参照する。
製造用データを作る前に配置・配線と再チェックが必要。

## 選定根拠

- [JLCPCB C165948](https://jlcpcb.com/partdetail/C165948): 型番と実装サービスへの登録を確認。
  在庫・価格・実装条件は発注時に再確認する。
- [HROの部品図面（LCSC配布）](https://www.lcsc.com/datasheet/C165948.pdf):
  2020-12-08の図面でCC1=A5、CC2=B5と電源端子、ランド配置を確認。
- [秋月 USB-C給電参考資料](https://akizukidenshi.com/goodsaffix/powered_by_type-c.pdf):
  Ver.2025-12-16。CC1・CC2をそれぞれ5.1kΩでGNDへ接続する受電構成を確認。

当初候補の秋月116895（A295-CTRPB-1）はCC端子がなく、USB-C同士での給電用途に
適さないため採用しない。この部品の専用シンボル・フットプリントはプロジェクトに残していない。
