# HUMA_TechniqueAnalysis

Hawk-Eye のフレームデータから選手の関節軌跡を取り出し、技の発動区間（時刻表）を解析するスクリプトです。

```powershell
pip install numpy cramjam pymongo matplotlib   # bson は pymongo に含まれます
python parse.py [Hawk-Eyeフレームフォルダ]   # 省略時は ../../Data/HawkEye → f9_people.npz
python prep.py                                  # → f9_players.npz（赤=jersey1 / 青=jersey2）
python analyze.py                               # → techniques_F9.json（技イベント一覧）
python viz2.py JAB RED L out.png 0              # 任意: 技ごとのスケルトン確認画像
```

`technique_table_F9.csv` は F9 データでの解析結果です。UE 側では `BP_TechniqueTimeline` の配列に取り込み済みです。
