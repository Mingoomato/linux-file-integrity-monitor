# Security policy

## Scope

This is educational Linux security lab software. It is not certified or supported as a production endpoint protection, backup, or policy enforcement product. Do not use it to protect important data or systems.

## Reporting a vulnerability

Please report suspected security issues privately to the repository owner before public disclosure. If GitHub private vulnerability reporting is enabled for the repository, use that channel. Include the affected project, revision, a concise impact description, and a safe reproduction in a disposable environment. Do not include real credentials, private keys, personal data, or destructive payloads.

## Safe validation

Kernel module loading and privileged fanotify/BPF experiments belong in a disposable VM with a snapshot and recovery console. The checked-in tests use temporary lab data and do not load kernel modules or enable enforcement.
