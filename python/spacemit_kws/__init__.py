"""
spacemit_kws - SpacemiT KWS (keyword spotting) Python Package

Usage:
    import spacemit_kws
    result = spacemit_kws.detect(audio)                  # 一次性检测
    engine = spacemit_kws.KwsEngine("xiaojin-4mic")      # 流式检测
    engine.set_callback(spacemit_kws.KwsCallback())
"""

from ._spacemit_kws import (
    # Enums
    KwsBackendType,
    KwsAudioStatus,
    KwsStreamStats,
    # Config
    KwsConfig,
    KwsKeyword,
    # Result
    KwsResult,
    # Engine
    KwsEngine,
    # Callback
    KwsCallback,
    # Quick function
    detect,
    # Module info
    __version__,
)

__all__ = [
    "KwsBackendType",
    "KwsAudioStatus",
    "KwsStreamStats",
    "KwsConfig",
    "KwsKeyword",
    "KwsResult",
    "KwsEngine",
    "KwsCallback",
    "detect",
    "__version__",
]
