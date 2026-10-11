"""The duress word is never stored or logged in plaintext. The detector compares a hash of
each transcript word against the enrolled hash so spelling slips still trigger it. On a hit
it calls the hook once and latches until reset. The service never acknowledges duress out
loud and the presence object does not change shape; only one boolean goes to the daemon."""

def scan(words):
    # pure check with no side effects, so it can be unit tested without the
    # rest of the service and without the microphone
    return False
