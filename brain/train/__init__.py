"""Training stages: continued pretraining (cpt), SFT, DPO, and the LoRA merge.

Every script prints its plan and exits unless ``--run`` is given; ``--dry-run`` is the same
plan without the GPU imports. Nothing here downloads a model or touches a GPU on its own.
"""
