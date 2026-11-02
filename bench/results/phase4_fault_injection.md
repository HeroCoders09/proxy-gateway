# Phase 4 fault injection results

Local test bed, three proxies; gateway response timeout 400 ms; probe cadence 2 s; probe deadline 1 s; circuit threshold 5 and cooldown 10 s.

- Proxy 3 kill detection: 3.88 s (limit 10 s); circuit `OPEN`.
- Requests during one-proxy outage: 100/100 successful by D-010 (100%), assigned to OPEN Proxy 3: 0.
- Proxy 3 restart recovery: 9.30 s; observed states: `OPEN -> HALF_OPEN -> CLOSED`.
- Proxy 2 with 1,500 ms delay detection: 4.21 s. After removing delay it returned to CLOSED in 11.66 s.
