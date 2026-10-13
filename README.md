# learningremote

Raspberry Pi Zero 2 W と pigpio を使う C++ の赤外線学習リモコンです。受信した信号をマーク・スペースの時間列として保存し、登録名で呼び出して送信します。

## 必要なもの

- Raspberry Pi Zero 2 W と Raspberry Pi OS
- pigpio の C/C++ 開発ファイル（`pigpio.h` と `libpigpio`）
- 赤外線受信モジュール（例: 38 kHz 復調型）
- 赤外線 LED、電流制限抵抗、トランジスタ等のドライバ回路

赤外線 LED は GPIO に直接接続せず、トランジスタ等で駆動してください。受信モジュールの電源・出力電圧と GPIO の 3.3 V 制約を確認してください。GPIO 番号は物理ピン番号ではなく BCM 番号です。学習入力は BCM GPIO 17、送信出力は BCM GPIO 13・19・26 に固定しています。送信時は3つの出力が同じタイミングで動作します。

## ビルド

pigpio を導入し、pigpio daemon を起動してからビルドします。

```sh
sudo apt update
sudo apt install pigpio libpigpio-dev libsqlite3-dev g++ make
sudo systemctl enable --now pigpiod
make
```

`pigpio` パッケージ名や daemon の起動方法は Raspberry Pi OS の版によって異なる場合があります。`pigpiod` が起動できない環境では、使用中の OS で pigpio が利用可能か確認してください。

## 使い方

```sh
sudo ./learningremote
```

起動後、プロンプトでコマンドを入力します。`quit` で終了します。`delete` は指定信号を削除し、`allclear` は指定したデータベースの学習内容をすべて消去します。

```text
learningremote> learn 電源
learningremote> send 電源
learningremote> send 電源 --repeat 3 --gap 150
learningremote> delete 電源
learningremote> list
learningremote> allclear
learningremote> quit
```

データはSQLiteデータベースとして実行ディレクトリの `codes.db` に保存します。別ファイルを使う場合は `--db FILE` を付けます。学習は最大 30 秒待ちます。時間を変えるには `--timeout SEC` を指定します。
`allclear --db livingroom.txt` のように指定すると、そのファイルの学習内容を消去できます。
従来のテキスト形式の `codes.txt` が残っている場合は、初回アクセス時に内容を `codes.db` へ移行し、元ファイルを `codes.txt.legacy` に退避します。

```sh
sudo ./learningremote
```

起動後の入力例: `learn エアコン冷房 --db livingroom.txt --timeout 60`、`send エアコン冷房 --db livingroom.txt`。

## 接続例

- 受信モジュールの OUT → BCM GPIO 17、VCC → モジュール仕様に合う電源、GND → GND
- 送信 LED は各出力ごとに抵抗とトランジスタ等のドライバを介し、BCM GPIO 13・19・26 で制御
- Raspberry Pi とドライバ回路の GND を共通化

実際の端子と必要な抵抗値は、受信モジュール・LED・ドライバの仕様に合わせてください。

## 保存形式と制約

`codes.db` 内のSQLite `ir_signals` テーブルに信号名、マーク・スペース時間列、繰り返し回数、ギャップ時間を保存します。学習した波形をそのまま再生する方式なので、機器によっては長押しの繰り返しやエアコンの状態を含む信号を完全に再現できないことがあります。学習時はボタンを短く一度押してください。送信搬送波は約 38 kHz 固定です。

pigpio の波形生成により送信します。Pi Zero 2 W では GPIO 番号と pigpio の導入状況を確認してください。OS の更新で pigpio が動かない場合、このプログラムも動作しません。

## Enhancement
機能拡張については、[機能拡張](./spec.md)を参照のこと。
