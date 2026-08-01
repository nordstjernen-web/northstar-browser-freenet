# Freenet support

Northstar speaks `freenet:` as a first-class URL scheme. A Freenet address
is typed into the address bar, linked to from a page, bookmarked, kept in
history and restored with a session the same way an `https:` address is,
and the page it loads gets its own origin rather than borrowing one.

Reference: the [Freenet manual](https://freenet.org/build/manual/) and
[freenet-core](https://github.com/freenet/freenet-core/).

    freenet://3ZZ98ojKWUJsixNyJsgRwkBZhLxN4CV2Z5AT8dVWJh48/

This is [Freenet](https://freenet.org/) — the peer-to-peer application
platform, formerly developed under the name Locutus. It is not Hyphanet,
and the difference matters when reading an address.

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

Completing the address is therefore the client's job, and Northstar does
it: before a short address is fetched, the node is asked which contracts
it knows and the one beginning with the given characters is used
(`ns_freenet_node_diagnostics` in `src/freenet.c`). The address bar then
shows the expanded form, so what is bookmarked and shared is the whole
key.

The question goes over the node's **client API**, not its web gateway.
Northstar opens `ws://<gateway>/v1/contract/command?encodingProtocol=native`
and sends one request:

    ClientRequest::NodeQueries(NodeQuery::NodeDiagnostics { config })

with every `include_*` flag off and `contract_keys` empty, which means
"all contracts, nothing else" — the reply is then a few kilobytes rather
than a full diagnostic dump. The contract ids come back as the keys of
`NodeDiagnosticsResponse::contract_states`.

The endpoint also offers a `flatbuffers` encoding, whose schema would be
pleasant to parse, but that schema has no `NodeQueries` member at all —
so this query exists only over `native`, which is bincode: positional,
schemaless, and pinned to the node's struct layout.

Rather than hand-roll that layout, the encoding is done by **the node's
own library**. See [the Rust component](#the-rust-component).

Expansion is deliberately conservative. A prefix that matches nothing, or
that matches more than one contract, is left exactly as it was and the
error page explains that the address is shortened — guessing between two
contracts would be worse than not resolving at all. If the node is
unreachable, or the reply is not what is expected, the address is simply
left short.

A prefix can only be completed against contracts the node has seen; the
network is content-addressed and cannot be searched by prefix, so an
address for a contract your node has never encountered stays unresolvable
until you have its whole key.

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

## What the manual requires

The conventions an application may rely on are set out in the
[Freenet manual](https://freenet.org/build/manual/) — in particular
[User Interfaces](https://freenet.org/build/manual/components/ui/),
[the TypeScript SDK](https://freenet.org/build/manual/typescript-sdk/) and
[Publish a Website](https://freenet.org/build/manual/publish-a-website/).
Measured against them:

| The manual says | Here |
| --- | --- |
| "Derive the URL from the page location rather than hardcoding a host or port" — `ws://{location.host}/v1/contract/command` | **Yes.** `location.host` is the contract key, so that yields `ws://<key>/…`; such a socket is recognised and sent to the node |
| Internal links absolute against `/v1/contract/web/<key>/` | **Yes.** Collapsed to the address they name |
| Assets resolved relative to the container URL (`base: "./"`) | **Yes.** Relative to the contract, since the key is the host |
| Contracts addressed by base58 instance id | **Yes**, including shortened prefixes |
| UI served inside a sandboxed iframe under a strict CSP | **Partly.** The frame renders and its own policy applies, and a policy written in the gateway's origin is translated. But an app whose socket is proxied by the shell rather than opened directly does not yet connect |
| "The shell injects auth" | **Not implemented.** The mechanism is undocumented, and the browser is not a party to it |

The last two are the same gap seen from two sides, and it is why an
application like River paints but does not come alive: it is sandboxed
without network access of its own and hands its WebSocket to the shell
that frames it, so the socket the browser sees is never the one the app
asked for. Static sites, and apps that open their own socket, work.

## The Rust component

`ClientRequest` and `HostResponse` are Rust types with a bincode encoding
and no schema. Reproducing that by hand in C means copying a struct layout
into the browser and mis-reading it silently the day a field moves
upstream. So the browser doesn't: it links
[freenet-stdlib](https://github.com/freenet/freenet-stdlib) — the same
library the node and its applications use — and lets it do the encoding.

`rust/ns-freenet` is a small crate around it, built as a `staticlib` with
a four-function C ABI:

| Function | Does |
| --- | --- |
| `ns_freenet_rs_contract_query` | encode the `NodeDiagnostics` request |
| `ns_freenet_rs_contract_ids` | decode a reply to its contract ids |
| `ns_freenet_rs_free` / `_free_string` | hand memory back |

The split is deliberate: **Rust owns the wire format, C owns the
transport.** The WebSocket is libcurl's, already linked and already used
for `WebSocket` in pages, so freenet-stdlib is taken with
`default-features = false` — its `net` feature would drag in tokio and
tokio-tungstenite to duplicate a transport we have.

It is optional. `-Dfreenet_rust=disabled`, or simply not having cargo,
falls back to an in-tree C encoder that builds the same request and finds
contract ids by scanning the reply for base58 runs of key length. That
fallback is looser but survives layout changes, so the two together
degrade sensibly: precise when the library is present, approximate when it
is not, and never wrong in a way that resolves an address to the wrong
contract. The two encoders were checked against each other and produce
identical bytes.

```sh
meson setup builddir                      # uses cargo when present
meson setup builddir -Dfreenet_rust=disabled   # C fallback only
```

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
| `src/history.c` | Recording `freenet:` visits alongside `http`/`https` |
| `rust/ns-freenet/` | Client-protocol encoding over freenet-stdlib, behind a C ABI |
| `scripts/build-rust-lib.py` | Drives cargo from meson and places the staticlib |

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

## Running a node

Northstar needs a node; it does not ship one and does not start one.
For ordinary use, install Freenet the supported way — the setup at
[freenet.org](https://freenet.org/) registers a background service with a
tray icon and keeps itself updated. What follows is the manual route,
which is what the work in this document was tested against and what you
want when you need a specific version, no service, or no installer.

### Download and verify

Binaries for each platform are published on the
[freenet-core releases page](https://github.com/freenet/freenet-core/releases).
Take the node (`freenet`) and, if you intend to publish, the developer
tool (`fdev`):

```sh
V=v0.2.116
BASE=https://github.com/freenet/freenet-core/releases/download/$V

curl -sLO $BASE/freenet-x86_64-unknown-linux-musl.tar.gz    # or -apple-darwin, -pc-windows-msvc.zip
curl -sLO $BASE/fdev-x86_64-unknown-linux-musl.tar.gz
curl -sLO $BASE/SHA256SUMS.txt
```

**Check the download before running it.** The release carries
`SHA256SUMS.txt`, and a signature beside it:

```sh
sha256sum -c SHA256SUMS.txt --ignore-missing
tar xzf freenet-x86_64-unknown-linux-musl.tar.gz             # unzip on Windows
./freenet --version
```

The versions this document was written against:

| Component | Version |
| --- | --- |
| `freenet` | 0.2.116 (`1b3bf6cab018`, 2026-07-31) |
| `fdev` | 0.3.278 |

### Start it

```sh
freenet network --ws-api-address 127.0.0.1 --ws-api-port 7509
```

`network` is the default mode and joins the public network; `local`
exists for development and talks to nobody. Binding the API to
`127.0.0.1` keeps it off the LAN — the node otherwise accepts private-LAN
addresses too, and anything that reaches that port has your node's full
client API. `7509` is the default and is what Northstar looks for.

A node that has just started is not yet useful: it has to find peers and
settle into the ring before it can retrieve anything it does not already
hold. Give it a few minutes.

### Check it is healthy

The node serves its own dashboard at the gateway root, which is the
quickest way to see whether it has actually joined:

```sh
northstar http://127.0.0.1:7509/
```

Look for a peer count and `Connection Status: Connected`. A healthy node
on a home connection shows a couple of dozen peers within a few minutes —
the one used here settled at 29. Zero peers means it has not joined, and
every address will fail no matter what the browser does.

Then point the browser at a contract:

```sh
northstar "freenet://<contract-key>/"
```

### Publishing something to read

```sh
fdev website init my-site          # generates a signing key, prints the contract key
mkdir my-site && $EDITOR my-site/index.html
fdev website publish ./my-site/ --key my-site
northstar "freenet://<the key it printed>/"
```

`fdev website update ./my-site/ --key my-site` publishes new content under
the same address. Back up the key file it names — losing it means the
address can never be updated again.

## Verifying it

Everything above was checked against a real node — the official
`freenet` v0.2.116 release, joined to the live network with 29 peers —
rather than a stand-in.

freenet:EqJ5YpEE/#AcArxczvbu/1/home

[`freenet-screenshot.png`](freenet-screenshot.png) is a site published
exactly that way, unretouched. Note that the node serves a contract's own
content inside a sandboxed frame and returns its shell page for the
top-level navigation, so a site's markup lives one frame down; the
address bar and the frame share the same `freenet:` origin.


### Not Hyphanet

The original Freenet was renamed **Hyphanet** in 2023, freeing the name
for the project above. They are separate networks with separate software,
separate addressing and no interoperability. Hyphanet's URIs are a
different grammar entirely:

    freenet:[KeyType@]RoutingKey,CryptoKey[,extra_meta…][/docname][/metastring]

where `KeyType` is `CHK`, `SSK`, `KSK` or `USK` — its parser
(`FreenetURI.java` in hyphanet/fred) requires the `@`, rejects an address
without one, and does not default to any key type. It also accepts the
scheme spelled `hyphanet:`, `hypha:`, `web+freenet:` and `ext+freenet:`.

Northstar implements the **freenet.org** scheme, where the host is a
base58 contract key and there is no key type, routing key or crypto key.
An address containing `@` therefore belongs to Hyphanet and will not
resolve here.

