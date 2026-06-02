from __future__ import annotations

import csv
import datetime as dt
import json
import logging
from logging.handlers import RotatingFileHandler
import queue
import threading
import time
import urllib.request
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass
from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, X, Button, Checkbutton, DoubleVar, Entry, IntVar, Label, StringVar, Tk, BooleanVar, filedialog, messagebox, Listbox
from tkinter import ttk

import yfinance as yf


TWSE_LISTED_URL = "https://openapi.twse.com.tw/v1/opendata/t187ap03_L"
TPEX_LISTED_URL = "https://www.tpex.org.tw/openapi/v1/mopsfin_t187ap03_O"
TWSE_ETF_URL = "https://openapi.twse.com.tw/v1/opendata/t187ap47_L"
TPEX_ETF_URL = "https://info.tpex.org.tw/api/etfFilter"
CACHE_DIR = Path(__file__).with_name(".cache")
CACHE_FILE = CACHE_DIR / "tw_stock_list.json"
LOG_DIR = Path(__file__).with_name("logs")
LOG_FILE = LOG_DIR / "tw_cash_yield_ui.log"
APP_VERSION = "1.1.0"


def setup_logging() -> logging.Logger:
    LOG_DIR.mkdir(exist_ok=True)
    logger = logging.getLogger("tw_cash_yield_ui")
    logger.setLevel(logging.INFO)
    if not logger.handlers:
        handler = RotatingFileHandler(
            LOG_FILE,
            maxBytes=1_000_000,
            backupCount=5,
            encoding="utf-8",
        )
        formatter = logging.Formatter(
            "%(asctime)s %(levelname)s [%(threadName)s] %(message)s"
        )
        handler.setFormatter(formatter)
        logger.addHandler(handler)
    return logger


LOGGER = setup_logging()


# === 新增：將 Log 傳遞到介面的 Handler ===
class QueueHandler(logging.Handler):
    def __init__(self, log_queue: queue.Queue) -> None:
        super().__init__()
        self.log_queue = log_queue

    def emit(self, record: logging.LogRecord) -> None:
        self.log_queue.put(self.format(record))
# ========================================


@dataclass(frozen=True)
class Stock:
    code: str
    name: str
    suffix: str

    @property
    def yahoo_symbol(self) -> str:
        return f"{self.code}{self.suffix}"


@dataclass
class YieldResult:
    code: str
    name: str
    price: float
    yields: dict[int, float]
    average_yield: float


def fetch_json(url: str, timeout: int = 20) -> list[dict]:
    LOGGER.info("fetch_json url=%s", url)
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "Mozilla/5.0",
            "Accept": "application/json,text/plain,*/*",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8-sig"))


def fetch_post_json(url: str, timeout: int = 20) -> dict:
    LOGGER.info("fetch_post_json url=%s", url)
    request = urllib.request.Request(
        url,
        data=b"",
        method="POST",
        headers={
            "User-Agent": "Mozilla/5.0",
            "Accept": "application/json,text/plain,*/*",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8-sig"))


def parse_stock_rows(rows: list[dict], suffix: str) -> list[Stock]:
    stocks: list[Stock] = []
    for row in rows:
        code = str(
            row.get("公司代號")
            or row.get("Code")
            or row.get("SecuritiesCompanyCode")
            or ""
        ).strip()
        name = str(
            row.get("公司名稱")
            or row.get("CompanyName")
            or row.get("CompanyAbbreviation")
            or ""
        ).strip()
        if code.isdigit() and len(code) == 4 and name:
            stocks.append(Stock(code=code, name=name, suffix=suffix))
    return stocks


def parse_twse_etf_rows(rows: list[dict]) -> list[Stock]:
    etfs: list[Stock] = []
    for row in rows:
        code = str(row.get("基金代號") or "").strip()
        name = str(row.get("基金簡稱") or row.get("基金中文名稱") or "").strip()
        if code and name:
            etfs.append(Stock(code=code, name=name, suffix=".TW"))
    return etfs


def parse_tpex_etf_rows(payload: dict) -> list[Stock]:
    etfs: list[Stock] = []
    for row in payload.get("data", []):
        code = str(row.get("stockNo") or "").strip()
        name = str(row.get("stockName") or "").strip()
        if code and name:
            etfs.append(Stock(code=code, name=name, suffix=".TWO"))
    return etfs


def fetch_etf_list() -> list[Stock]:
    listed = parse_twse_etf_rows(fetch_json(TWSE_ETF_URL))
    otc = parse_tpex_etf_rows(fetch_post_json(TPEX_ETF_URL))
    etfs = sorted({(s.code, s.suffix): s for s in listed + otc}.values(), key=lambda s: s.code)
    LOGGER.info("loaded_etf_list listed=%s otc=%s total=%s", len(listed), len(otc), len(etfs))
    return etfs


def load_stock_list(use_cache: bool = True, include_etfs: bool = True) -> list[Stock]:
    if use_cache and CACHE_FILE.exists():
        age = time.time() - CACHE_FILE.stat().st_mtime
        if age < 7 * 24 * 60 * 60:
            payload = json.loads(CACHE_FILE.read_text(encoding="utf-8"))
            stocks = [Stock(**item) for item in payload]
            if include_etfs:
                stocks = sorted({(s.code, s.suffix): s for s in stocks + fetch_etf_list()}.values(), key=lambda s: s.code)
            LOGGER.info(
                "loaded_stock_list cache=true include_etfs=%s total=%s",
                include_etfs,
                len(stocks),
            )
            return stocks

    listed = parse_stock_rows(fetch_json(TWSE_LISTED_URL), ".TW")
    otc = parse_stock_rows(fetch_json(TPEX_LISTED_URL), ".TWO")
    stocks = sorted({(s.code, s.suffix): s for s in listed + otc}.values(), key=lambda s: s.code)

    CACHE_DIR.mkdir(exist_ok=True)
    CACHE_FILE.write_text(
        json.dumps([stock.__dict__ for stock in stocks], ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    if include_etfs:
        stocks = sorted({(s.code, s.suffix): s for s in stocks + fetch_etf_list()}.values(), key=lambda s: s.code)
    LOGGER.info(
        "loaded_stock_list cache=false include_etfs=%s listed=%s otc=%s total=%s",
        include_etfs,
        len(listed),
        len(otc),
        len(stocks),
    )
    return stocks


def completed_recent_years(count: int = 5) -> list[int]:
    current_year = dt.date.today().year
    return list(range(current_year - count, current_year))


def latest_price_and_dividends(symbol: str, years: list[int]) -> tuple[float, dict[int, float]]:
    start = f"{min(years) - 1}-12-01"
    ticker = yf.Ticker(symbol)
    history = ticker.history(start=start, auto_adjust=False, actions=True, timeout=12)
    if history.empty or "Close" not in history:
        raise ValueError("no price history")

    close = history["Close"].dropna()
    if close.empty:
        raise ValueError("no close price")
    price = float(close.iloc[-1])
    if price <= 0:
        raise ValueError("invalid price")

    annual_dividends = {year: 0.0 for year in years}
    annual_ex_prices = {year: [] for year in years}

    if "Dividends" not in history:
        raise ValueError("no dividend column")

    dividend_rows = history[history["Dividends"].fillna(0) > 0]
    for timestamp, row in dividend_rows.iterrows():
        year = int(timestamp.year)
        if year not in annual_dividends:
            continue

        earlier_rows = history.loc[:timestamp]
        if len(earlier_rows) < 2:
            continue

        previous_close = float(earlier_rows["Close"].iloc[-2])
        split_ratio = float(row.get("Stock Splits", 0) or 0)
        if 0 < split_ratio < 1:
            LOGGER.info(
                "skip_reverse_split_dividend symbol=%s date=%s dividend=%s split_ratio=%s",
                symbol,
                timestamp.date(),
                float(row["Dividends"]),
                split_ratio,
            )
            continue
        if split_ratio <= 0:
            split_ratio = 1.0

        annual_dividends[year] += float(row["Dividends"])
        annual_ex_prices[year].append(previous_close * split_ratio)

    yields = {}
    for year in years:
        ex_prices = annual_ex_prices[year]
        if not ex_prices:
            yields[year] = 0.0
            continue
        average_ex_price = sum(ex_prices) / len(ex_prices)
        yields[year] = (annual_dividends[year] / average_ex_price) * 100
    return price, yields


def analyze_stock(stock: Stock, years: list[int], threshold: float) -> YieldResult | None:
    price, yields = latest_price_and_dividends(stock.yahoo_symbol, years)
    if all(yield_pct > threshold for yield_pct in yields.values()):
        avg = sum(yields.values()) / len(yields)
        return YieldResult(stock.code, stock.name, price, yields, avg)
    return None


class CashYieldApp:
    def __init__(self, root: Tk) -> None:
        self.root = root
        self.root.title(f"台股近五年現金殖利率篩選 v{APP_VERSION}")
        self.root.geometry("1120x720") # 稍微調大一點高度以容納 Log

        self.threshold = DoubleVar(value=5.0)
        self.limit = IntVar(value=10)
        self.workers = IntVar(value=3)
        self.use_cache = BooleanVar(value=True)
        self.include_etfs = BooleanVar(value=True)
        self.status = StringVar(value="準備就緒")
        self.progress_text = StringVar(value="")
        self.is_running = False
        self.stop_requested = False
        self.stop_event = threading.Event()
        self.results: list[YieldResult] = []
        self.queue: queue.Queue[tuple[int, str, object]] = queue.Queue()
        self.log_queue: queue.Queue[str] = queue.Queue() # 給 Log 用的 Queue
        self.years = completed_recent_years()
        self.active_run_id = 0
        
        self._build_ui()
        self._setup_ui_logger()
        
        LOGGER.info("app_start version=%s log_file=%s", APP_VERSION, LOG_FILE)
        self.root.after(150, self._drain_queue)

    def _setup_ui_logger(self) -> None:
        """設定將 Log 導向 UI 的機制"""
        handler = QueueHandler(self.log_queue)
        formatter = logging.Formatter("%(asctime)s - %(message)s", datefmt="%H:%M:%S")
        handler.setFormatter(formatter)
        LOGGER.addHandler(handler)
        self.root.after(100, self._drain_logs)

    def _drain_logs(self) -> None:
        """負責從背景拿取 Log 並顯示到介面上"""
        updated = False
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self.log_list.insert(END, msg)
                updated = True
        except queue.Empty:
            pass

        if updated:
            # 確保最多只保留 10 筆
            while self.log_list.size() > 10:
                self.log_list.delete(0)
            # 自動捲動到最底
            self.log_list.yview(END)

        self.root.after(100, self._drain_logs)

    def _build_ui(self) -> None:
        controls = ttk.Frame(self.root, padding=12)
        controls.pack(fill=X)

        Label(controls, text="門檻 >").pack(side=LEFT)
        Entry(controls, textvariable=self.threshold, width=6).pack(side=LEFT, padx=(4, 4))
        Label(controls, text="%").pack(side=LEFT, padx=(0, 16))

        Label(controls, text="前").pack(side=LEFT)
        Entry(controls, textvariable=self.limit, width=5).pack(side=LEFT, padx=(4, 4))
        Label(controls, text="檔").pack(side=LEFT, padx=(0, 16))

        Label(controls, text="平行數").pack(side=LEFT)
        Entry(controls, textvariable=self.workers, width=5).pack(side=LEFT, padx=(4, 16))

        Checkbutton(controls, text="使用股票清單快取", variable=self.use_cache).pack(side=LEFT, padx=(0, 16))
        Checkbutton(controls, text="包含 ETF", variable=self.include_etfs).pack(side=LEFT, padx=(0, 16))

        self.start_button = Button(controls, text="開始收集", command=self.start)
        self.start_button.pack(side=LEFT)
        self.stop_button = Button(controls, text="停止", command=self.stop, state="disabled")
        self.stop_button.pack(side=LEFT, padx=(8, 0))
        Button(controls, text="匯出 CSV", command=self.export_csv).pack(side=RIGHT)

        year_text = "、".join(str(year) for year in self.years)
        Label(self.root, text=f"年度：{year_text}，殖利率以各年度現金股利 / 除息前股價平均值計算。", anchor="w").pack(fill=X, padx=12)

        # === 修正排版：先將 footer 貼齊視窗最底部 ===
        footer = ttk.Frame(self.root, padding=(12, 6, 12, 12))
        footer.pack(side="bottom", fill=X)
        
        Label(footer, text=f"v{APP_VERSION}", anchor="w").pack(side=LEFT, padx=(0, 12))
        Label(footer, textvariable=self.status, anchor="w").pack(side=LEFT, fill=X, expand=True)
        
        self.progress_bar = ttk.Progressbar(footer, orient="horizontal", length=150, mode="determinate")
        self.progress_bar.pack(side=RIGHT, padx=(8, 0))
        
        Label(footer, textvariable=self.progress_text, anchor="e").pack(side=RIGHT)

        # === 新增：Log 日誌顯示區塊 (貼齊底部，在 footer 之上) ===
        log_frame = ttk.Frame(self.root)
        log_frame.pack(side="bottom", fill=X, padx=12, pady=(0, 0))
        
        # 使用 Listbox 顯示最新的 10 筆 Log，並加上黑色邊框
        self.log_list = Listbox(log_frame, height=10, relief="solid", borderwidth=1, bg="#f9f9f9")
        self.log_list.pack(fill=X, expand=True)

        # === 修正排版：用一個專屬的 Frame 包裝表格與滾動條 (填滿剩餘的上方空間) ===
        tree_frame = ttk.Frame(self.root)
        tree_frame.pack(fill=BOTH, expand=True, padx=12, pady=(12, 12))

        columns = ["rank", "code", "name", "price", *[str(year) for year in self.years], "avg"]
        
        self.tree = ttk.Treeview(tree_frame, columns=columns, show="headings", height=22)
        headings = {
            "rank": "排名",
            "code": "股票代碼",
            "name": "名稱",
            "price": "最新股價",
            "avg": "平均殖利率",
        }
        widths = {
            "rank": 60,
            "code": 90,
            "name": 150,
            "price": 100,
            "avg": 110,
        }
        for col in columns:
            self.tree.heading(col, text=headings.get(col, f"{col}殖利率"))
            self.tree.column(col, width=widths.get(col, 120), anchor="center")

        scrollbar = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)
        
        self.tree.pack(side=LEFT, fill=BOTH, expand=True)
        scrollbar.pack(side=RIGHT, fill="y")

    def start(self) -> None:
        if self.is_running:
            self.status.set("正在收集中，請等待完成或按停止。")
            return
        try:
            threshold = float(self.threshold.get())
            limit = int(self.limit.get())
            workers = max(1, min(8, int(self.workers.get())))
        except Exception:
            messagebox.showerror("輸入錯誤", "請確認門檻、前幾檔、平行數都是數字。")
            return
        if limit <= 0:
            messagebox.showerror("輸入錯誤", "前幾檔必須大於 0。")
            return

        self.is_running = True
        self.stop_requested = False
        self.stop_event = threading.Event()
        self.active_run_id += 1
        run_id = self.active_run_id
        self.results = []
        self.start_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.tree.delete(*self.tree.get_children())
        self.status.set("正在取得台股股票清單...")
        self.progress_text.set("")
        
        self.progress_bar["value"] = 0
        
        LOGGER.info(
            "run_start run_id=%s threshold=%s limit=%s workers=%s use_cache=%s include_etfs=%s years=%s",
            run_id,
            threshold,
            limit,
            workers,
            bool(self.use_cache.get()),
            bool(self.include_etfs.get()),
            self.years,
        )

        thread = threading.Thread(
            target=self._worker,
            args=(
                run_id,
                self.stop_event,
                threshold,
                limit,
                workers,
                bool(self.use_cache.get()),
                bool(self.include_etfs.get()),
            ),
            daemon=True,
        )
        thread.start()

    def stop(self) -> None:
        self.stop_requested = True
        self.stop_event.set()
        self.status.set("正在停止，會等目前執行中的查詢完成後才能再次開始...")
        LOGGER.info("run_stop_requested run_id=%s", self.active_run_id)

    def _worker(
        self,
        run_id: int,
        stop_event: threading.Event,
        threshold: float,
        limit: int,
        workers: int,
        use_cache: bool,
        include_etfs: bool,
    ) -> None:
        try:
            stocks = load_stock_list(use_cache=use_cache, include_etfs=include_etfs)
            self.queue.put((run_id, "status", f"共取得 {len(stocks)} 檔股票，開始分析股利與股價..."))
            LOGGER.info("run_loaded_stocks run_id=%s total=%s", run_id, len(stocks))
            total = len(stocks)
            done = 0
            next_index = 0
            fatal_error: str | None = None
            matches: list[YieldResult] = []
            active: set[Future] = set()
            max_in_flight = max(workers * 2, workers)

            executor = ThreadPoolExecutor(max_workers=workers)
            try:
                while (next_index < total or active) and not stop_event.is_set():
                    while next_index < total and len(active) < max_in_flight and not stop_event.is_set():
                        stock = stocks[next_index]
                        active.add(executor.submit(analyze_stock, stock, self.years, threshold))
                        next_index += 1

                    if not active:
                        break

                    completed, active = wait(active, return_when=FIRST_COMPLETED)
                    for future in completed:
                        done += 1
                        try:
                            result = future.result()
                        except Exception as exc:
                            if self._is_rate_limit_error(exc):
                                fatal_error = (
                                    "Yahoo Finance 暫時限制查詢流量。請等 10-30 分鐘後再試，"
                                    "或把平行數調低到 1-2。"
                                )
                                LOGGER.warning("run_rate_limited run_id=%s error=%r", run_id, exc)
                                stop_event.set()
                                result = None
                                break
                            LOGGER.debug("stock_analyze_failed run_id=%s error=%r", run_id, exc)
                            result = None
                        if result:
                            matches.append(result)
                            matches.sort(key=lambda item: item.average_yield, reverse=True)
                            self.queue.put((run_id, "results", matches[:limit]))
                        if done == 1 or done % 20 == 0 or done == total:
                            self.queue.put((run_id, "progress", (done, total)))

                    if fatal_error:
                        break

                if stop_event.is_set():
                    for future in active:
                        future.cancel()
            finally:
                executor.shutdown(wait=True, cancel_futures=stop_event.is_set())

            matches.sort(key=lambda item: item.average_yield, reverse=True)
            if fatal_error:
                self.queue.put((run_id, "error", fatal_error))
            else:
                LOGGER.info(
                    "run_done run_id=%s stopped=%s scanned=%s matches=%s",
                    run_id,
                    stop_event.is_set(),
                    done,
                    len(matches),
                )
                self.queue.put((run_id, "done", (matches[:limit], stop_event.is_set())))
        except Exception as exc:
            LOGGER.exception("run_failed run_id=%s", run_id)
            self.queue.put((run_id, "error", str(exc)))

    @staticmethod
    def _is_rate_limit_error(exc: Exception) -> bool:
        text = str(exc).lower()
        return exc.__class__.__name__ == "YFRateLimitError" or "rate limit" in text or "too many requests" in text

    def _drain_queue(self) -> None:
        try:
            while True:
                run_id, kind, payload = self.queue.get_nowait()
                if run_id != self.active_run_id:
                    continue
                if kind == "status":
                    self.status.set(str(payload))
                elif kind == "progress":
                    done, total = payload
                    self.progress_text.set(f"{done}/{total}")
                    
                    if total > 0:
                        self.progress_bar["value"] = (done / total) * 100
                        
                elif kind == "results":
                    self._render(payload)
                elif kind == "done":
                    results, stopped = payload
                    self.results = list(results)
                    self._render(self.results)
                    self._finish("已停止" if stopped else f"完成，共顯示 {len(self.results)} 檔。")
                elif kind == "error":
                    LOGGER.error("run_error run_id=%s message=%s", run_id, payload)
                    self._finish("發生錯誤")
                    messagebox.showerror("錯誤", str(payload))
        except queue.Empty:
            pass
        self.root.after(150, self._drain_queue)

    def _finish(self, message: str) -> None:
        self.is_running = False
        self.stop_requested = False
        self.start_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        self.status.set(message)
        
        if "完成" in message:
            self.progress_bar["value"] = 100

    def _render(self, results: list[YieldResult]) -> None:
        self.tree.delete(*self.tree.get_children())
        for index, result in enumerate(results, start=1):
            row = [
                index,
                result.code,
                result.name,
                f"{result.price:.2f}",
                *[f"{result.yields[year]:.2f}%" for year in self.years],
                f"{result.average_yield:.2f}%",
            ]
            self.tree.insert("", END, values=row)

    def export_csv(self) -> None:
        if not self.results:
            messagebox.showinfo("沒有資料", "目前沒有可匯出的結果。")
            return
        path = filedialog.asksaveasfilename(
            title="匯出 CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv")],
            initialfile="tw_cash_yield_top10.csv",
        )
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8-sig") as file:
            writer = csv.writer(file)
            writer.writerow([f"程式版本 v{APP_VERSION}"])
            writer.writerow(["排名", "股票代碼", "名稱", "最新股價", *[f"{year}現金殖利率" for year in self.years], "平均殖利率"])
            for index, result in enumerate(self.results, start=1):
                writer.writerow([
                    index,
                    result.code,
                    result.name,
                    f"{result.price:.2f}",
                    *[f"{result.yields[year]:.2f}%" for year in self.years],
                    f"{result.average_yield:.2f}%",
                ])
        LOGGER.info("export_csv path=%s rows=%s version=%s", path, len(self.results), APP_VERSION)
        messagebox.showinfo("完成", f"已匯出：{path}")


def main() -> None:
    root = Tk()
    app = CashYieldApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
