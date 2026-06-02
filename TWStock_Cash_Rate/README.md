# 台股近五年現金殖利率 UI

目前版本：`v1.1.0`

這個工具會收集台股上市與上櫃股票，篩選近五個完整年度現金殖利率都大於指定門檻的股票，並在桌面 UI 顯示前 N 檔。

可在 UI 勾選「包含 ETF」，將上市與上櫃 ETF 一起納入掃描。

## 執行

```bash
python3 -m pip install -r requirements.txt
python3 tw_cash_yield_ui.py
```

macOS 也可以直接雙擊：

```text
run_tw_cash_yield_ui.command
```

若 Yahoo Finance 顯示暫時限制查詢流量，請等 10-30 分鐘後再試，或把 UI 裡的「平行數」調成 1-2。

## Log

程式會把每次執行記錄到：

```text
logs/tw_cash_yield_ui.log
```

Log 會記錄版本號、資料來源載入、執行參數、Yahoo 限流、單檔查詢錯誤，以及被排除的特殊股利事件，例如 Yahoo Finance 將減資/資本返還列入 `Dividends` 的情況。

## 顯示欄位

- 股票代碼
- 名稱
- 最新股價
- 近五年每年現金殖利率
- 平均殖利率

現金殖利率計算方式：

```text
年度現金股利總和 / 該年度除息前股價平均值 * 100
```

預設取近五個已完整年度。若目前是 2026 年，年度會是 2021-2025。

UI 仍會另外顯示最新股價，但歷史年度殖利率不會用最新股價當分母。

## 資料來源

- 上市股票清單：TWSE OpenAPI
- 上櫃股票清單：TPEx OpenAPI
- 上市 ETF 清單：TWSE 基金基本資料彙總表 OpenAPI
- 上櫃 ETF 清單：TPEx ETF InfoHub 篩選器 API
- 股價與股利：Yahoo Finance，透過 `yfinance`
