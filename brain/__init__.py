"""Lamport-8B training pipeline (import name ``pincer_brain``).

The directory ``brain/`` is the package: ``pincer_brain.sft`` is ``brain/sft``, and so on.
Nothing in this package imports torch, transformers or the network at module import time;
the training scripts import them inside ``main()`` so the tests run on a laptop.
"""

from __future__ import annotations

from pathlib import Path

__version__ = "0.1.0"

#: The ``brain/`` directory.
ROOT = Path(__file__).resolve().parent
#: The Pincer1 repository root (parent of ``brain/``), if we are inside it.
REPO = ROOT.parent
