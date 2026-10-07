"""Offline regression tests for the application's actual Yahoo fetch functions."""
import importlib
import math
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import yfinance_data as data


class Ticker:
    def __init__(self, history=None, failure=None, info=None):
        self.frame = history
        self.failure = failure
        self.information = info

    def history(self, **kwargs):
        assert kwargs["timeout"] > 0 and kwargs["raise_errors"]
        if self.failure:
            raise self.failure
        return self.frame

    @property
    def info(self):
        if isinstance(self.information, Exception):
            raise self.information
        return self.information

    def get_history_metadata(self):
        return {"exchangeName": "SNP", "currency": "USD"}


def bars(closes=(100.0, 110.0)):
    return pd.DataFrame({"Close": closes, "Open": closes, "High": closes,
                         "Low": closes, "Volume": [123] * len(closes)},
                        index=pd.date_range("2026-01-02", periods=len(closes), tz="UTC"))


class YahooResilienceTest(unittest.TestCase):
    def test_quote_does_not_depend_on_company_info(self):
        ticker = Ticker(bars(), info=RuntimeError("curl: (35) TLS connect error"))
        with patch.object(data.yf, "Ticker", return_value=ticker):
            quote = data.get_quote("^GSPC")
        self.assertNotIn("error", quote)
        self.assertEqual(quote["price"], 110)
        self.assertEqual(quote["previous_close"], 100)
        self.assertEqual(quote["change"], 10)
        self.assertEqual(quote["change_percent"], 10)
        self.assertEqual(quote["currency"], "USD")

    def test_transient_failure_retries_once(self):
        with patch.object(data.yf, "Ticker", side_effect=[
            Ticker(failure=RuntimeError("curl: (35) TLS connect error")), Ticker(bars())
        ]) as factory:
            quote = data.get_quote("^GSPC")
        self.assertNotIn("error", quote)
        self.assertEqual(factory.call_count, 2)

    def test_exhausted_retry_returns_error(self):
        with patch.object(data.yf, "Ticker", return_value=Ticker(
            failure=RuntimeError("curl: (28) timed out"))) as factory:
            result = data.get_quote("^GSPC")
        self.assertIn("timed out", result["error"])
        self.assertEqual(factory.call_count, 2)

    def test_provider_and_certificate_errors_do_not_retry(self):
        for message in ["invalid ticker", "Too Many Requests (429)",
                        "curl: (60) SSL peer certificate verification failed"]:
            with self.subTest(message=message), patch.object(data.yf, "Ticker", return_value=Ticker(
                failure=RuntimeError(message))) as factory:
                self.assertIn("error", data.get_quote("BAD"))
                self.assertEqual(factory.call_count, 1)

    def test_invalid_prices_do_not_poison_quote(self):
        with patch.object(data.yf, "Ticker", return_value=Ticker(bars((100, 110, math.nan)))):
            quote = data.get_quote("AAPL")
        self.assertEqual(quote["price"], 110)
        self.assertTrue(quote["change_available"])
        with patch.object(data.yf, "Ticker", return_value=Ticker(bars((math.nan, math.inf, 0)))):
            self.assertIn("error", data.get_quote("AAPL"))

    def test_missing_previous_close_is_explicit(self):
        with patch.object(data.yf, "Ticker", return_value=Ticker(bars((110,)))):
            quote = data.get_quote("NEW")
        self.assertEqual(quote["price"], 110)
        self.assertFalse(quote["change_available"])
        self.assertIsNone(quote["change"])
        self.assertIsNone(quote["previous_close"])

    def test_info_and_history_recover_from_transport_failure(self):
        with patch.object(data.yf, "Ticker", side_effect=[
            Ticker(info=RuntimeError("curl: (35) TLS connect error")), Ticker(info={"longName": "Index"})
        ]) as factory:
            self.assertEqual(data.get_info("^GSPC")["company_name"], "Index")
            self.assertEqual(factory.call_count, 2)
        with patch.object(data.yf, "Ticker", side_effect=[
            Ticker(failure=RuntimeError("connection reset")), Ticker(bars())
        ]) as factory:
            history = data.get_historical_period("^GSPC", "1mo", "1d")
            self.assertEqual(len(history), 2)
            self.assertEqual(factory.call_count, 2)

    def test_invalid_timeout_configuration_has_finite_default(self):
        for value in ["nan", "inf", "-1", "0", "bad"]:
            with patch.dict(os.environ, {"FINCEPT_YF_TIMEOUT": value}):
                importlib.reload(data)
                self.assertEqual(data._NET_TIMEOUT, 15.0)
        importlib.reload(data)


if __name__ == "__main__":
    unittest.main()
