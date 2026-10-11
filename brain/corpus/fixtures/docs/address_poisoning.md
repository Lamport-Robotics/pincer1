# Address poisoning

Address poisoning is a scam that relies on the way wallets display addresses. The attacker
generates a vanity address whose first and last characters match a contact the victim pays
often, then sends a dust transfer so the lookalike shows up in the victim's history. When the
victim copies an address from that history instead of from their address book, the money goes
to the attacker. The defence is boring and reliable: compare the whole address, keep a labelled
address book, and have the wallet refuse a destination that matches a contact on both ends but
differs in the middle. Pincer1 treats that shape as a block, not a warning, because nothing
legitimate produces it.
