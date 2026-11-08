# Security Policy

## Supported versions

Security fixes are provided for the latest stable release of Genia Unlocker.

| Version | Supported |
| --- | --- |
| 0.5.0 | Yes |
| 0.4.1 and older | No |

## Reporting a vulnerability

Please do not publish a security-sensitive issue before giving the project
maintainer a reasonable opportunity to investigate it.

Use GitHub's private vulnerability reporting feature for this repository when
available. Include:

- Genia Unlocker version;
- Windows version/build and architecture;
- exact steps required to reproduce the issue;
- whether the process was elevated;
- crash/error details and relevant diagnostic output.

Do not include passwords, private files, tokens, personal documents, or other
unrelated sensitive data.

## Scope

Genia Unlocker intentionally performs advanced user-mode operations involving
processes and file handles. A warning shown before Force Unlock or process
termination is part of the security boundary and should not be silently
bypassed.
