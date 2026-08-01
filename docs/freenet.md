# Freenet support

Northstar speaks `freenet:` as a first-class URL scheme. A Freenet address
is typed into the address bar, linked to from a page, bookmarked, kept in
history and restored with a session the same way an `https:` address is,
and the page it loads gets its own origin rather than borrowing one.

    freenet://3ZZ98ojKWUJsixNyJsgRwkBZhLxN4CV2Z5AT8dVWJh48/

This is [Freenet](https://freenet.org/) — the peer-to-peer application
platform, formerly developed under the name Locutus. It is not Hyphanet,
which was the project's name for the older `freenet:USK@…` network and is
not supported here.

## What Freenet is, for the purposes of the browser

Freenet is a network of peers holding signed, content-addressed
**contracts**. A contract's key is a hash of its WebAssembly code and its
author's public key, written in base58, and it is
permanent: publishing new content under the same key does not change the
address. A contract whose state is a compressed archive of HTML, CSS,
JavaScript and images is a website.

Northstar does not join the network itself. Reaching Freenet requires a
**node** running on the same machine, which finds peers, retrieves
contracts and serves their web state over a local HTTP and WebSocket API.
Northstar is a client of that node. Installing and running one is
described at [freenet.org](https://freenet.org/).

## The scheme

    freenet://<contract-key>/<path>

The contract key is the URL's host. Everything after it is a path within
the contract's published archive, and query strings and fragments behave
as they do anywhere else. `freenet:<key>/…` — one colon, no slashes — is
accepted as typed shorthand and canonicalised to the `//` form; so is a
bare contract key pasted into the address bar with nothing around it,
which is what `fdev` prints when a site is published.

### Short addresses

A full key is around 43 base58 characters, but addresses are commonly
passed around shortened to their first few — `freenet:EqJ5YpEE` for
`freenet://EqJ5YpEEV3XLqEvKWLQHFhGAac2qXzSUoE6k2zbdnXBr/`. These are
genuine addresses, not typos.

**The browser does not judge how long a key should be**: anything base58
up to 64 characters is a syntactically valid host and is carried to the
node. But a node's web gateway resolves *whole* keys only. Given a
prefix it does not search for the contract that starts with it — it
decodes and pads what it was handed, arriving at an unrelated key, and
answers "not found". This is verifiable: the prefix of a contract the
node demonstrably holds fails exactly the same way.

So a shortened address cannot be opened until it is completed, and
Northstar says so specifically rather than reporting a generic failure.
Expanding a prefix would mean asking the node which contracts it knows,
which is a client-API operation over the WebSocket rather than anything
the HTTP gateway exposes; the node's own dashboard is where those full
keys can be read today.

Only one place applies a stricter rule: a bare string typed into the
address bar with no `freenet:` prefix is treated as a contract key only
at full length (32+ characters). Otherwise every short word typed into
the address bar would become a Freenet address instead of a search.

Because the key is the host, a relative link inside a contract resolves
inside the same contract, and a root-relative link (`/style.css`) resolves
to the contract root rather than to the node.

### Gateway paths

The Freenet documentation asks site authors to write internal links
against the node's own URL space:

    <a href="/v1/contract/web/3ZZ98o…Jh48/about/">

so that a site works when it is browsed directly at
`http://127.0.0.1:7509/`. Northstar honours that convention rather than
breaking on it. A `freenet:` URL whose path is itself a
`v1/contract/web/<key>/…` gateway path is collapsed to the address it
names, so the link above resolves — and is reported by `href`,
`location.href` and the address bar — as

    freenet://3ZZ98ojKWUJsixNyJsgRwkBZhLxN4CV2Z5AT8dVWJh48/about/

including when the key it names is a *different* contract, which is how a
site links to its neighbours. The collapse is repeated until the path no
longer starts with a gateway web prefix.

Any other `v1/…` path is passed to the node untouched. That is what makes
the node's client API reachable from a contract's own JavaScript at the
same relative path it would use when browsing the gateway directly:

```js
new WebSocket('/v1/contract/command')
```

resolves to `ws://127.0.0.1:7509/v1/contract/command`, so an application
built with `freenet-stdlib` that derives its WebSocket URL from
`location` connects without modification.

## How a fetch is served

The translation happens in one place, at the bottom of the network stack,
and only for the duration of the transfer:

| Stage | URL |
| --- | --- |
| Document, DOM, JS, address bar, history | `freenet://KEY/about/` |
| Handed to libcurl | `http://127.0.0.1:7509/v1/contract/web/KEY/about/` |
| Response `final_url`, mapped back | `freenet://KEY/about/` |

`ns_fetch_sync` (`src/net.c`) rewrites the request URL through
`ns_freenet_to_gateway` before the redirect loop and maps the response's
final URL back through `ns_freenet_from_gateway` after it. Redirects the
node issues — the trailing-slash redirect on a contract root, for
instance — are followed as ordinary HTTP redirects and the result is
translated back, so a redirect never leaks a `http://127.0.0.1:…`
document URL into the address bar. If the final URL is a gateway path
that does not name a contract, the original `freenet:` URL is kept.

Everything above that layer — the HTML parser, CSS, the JavaScript
bindings, the HTTP cache, CSP — sees a `freenet:` document throughout and
needs no special case.

## The node's Content-Security-Policy

A node sends a CSP with everything it serves, written in terms of its own
gateway origin:

    default-src http://127.0.0.1:7509 'unsafe-inline' 'unsafe-eval' blob: data:;
    connect-src http://127.0.0.1:7509 blob: data:

Taken literally against a `freenet:` document that policy blocks the
contract's own scripts, styles and images, because they are
`freenet://<key>/…` and no source expression names that origin. So a
policy that names the gateway origin is **widened** with the document's
own Freenet origin as the response comes back
(`ns_freenet_localize_csp`). The gateway source is kept, which is what
keeps `ws://<gateway>/v1/contract/command` allowed under CSP3's rule that
an `http` source also matches `ws` and `wss`.

Nothing is granted that was not already reachable: the gateway origin and
the Freenet origin are the same bytes over the same connection. Only the
document's *own* origin is added, so a policy that would have let one
contract pull another contract's assets off the shared gateway origin
becomes stricter, not looser.

The mirror of this applies to `WebSocket` and `EventSource`: both are
CSP-checked against the URL the document asked for, *before* the URL is
mapped to the node, so `connect-src 'self'` means the contract's own
origin as the page author intended.

## Origins and isolation

`freenet://<key>` is a tuple origin, alongside `http`, `https`, `ws`,
`wss` and `ftp` (`ns_url_parts_new_depth` and `ns_url_origin_from` in
`src/net.c`). The contract key is the host, so **each contract is its own
origin**:

- `localStorage`, IndexedDB and the HTTP cache partition are keyed per
  contract.
- One contract cannot read another's data with `fetch`; the same-origin
  policy applies and the node sends no CORS headers.
- An ordinary web page cannot read Freenet content either. `fetch` from
  `https://example.com` to a `freenet:` URL is refused as a cross-origin
  request without CORS.

This is stronger than browsing the node's gateway directly in a
conventional browser, where every contract shares the
`http://127.0.0.1:7509` origin and can therefore read every other
contract's storage and responses.

Navigation to `freenet:` is permitted from page JavaScript
(`ns_location_target_allowed` in `src/js.c`), as it is for `http`,
`https`, `about`, `data` and `mailto`. Reading the response still
requires same origin.

The address bar shows a distinct security state, `NS_SEC_FREENET`, rather
than the "not encrypted" warning the underlying loopback HTTP transfer
would otherwise earn. Contract content is content-addressed and signed,
and the transfer never leaves the machine.

## Configuration

The node's gateway address is a single setting. It is the host and port
only — no scheme, no path.

| Where | Key | Default |
| --- | --- | --- |
| `northstar.conf` | `freenet_gateway` | `127.0.0.1:7509` |
| Environment | `NS_FREENET_GATEWAY` | — |
| `about:settings` | "Freenet node gateway", under General | — |

`7509` is the default port for a Freenet node's local HTTP/WebSocket API.
Point this elsewhere if the node listens on another port, or at the local
end of an SSH tunnel to a node on another machine.

A scheme may be given, and `https://` reaches a node behind TLS — a
hosted gateway, or the far end of a tunnel that terminates TLS:

    freenet_gateway = https://gateway.example.org

WebSocket URLs follow it: an `https` gateway is reached over `wss`.

Confirm what is in effect with:

```sh
northstar --print-config | grep freenet
```

## Errors

`freenet:` failures are reported against Freenet rather than against
HTTP (`classify_error` in `src/net.c`):

- **That is not a Freenet address** — the host is not a plausible
  contract key. Raised before any request is made.
- **No Freenet node is running** — nothing answered at the configured
  gateway. The page names the address it tried and how to change it.
- **Contract not found on Freenet** — the node answered 404 or 410. The
  contract may not exist, or no peer holding it has been reached yet;
  unlike an HTTP 404 this is worth retrying on a node that has just
  started.

## Source map

| File | Responsibility |
| --- | --- |
| `src/freenet.c`, `src/freenet.h` | The scheme: key validation, canonicalisation, gateway mapping in both directions |
| `src/net.c` | Fetch translation, origin, error pages, the `about:settings` field |
| `src/js.c` | WebSocket URL mapping, navigation scheme allow-list |
| `src/gtk/procwindow.c` | Address-bar normalisation, security indicator, session restore |
| `src/config.c`, `src/config.h` | The `freenet_gateway` setting |

## Limits

- **A node is required.** Northstar is a client of a local node, not a
  peer. Without one, `freenet:` addresses report that no node is running.
- **Contracts are fetched, not verified, by the browser.** Signature and
  hash checking is the node's job; Northstar trusts what the configured
  gateway returns. Point `freenet_gateway` only at a node you control.
- **A contract file literally under `v1/contract/web/<key>/`** cannot be
  addressed, because that path is read as a gateway path. The ambiguity
  belongs to the node's URL space and is not introduced here.
- **The node's client API is reachable from contract JavaScript**, by
  design — that is what an application needs. It is equally reachable
  from any page in any browser that can open `ws://127.0.0.1:7509`, so
  bind the node to loopback and treat access to that port as full access
  to the node.
- **The node's session cookie is not replayed.** A node answers with an
  `authorization` bearer cookie scoped to the contract's gateway path.
  Because the document is first-party to `freenet://<key>` and the
  request is made to the gateway, the default first-party cookie policy
  does not send it back. Node web content does not require it. The effect
  is that a contract cannot have the browser replay a token it was
  issued.

## Verifying it

Everything above was checked against a real node — the official
`freenet` v0.2.116 release, joined to the live network — rather than a
stand-in:

```sh
freenet network --ws-api-address 127.0.0.1 --ws-api-port 7509
fdev website init my-site
fdev website publish ./my-site/ --key my-site
northstar "freenet://<the key it printed>/"
```

[`freenet-screenshot.png`](freenet-screenshot.png) is that, unretouched.
Note that the node serves a contract's own content inside a sandboxed
frame and returns its shell page for the top-level navigation, so a
site's markup lives one frame down; the address bar and the frame share
the same `freenet:` origin.
