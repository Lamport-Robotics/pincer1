"""Make ``pincer_brain`` importable from a plain checkout (no ``pip install -e .``).

The package's import name is ``pincer_brain`` but the directory is ``brain/``; when the
package is not installed we register this directory under that name before collection.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent


def _register() -> None:
    if "pincer_brain" in sys.modules:
        return
    try:
        import pincer_brain  # noqa: F401  (installed copy wins)

        return
    except ImportError:
        pass
    spec = importlib.util.spec_from_file_location(
        "pincer_brain", _ROOT / "__init__.py", submodule_search_locations=[str(_ROOT)]
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["pincer_brain"] = module
    spec.loader.exec_module(module)


_register()
