/* Which address the dashboard listens on.
 *
 * Loopback by default, like the payout admin listener (PAYOUT_ADMIN_BIND):
 * the dashboard is meant to be published through nginx (deploy/nginx/*.conf
 * proxy_pass to 127.0.0.1), and /admin speaks HTTP Basic auth and can trigger
 * real Thunder deposits. Bound to every interface, a default install — ufw is
 * off unless --enable-firewall — served /admin in the clear on :8081, around
 * nginx's TLS and rate limit.
 *
 * DASHBOARD_BIND=0.0.0.0 (or ::) is the explicit opt-in for serving it
 * directly: install.sh writes it when there is no nginx, and the Docker image
 * needs it because loopback inside a container is unreachable from the
 * published port. Same knob and default as commit 961ee98 on the
 * fix/per-job-coinbase-salt line, so both lines read the same drop-in. */
export function resolveBind(env = process.env) {
    return env.DASHBOARD_BIND || '127.0.0.1';
}
