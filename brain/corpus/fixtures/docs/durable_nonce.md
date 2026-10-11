# Durable nonces

A durable nonce lets a transaction stay valid long after the recent blockhash it would
normally carry has expired. That is useful for offline signing: the robot can sign a payment
with no network and the owner can broadcast it hours later from a phone. It is also a classic
trick in phishing dapps, because a transaction that never expires can be collected now and
broadcast when the victim's balance is highest. The filter therefore treats a durable nonce
that arrives from a website as high risk, and an offline payment the owner created on purpose
as normal. The difference is who asked for it, which is something the daemon knows and a
language model does not need to guess.
