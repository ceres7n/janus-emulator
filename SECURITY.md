# Security policy

## Supported versions

| Version | Supported |
|---|---|
| 0.1.x (public beta) | Yes |

## Reporting a vulnerability

**Please do not report vulnerabilities in public issues, discussions or chat channels.**

Report them privately through GitHub's **private vulnerability reporting**: the "Report a
vulnerability" button on the Security tab of <https://github.com/ceres7n/janus-emulator>
(<https://github.com/ceres7n/janus-emulator/security>). Only the maintainers can read these
reports.

Please include:
- the version (each server prints it when it starts);
- which server is affected (Auth, Login, Master or Game);
- the steps or the packet sequence that reproduce the problem;
- what an attacker gains (for example another player's items, a crash, access to the database);
- any idea for a fix.

Please give the maintainers reasonable time to release a fix before you talk about the problem in
public. Do not test a vulnerability on a server you do not run yourself, and do not access or
change other players' data.

## Known limitations (not vulnerabilities)

These are documented limits of the v0.1.0 beta. They do not need a report:

- **There are no account passwords.** The account name is the only key. Operators must create
  account names nobody can guess and hand them out privately.
- **The MasterServer (TCP 9010) has no authentication.** It must only be reachable from the same
  machine (its default `bind_ip` is `127.0.0.1`). The same holds for the database port.

A way to get around these limits is still worth a private report, for example logging in to an
account without knowing its name.

## Checklist for operators

- Create accounts with names nobody can guess, one per player.
- Let the servers use a database user with `SELECT`, `INSERT`, `UPDATE` and `DELETE` only, and
  keep MariaDB on `127.0.0.1`.
- Open only TCP 9980, 8001, 8002 and 8003. Never forward 9010 or the database port.
- Keep `log_level: info`. `debug` writes every packet to the logs.
- The logs contain account names and IP addresses: do not publish them unedited.

Details: [doc/INSTALL.md](doc/INSTALL.md#10-public-hosting).
