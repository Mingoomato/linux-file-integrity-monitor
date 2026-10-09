# Contributing

1. Keep changes within the documented lab scope and preserve explicit fail-closed behavior.
2. Build with the commands in the project README and run the checked-in smoke test before submitting.
3. Add or update requirement traceability and test-result notes when behavior changes.
4. Never commit private keys, certificates, local databases, build output, or real endpoint data.
5. Do not load the kernel module or run privileged hook experiments on a shared host. Use a disposable Linux VM with a recovery console.
6. Describe known limitations and distinguish synthetic evidence from runtime or production evidence.
