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
author's public key, written as 32–64 base58 characters, and it is
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
