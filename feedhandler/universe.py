"""Default trading universe: ~100 popular Coinbase USD products, ordered
roughly by market cap / liquidity.

Used as the boot subscription set for the Coinbase feed so the terminal has a
rich Market Watch out of the box (no manual searching needed). The feedhandler
intersects this with the live product catalog before subscribing, so any entry
that has since been delisted is simply skipped.
"""

from __future__ import annotations

TOP_CRYPTO: list[str] = [
    "BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD", "DOGE-USD", "ADA-USD",
    "AVAX-USD", "LINK-USD", "DOT-USD", "LTC-USD", "BCH-USD", "SHIB-USD",
    "XLM-USD", "UNI-USD", "ATOM-USD", "ETC-USD", "HBAR-USD", "FIL-USD",
    "ICP-USD", "APT-USD", "NEAR-USD", "ARB-USD", "OP-USD", "IMX-USD",
    "INJ-USD", "GRT-USD", "AAVE-USD", "MKR-USD", "ALGO-USD", "RENDER-USD",
    "SAND-USD", "MANA-USD", "AXS-USD", "FLOW-USD", "XTZ-USD", "CHZ-USD",
    "CRV-USD", "LDO-USD", "SNX-USD", "COMP-USD", "SUI-USD", "SEI-USD",
    "TIA-USD", "JTO-USD", "PYTH-USD", "BONK-USD", "JUP-USD", "STX-USD",
    "FET-USD", "APE-USD", "GALA-USD", "LRC-USD", "BAT-USD", "ZEC-USD",
    "DASH-USD", "1INCH-USD", "YFI-USD", "SUSHI-USD", "UMA-USD", "BAL-USD",
    "ANKR-USD", "SKL-USD", "STORJ-USD", "ENS-USD", "DYDX-USD", "MINA-USD",
    "ROSE-USD", "KAVA-USD", "ZRX-USD", "QNT-USD", "EGLD-USD", "CELO-USD",
    "BAND-USD", "NMR-USD", "CVX-USD", "RPL-USD", "AUDIO-USD", "FORTH-USD",
    "GTC-USD", "MASK-USD", "ACH-USD", "POWR-USD", "CTSI-USD", "LPT-USD",
    "KNC-USD", "BNT-USD", "OGN-USD", "API3-USD", "BADGER-USD", "RLC-USD",
    "TRB-USD", "SUPER-USD", "RARE-USD", "MDT-USD", "SPELL-USD", "JASMY-USD",
    "AERO-USD", "PEPE-USD", "WIF-USD", "ONDO-USD",
]
