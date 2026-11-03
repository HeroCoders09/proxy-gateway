# Handoff: Real-world gateway productization

- Branch: `codex/real-world-gateway`
- Started: 2026-11-03
- Status: Self-hosted HTTP outbound gateway MVP implemented and verified. No commit or push made.
- Parent status: Phase 6 software acceptance is complete (see `docs/HANDOFF_PHASE_6.md`); Phase 5 performance acceptance and final project sign-off remain open.

## Implemented in this first slice

- Optional HTTP Basic client authentication. The gateway reads the password from an environment variable named by `security.client_auth.password_env`; the secret is never stored in JSON config or forwarded to upstream proxies. Missing/invalid credentials receive 407 with a `Proxy-Authenticate` challenge.
- Optional exact host and destination-port allowlist. Denied requests receive 403. No wildcard matching is supported.
- Optional per-upstream HTTP Basic authentication. Credentials are read from environment variables and injected only into the selected upstream request; client-supplied Proxy-Authorization is stripped first.
- The PriceLens demo enables authentication and only allows its three fictional retailer hosts on port 80.
- `scripts/demo.sh start` creates a random local password if one is not supplied, stores it in ignored `testbed/.run/client-password` with mode 0600, and passes it to the gateway and offer monitor without printing it.
- The demo guide shows a `curl --proxy-user` request and how to read the local generated password.
- An optional Docker Compose `tls` profile now puts stunnel in front of the Docker gateway, uses externally supplied certificate files for deployment, and binds TLS port 8443 to loopback by default. `scripts/docker-demo-tls.sh` generates a self-signed localhost certificate for local use.
- Provider auth config: `proxies[].auth.username` plus `proxies[].auth.password_env`; the secret must be at least 16 characters. Upstream transport remains plaintext HTTP.

## Checks recorded

- `cmake -S . -B build && cmake --build build -j4 && ctest --test-dir build --output-on-failure`: passed (96 test cases; CTest working directory fixed to repository root so the dashboard asset route resolves).
- Local demo integration: 407 without credentials, 403 for a destination outside the allowlist, and 200 plus a synthetic offer payload for an authenticated allowlisted request.
- Docker TLS profile: all images built, including gateway with GCC 11.4; all seven services started; certificate-validated HTTPS proxy requests returned 407/403/200 as expected. Host ports 8000, 8443, 9000 and testbed ports were published on 127.0.0.1. Test containers were stopped afterward.
- Compose config validation and `git diff --check`: passed.
- Sanitizer suite, dashboard browser walkthrough and Phase 6 benchmark rehearsals are complete; results are recorded in `docs/HANDOFF_PHASE_6.md`. A live third-party provider request was not run. A unit integration test confirms that configured provider credentials reach the selected local upstream proxy and client-supplied Proxy-Authorization does not.

## Security limitations to resolve before remote deployment

- The raw gateway listener has no TLS. The optional Compose stunnel profile passed a local self-signed certificate handshake. For remote clients, deploy with a trusted certificate and bind deliberately; the test script defaults to loopback (I-018).
- Egress rules match hostnames and ports, but destination DNS is resolved by the upstream proxy. Private-address/DNS-rebinding checks are not enforced at the egress point (I-017).
- Authentication supports one shared username/password, not per-user identities, credential rotation, or authorization scopes.
- Admin/API access control and deployment hardening remain separate work; keep the admin listener bound to localhost.

## Next steps

1. Add destination-IP policy at the egress boundary before internet-facing deployment.
2. Add TLS transport to upstream proxy providers if credentials must traverse an untrusted network.
3. Reconcile the editable project specifications and close the remaining Phase 5 acceptance items before final project sign-off.
4. Revisit CONNECT only if the agreed HTTP-only product scope changes.
