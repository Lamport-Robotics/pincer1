# Security

Pincer1 holds keys. If you find a way to make it sign without its owner, read what the owner did not
approve, leak a seed, bypass the presence gate, the PIN, the duress wallet or the daily limits, or get
code onto the consent controller, we want to know before anyone else does.

Write to support@lamportrobotics.com with "security" in the subject. Include what you did, what you
saw and what you expected; a proof of concept against `CONTROLLER=sim` or the host build of the firmware
is ideal. We answer within three days, fix in the open once a fix exists, and credit you in the commit
unless you ask us not to. Do not open a public issue for a vulnerability.

In scope: everything in this repository (`firmware/`, `daemon/`, `extension/`, `robot/`, `beacon/`) and
the hardware design it describes. The threat model is `docs/SECURITY.md`; findings that sit inside what
it already accepts (a replayed video of the owner's face, a stolen seed read off the display at init)
are still welcome as issues, not as reports.

Firmware images are signed and the controller boots only images whose signing key hash is in its OTP;
`hello` reports the firmware version it runs. A controller that boots an image we did not sign, or
reports a version we did not ship, is a report too.

Every release lists the SHA-256 of its images. The unsigned builds are reproducible: build the tag from
source and the hash matches `SHA256SUMS`, which CI checks by building twice. The signed image provisioned
boards run is uploaded to the same release with its hash and the hash of the signing key
(`SHA256SUMS.signed`); `firmware/scripts/verify-image.sh` checks a file against the list and, with
`--board`, a controller's flash against the file. A release whose listed hash you cannot reproduce from
the tag is a report too.
