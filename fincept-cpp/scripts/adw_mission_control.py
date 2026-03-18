"""
ADW AI Mission Control — Python analytics bridge
Bridges trading-bot and forex-bot ML models to the FinceptTerminal C++ UI.

Returns a JSON blob with:
  - futures signals (ES=F, NQ=F, YM=F)
  - forex signals (majors + user-configured pairs)
  - bot status (mode, equity, drawdown, circuit breaker)
  - risk metrics

Invocation (from C++ via python_runner):
    adw_mission_control.py [refresh|status|signals|all]

Output: single JSON object to stdout (last JSON line extracted by python_runner)
"""

import sys
import json
import os
import time
import random
import traceback
from datetime import datetime, timezone
from pathlib import Path

# ─── resolve bot roots ───────────────────────────────────────────────────────
# FinceptTerminal lives at  <repo>/fincept-cpp/
# Our bots live at          <repo>/../Antigravity.integration.combaus/trading-bot/
# We search upward from this file for the combaus repo.

def _find_combaus_root() -> Path | None:
    here = Path(__file__).resolve()
    for parent in [here.parent.parent.parent.parent, here.parent.parent.parent]:
        candidate = parent / "Antigravity.integration.combaus"
        if candidate.exists():
            return candidate
    # Try common Windows user home path
    home = Path.home()
    candidate = home / "Antigravity.integration.combaus"
    if candidate.exists():
        return candidate
    return None

COMBAUS_ROOT = _find_combaus_root()
FUTURES_MODELS_DIR = COMBAUS_ROOT / "trading-bot" / "models" if COMBAUS_ROOT else None
FOREX_MODELS_DIR   = COMBAUS_ROOT / "forex-bot"   / "models" if COMBAUS_ROOT else None

# ─── helpers ─────────────────────────────────────────────────────────────────

FUTURES_SYMBOLS = ["ES=F", "NQ=F", "YM=F", "RTY=F", "CL=F", "GC=F"]
FOREX_SYMBOLS   = [
    "EURUSD=X","GBPUSD=X","USDJPY=X","USDCHF=X","AUDUSD=X","USDCAD=X",
    "NZDUSD=X","EURGBP=X","EURJPY=X","GBPJPY=X","AUDJPY=X","CADJPY=X",
]

def _model_exists(models_dir: Path | None, symbol: str) -> bool:
    if not models_dir:
        return False
    slug = symbol.replace("=F","").replace("=X","")
    return (models_dir / f"{slug}_model.pkl").exists()


def _live_signal(symbol: str, models_dir: Path | None) -> dict:
    """
    Try to load XGBoost model and score latest bar.
    Falls back to synthetic demo data when model/data unavailable.
    """
    slug = symbol.replace("=F","").replace("=X","")
    model_path = models_dir / f"{slug}_model.pkl" if models_dir else None
    scaler_path = models_dir / f"{slug}_scaler.pkl" if models_dir else None

    if model_path and model_path.exists() and scaler_path and scaler_path.exists():
        try:
            import pickle, numpy as np
            import yfinance as yf, pandas as pd
            from io import StringIO
            import contextlib

            with open(model_path, "rb") as f:
                model = pickle.load(f)
            with open(scaler_path, "rb") as f:
                scaler = pickle.load(f)

            buf = StringIO()
            with contextlib.redirect_stdout(buf):
                hist = yf.Ticker(symbol).history(period="60d", interval="1h")

            if hist.empty or len(hist) < 30:
                raise ValueError("insufficient history")

            # Minimal feature set matching trainer.py
            df = hist[["Open","High","Low","Close","Volume"]].copy()
            df.columns = ["open","high","low","close","volume"]

            # RSI-14
            delta = df["close"].diff()
            gain  = delta.clip(lower=0).rolling(14).mean()
            loss  = (-delta.clip(upper=0)).rolling(14).mean()
            rs    = gain / loss.replace(0, 1e-9)
            df["rsi"] = 100 - (100 / (1 + rs))

            # MACD
            ema12 = df["close"].ewm(span=12).mean()
            ema26 = df["close"].ewm(span=26).mean()
            df["macd"] = ema12 - ema26
            df["macd_sig"] = df["macd"].ewm(span=9).mean()

            # ATR
            hl = df["high"] - df["low"]
            hc = (df["high"] - df["close"].shift()).abs()
            lc = (df["low"]  - df["close"].shift()).abs()
            df["atr"] = pd.concat([hl,hc,lc], axis=1).max(axis=1).rolling(14).mean()

            df.dropna(inplace=True)
            feat_cols = ["rsi","macd","macd_sig","atr","volume"]
            feat_cols = [c for c in feat_cols if c in df.columns]
            X = scaler.transform(df[feat_cols].iloc[[-1]])
            proba = model.predict_proba(X)[0]
            confidence = float(max(proba))
            direction  = "LONG" if proba.argmax() == 1 else "SHORT"

            return {
                "symbol": symbol,
                "direction": direction,
                "confidence": round(confidence * 100, 1),
                "price": round(float(df["close"].iloc[-1]), 4),
                "rsi": round(float(df["rsi"].iloc[-1]), 1),
                "model_loaded": True,
                "timestamp": int(time.time()),
            }
        except Exception as e:
            pass  # fall through to demo data

    # ── Demo / synthetic signal (no model present) ────────────────────────────
    seed = hash(symbol + str(int(time.time() // 300))) & 0xFFFF
    rng = random.Random(seed)
    confidence = round(rng.uniform(52, 94), 1)
    direction  = rng.choice(["LONG", "SHORT"])
    return {
        "symbol": symbol,
        "direction": direction,
        "confidence": confidence,
        "price": None,
        "rsi": round(rng.uniform(30, 70), 1),
        "model_loaded": False,
        "timestamp": int(time.time()),
    }


def _circuit_breaker_status(models_dir: Path | None) -> dict:
    """Check for circuit-breaker state file written by risk manager."""
    if not models_dir:
        return {"triggered": False, "reason": "no_model_dir"}
    state_file = models_dir.parent / "circuit_breaker.json"
    if state_file.exists():
        try:
            with open(state_file) as f:
                return json.load(f)
        except Exception:
            pass
    return {"triggered": False, "reason": None}


def _read_equity_log(models_dir: Path | None) -> dict:
    """Read latest equity snapshot written by main.py paper loop."""
    if not models_dir:
        return {"equity": None, "daily_pnl": None, "drawdown": None}
    eq_file = models_dir.parent / "equity.json"
    if eq_file.exists():
        try:
            with open(eq_file) as f:
                return json.load(f)
        except Exception:
            pass
    return {"equity": None, "daily_pnl": None, "drawdown": None}


def _read_mode(env_key: str, default: str) -> str:
    """Read trading mode from environment or config file."""
    mode = os.environ.get(env_key, "").upper()
    if mode in ("CONSERVATIVE", "AGGRESSIVE", "PAPER", "LIVE"):
        return mode
    return default

# ─── main payload builder ────────────────────────────────────────────────────

def build_payload() -> dict:
    futures_mode = _read_mode("TRADING_MODE", "PAPER")
    forex_mode   = _read_mode("FOREX_MODE",   "CONSERVATIVE")

    futures_signals = [_live_signal(sym, FUTURES_MODELS_DIR) for sym in FUTURES_SYMBOLS]
    forex_signals   = [_live_signal(sym, FOREX_MODELS_DIR)   for sym in FOREX_SYMBOLS]

    futures_cb  = _circuit_breaker_status(FUTURES_MODELS_DIR)
    forex_cb    = _circuit_breaker_status(FOREX_MODELS_DIR)
    futures_eq  = _read_equity_log(FUTURES_MODELS_DIR)
    forex_eq    = _read_equity_log(FOREX_MODELS_DIR)

    futures_models_ready = any(s["model_loaded"] for s in futures_signals)
    forex_models_ready   = any(s["model_loaded"] for s in forex_signals)

    avg_futures_conf = (sum(s["confidence"] for s in futures_signals) / len(futures_signals)) if futures_signals else 0
    avg_forex_conf   = (sum(s["confidence"] for s in forex_signals)   / len(forex_signals))   if forex_signals   else 0

    return {
        "ok": True,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "combaus_root": str(COMBAUS_ROOT) if COMBAUS_ROOT else None,
        "futures": {
            "mode": futures_mode,
            "models_ready": futures_models_ready,
            "circuit_breaker": futures_cb,
            "equity": futures_eq,
            "avg_confidence": round(avg_futures_conf, 1),
            "signals": futures_signals,
        },
        "forex": {
            "mode": forex_mode,
            "models_ready": forex_models_ready,
            "circuit_breaker": forex_cb,
            "equity": forex_eq,
            "avg_confidence": round(avg_forex_conf, 1),
            "signals": forex_signals,
        },
    }

# ─── entry point ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    try:
        payload = build_payload()
        print(json.dumps(payload))
    except Exception as e:
        err = {
            "ok": False,
            "error": str(e),
            "traceback": traceback.format_exc(),
        }
        print(json.dumps(err))
        sys.exit(1)
