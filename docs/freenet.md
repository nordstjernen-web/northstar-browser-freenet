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
Northstar is a client of that node, and on a machine that has never had
one, getting a node running is the whole of the setup — see [Setting up a
node on a new install](#setting-up-a-node-on-a-new-install).

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

The node's URL space is versioned, and `v1` and `v2` are the same surface
— `ApiVersion` in `crates/core/src/server.rs` differs only in the path
prefix it prints. Both are recognised, in the collapse above and in the
mapping back from a response URL, so a contract that links to `/v2/…`
resolves exactly as one that links to `/v1/…`.

The same URL that `fdev` prints when a site is published —

    http://127.0.0.1:7509/v1/contract/web/3ZZ98o…Jh48/

— is what the manual asks authors to paste and share, so pasting one into
the address bar resolves to the `freenet:` address it names rather than
loading the gateway's shared origin. This applies to the *configured*
gateway only; a gateway URL for some other node is left alone.

Any `v1/…` or `v2/…` path is passed to the node untouched. That is what
makes the node's client API reachable from a contract's own JavaScript at
the same relative path it would use when browsing the gateway directly:

```js
new WebSocket('/v1/contract/command')
```

resolves to `ws://127.0.0.1:7509/v1/contract/command`, so an application
built with `freenet-stdlib` that derives its WebSocket URL from
`location` connects without modification.

A few endpoints live at the node's root rather than under the versioned
tree, and they are passed through too. `permission/…` is the one that
matters: the shell page the node returns for every contract subscribes to
`/permission/events` and polls `/permission/pending`, which is how the
node asks the user to approve a delegate's request. Everything else that
is root-relative still resolves inside the contract, because a bare
`/style.css` in hand-written markup means the contract's own file.

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
| UI served inside a sandboxed iframe under a strict CSP | **Yes.** The frame renders, its own policy applies, and a policy written in the gateway's origin is translated |
| "The shell injects auth" — leave the SDK's `authToken` empty inside the container | **No.** See below: the shell's own bridge refuses the socket before auth is reached |

Static sites work. An application that talks to the node does not yet,
and the reason is precise rather than general — see the next section.

## The node's shell page, and why Northstar asks past it

A node does not serve a contract's HTML for a top-level navigation. It
serves a **shell page**: a document of its own that frames the contract in

```html
<iframe id="app" sandbox="allow-scripts allow-forms allow-popups
        allow-downloads allow-modals" data-src="…/?__sandbox=1">
```

and, into the framed document, injects a shim that replaces
`window.WebSocket` with one that posts to `window.parent`. The shell holds
the node's auth token, opens the real socket, and relays. That is the
"shell injects auth" the manual refers to; the token is interpolated into
the shell page's own script, not handed over any documented channel
(`shell_page` in `crates/core/src/server/path_handlers.rs`).

The shell exists to give each contract an origin. A conventional browser
loads every contract from `http://127.0.0.1:7509`, so without the
sandboxed frame any contract could read any other's storage and
responses. Northstar gives each contract its own origin from the scheme
itself, which is the whole of what the shell is for — and the shell's
socket bridge cannot survive that. Before opening the socket it checks
that the address the app asked for names the node:

```js
var LOCAL_API_ORIGIN = location.origin;
…
var httpProto = u.protocol === 'wss:' ? 'https:' : 'http:';
if (httpProto + '//' + u.host !== LOCAL_API_ORIGIN) { /* refuse */ }
```

The app builds `ws://${location.host}/v1/contract/command`, exactly as the
manual instructs. Under Northstar `location.host` is the contract key, so
the shell compares `http://<key>` against its own origin,
`freenet://<key>`, and refuses. It is the same key on both sides, and the
schemes cannot match: `freenet:` has no `ws:` counterpart, and the mapping
to one is `ws:` ↔ `http:` written into the shell. No page in a
per-contract origin can name the node, so no page in one can pass. An app
like River painted and never came alive.

Nothing in the browser can change the outcome of that comparison — both
inputs derive from the document's own correct URL — and rewriting a script
the node served would be the site-specific hack this codebase refuses. So
Northstar does not take the shell. It asks the node for the contract's own
document, which is what the scheme already means: `freenet://<key>/x` is
the contract's resource `x`, and the node's `/v1/contract/web/` endpoint is
how Northstar retrieves it.

Two things were in the way, and both are about how the request is phrased
rather than what it asks for:

- **A bare directory always gets the shell.** The node answers
  `/v1/contract/web/<key>/` with the shell however the request is phrased,
  so a directory resolves to its index document —
  `freenet://<key>/` fetches `…/<key>/index.html`. Any static site
  published with `fdev website publish` has one.
- **A contract resource does not claim to be a document.** The node picks
  the shell from `Sec-Fetch-Dest: document`; anything else gets the file.
  A retrieval through the node is not a navigation of the node's web UI,
  so `ns_fetch_sync_hop` sends `empty` for a URL that
  `ns_freenet_from_gateway` recognises as a contract resource. Browsing to
  `http://127.0.0.1:7509/` — the node's own dashboard, not a contract
  path — is unaffected and still navigates as a document.

The app then runs in its own `freenet://<key>` origin with no frame and no
shim, builds `ws://<key>/v1/contract/command` from `location` as the manual
instructs, and `ns_freenet_localize_origin` maps that to the node's
WebSocket endpoint — the localization that has been in the browser all
along for exactly this address. River connects, registers its chat
delegate and syncs.

The auth token the shell would have injected is not needed here: it exists
to separate users sharing one `http://127.0.0.1:7509` origin, and a
loopback client-API connection is accepted without one. A hosted node
reached over anything but loopback is a different question, and not one
this edition answers.

The `event.source` fix that a frame's parent be delivered as the source of
a message it sent stays, and was never Freenet-specific — that check is the
first line of any postMessage bridge.

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

A node sends two different policies, one for each half of what it serves.
The shell page gets a policy written in relative terms, which needs no
translation and means the right thing under `freenet:` unchanged —
`'self'` is the contract's own origin, and the scheme sources cover the
socket the shell opens:

    default-src 'none'; script-src 'unsafe-inline'; frame-src 'self';
    style-src 'unsafe-inline'; img-src data:;
    connect-src 'self' ws: wss:; worker-src 'self'

The framed contract gets one written in terms of the gateway origin,
because the frame's own origin is opaque and `'self'` would match
nothing:

    default-src http://127.0.0.1:7509 'unsafe-inline' 'unsafe-eval' blob: data:;
    connect-src http://127.0.0.1:7509 blob: data:

The origin in the second is derived from the request's `Host` header — or
`X-Forwarded-Host`/`-Proto` behind a TLS proxy — so it always names the
address the browser used to reach the node.

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

## Watching the node, and starting it

Two pieces of chrome exist because a browser that depends on a local
daemon should say whether the daemon is there.

An **icon in the toolbar** carries the node's state, using the standard
network symbolic icons so it themes and scales with everything else:
`network-offline-symbolic`, dimmed, when nothing answers at the gateway;
`network-no-route-symbolic`, amber, when the node is running but has
found no peers — the state in which every address fails and nothing
explains why — and `network-transmit-receive-symbolic`, green, with the
peer count beside it once it has joined. Its tooltip spells the same
thing out. It is one request to the gateway root every twenty seconds,
made on a worker thread; when no node is listening that is a refused
connection on loopback and costs nothing. Clicking it opens the console.

The **Freenet node console** is `about:freenet`, reached from that icon
or from **Menu → Freenet Node Console**. It reports the gateway, whether
the node answers, its peer count, how many contracts it holds, its uptime
and its peer id, and it has buttons for **start**, **stop**, **restart**
and **service status**. Like `about:settings` and `about:history` it is
refused to web content — a page that fetches `about:freenet-control` gets
403, which matters more here than for the others.

It also lists **the contracts the node holds**, each as a link, and takes
an address in a box. That list is not decoration: it is exactly the set a
[shortened address](#short-addresses) is completed against, so what the
browser can and cannot resolve is visible rather than guessed at. An
address whose prefix is not in that list will not open, and now says so
with the evidence in front of you.

### Why the supervisor runs the command

Northstar does not start the node itself and does not supervise it: a
Freenet node is meant to be run by systemd or launchd, because it updates
itself by exiting with code 42 and waiting to be restarted. A second
supervisor would fight the first and an unsupervised node stops updating.
So the console does not spawn a node — it runs `freenet service start`,
the command the node's own documentation gives, and lets the platform's
service manager do what it already does.

It cannot run that command itself. The browser process is sandboxed:
`execve` is not in the seccomp allow-list (`src/security.c`), and Landlock
grants execute only under `/usr` and `/lib`, while the installer puts the
node in `~/.local/bin`. The **watchdog** — the supervisor that launches
the browser and restarts it on a crash — is outside that sandbox, so it
is the process that runs the command.

The two are joined by a socketpair created before the browser is spawned,
whose child end is passed down in `NS_NODE_CONTROL_FD`
(`src/nodectl.c`). The browser writes one verb per line and reads one
reply line back; the supervisor accepts only `ping`, `status`, `start`,
`stop` and `restart`, and the browser refuses anything else before
sending it. No argument crosses the channel — the verb selects a fixed
command line, so there is nothing for page content to inject into even if
it could reach the endpoint. The reply carries the command's own output,
which is what the console prints, so `sudo freenet service start
--system` advice from the node reaches the user unedited.

When the browser runs without its supervisor — `--no-watchdog`, or any
headless invocation — there is no channel, the buttons are disabled, and
the console says why.

**Headless can still run the node, if asked.** `--headless` never
supervises: a headless render is usually one-shot, and a watchdog that
restarts it would run the render again. So there is nobody to ask, and
`--allow-node-control` instead lets that process run the command itself:

```sh
northstar --headless --allow-node-control --url=about:freenet-control?verb=start
```

It is off by default and has to be typed, because it is a real trade —
that process still renders untrusted content, and the flag is what stands
between a page and a program. Prefer the supervisor whenever there is
one; this exists for automation, where there is not.

**Windows works the same way, for a different reason.** There is no
seccomp or Landlock there, but the browser applies
`ProcessChildProcessPolicy = NoChildProcessCreation` to itself
(`src/security.c`), so it cannot create a process at all. `CreateProcessW`
is refused, and so is GLib's `g_spawn`, whose Windows implementation goes
through a `gspawn-win64-helper` binary that is itself a child. The
supervisor is spawned before that mitigation is applied — it returns from
`ns_watchdog_run_supervisor` above the call in `src/appmain.c` — so it is
unrestricted, exactly as on Unix.

The channel is a pair of anonymous pipes whose child ends are
inheritable, their handle values passed down in the same
`NS_NODE_CONTROL_FD` variable, and the line protocol is shared with the
Unix path rather than reinvented. The variable is set through
`SetEnvironmentVariableW` as well as `g_setenv`, because `CreateProcessW`
with a null environment hands the child the Windows environment block and
not the C runtime's copy — without that the handles never arrive.

The policy is read back from the kernel with
`GetProcessMitigationPolicy` rather than assumed, so if it is ever lifted
— `NS_NO_WIN32_MITIGATIONS` does that — the browser notices and runs the
command directly instead of pretending it cannot.

## Configuration

The node's gateway address is a single setting. It is the host and port
only — no scheme, no path.

| Where | Key | Default |
| --- | --- | --- |
| `northstar.conf` | `freenet_gateway` | `127.0.0.1:7509` |
| Environment | `NS_FREENET_GATEWAY` | — |
| `about:settings` | "Freenet node gateway", under General | — |

`7509` is the default port for a Freenet node's local HTTP/WebSocket API,
and a supported install uses it. Point this elsewhere if the node's own
`~/.config/freenet/config.toml` sets `[ws-api] ws-api-port` to something
else, or at the local end of an SSH tunnel to a node on another machine.

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
  gateway. This is what a new install hits, so the page carries the
  command that installs one, the command that starts one already
  installed, the address it tried and how to change it.
- **Contract not found on Freenet** — the node answered 404 or 410. The
  contract may not exist, or no peer holding it has been reached yet;
  unlike an HTTP 404 this is worth retrying on a node that has just
  started, and the page says to check the node's dashboard for a peer
  count first.
- **That address is shortened** — a prefix that could not be completed.
  If this build's libcurl has no WebSocket protocol the page says so,
  because that is why it could not ask the node.

## Source map

| File | Responsibility |
| --- | --- |
| `src/freenet.c`, `src/freenet.h` | The scheme: key validation, canonicalisation, gateway mapping in both directions |
| `src/net.c` | Fetch translation, origin, error pages, the `about:settings` field |
| `src/js.c` | WebSocket URL mapping, navigation scheme allow-list, the frame's view of its parent as a message source |
| `src/gtk/procwindow.c` | Address-bar normalisation, gateway URLs resolved to addresses, security indicator, session restore, the node dot and the console's menu entry |
| `src/nodectl.c`, `src/nodectl.h` | The channel from the browser to its supervisor, and the service commands the supervisor runs |
| `src/watchdog.c` | Opens that channel before spawning the browser |
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
- **A contract file literally under `v1/contract/web/<key>/`,
  `v2/contract/web/<key>/` or `permission/`** cannot be addressed,
  because those paths are read as the node's. The ambiguity belongs to
  the node's URL space and is not introduced here.
- **The node's client API is reachable from contract JavaScript**, by
  design — that is what an application needs. It is equally reachable
  from any page in any browser that can open `ws://127.0.0.1:7509`, so
  bind the node to loopback and treat access to that port as full access
  to the node.
- **The node's auth token is not held by the browser.** The node mints a
  token per shell-page load and interpolates it into that page's own
  script; there is no channel by which a user agent could be handed it
  without reading a script the node served. So the browser is not a party
  to auth, and an operation the node gates on a token — a delegate call,
  and therefore anything that stores a secret — is out of reach. Reading
  and writing contract state is not gated this way.
- **`WebSocket` is libcurl's**, and libcurl builds that protocol
  optionally. Where it is absent the client API cannot be reached at all:
  short addresses cannot be completed and no application can connect.
  Check with `curl-config --protocols`.

## Setting up a node on a new install

Northstar needs a node; it does not ship one, does not start one, and
does not install one. Nothing about `freenet:` works until a node is
running, which is why the error page for an unreachable gateway now
carries the install command rather than a link to go and find it.

The whole of setup is one step per platform, and it ends with a node
running as a background service on `127.0.0.1:7509` — the address
Northstar looks for by default. Once a node has been installed once,
**Menu → Freenet Node Console** starts and stops it without a terminal.

| Platform | Install |
| --- | --- |
| **Linux** | `curl -fsSL https://freenet.org/install.sh \| sh` |
| **macOS** | `Freenet.dmg` from the [latest release](https://github.com/freenet/freenet-core/releases/latest); drag **Freenet** into **Applications** and launch it |
| **Windows** | `freenet.exe` from the same release; run the setup wizard |

On macOS a rabbit appears in the menu bar and the node starts at login;
on Windows the wizard registers a background service with a tray icon. On
Linux the script installs the binaries into `~/.local/bin` and registers
a service: a systemd **system** service when it can elevate (`sudo`), and
otherwise a **user** service with lingering enabled so the node still
runs when nobody is logged in.

Then open the node's own dashboard, which is the same address Northstar
fetches contracts from:

```sh
northstar http://127.0.0.1:7509/
```

A node that has just started is not yet useful. It has to find peers and
settle into the ring before it can retrieve anything it does not already
hold — look for a peer count and `Connection Status: Connected`, and give
it a few minutes. Zero peers means every address will fail no matter what
the browser does. Then:

```sh
northstar "freenet://<contract-key>/"
```

### The service is not optional

A Freenet node updates itself, and the mechanism needs a supervisor: the
node detects a new release, **exits with code 42**, and something has to
notice and run `freenet update`. A node started by hand exits, nothing
restarts it, and it silently stops updating — older versions stop working
with the network over time. That is why every supported install registers
a service, and why the Linux script only leaves a node unsupervised if you
set `FREENET_NO_SERVICE=1`.

| Want | Command |
| --- | --- |
| Start it | `freenet service start` |
| Restart it | `freenet service restart` |
| Install as a user service | `freenet service install` |
| Install system-wide | `sudo freenet service install --system` |
| Start a system service | `sudo freenet service start --system` |

Service installation commonly fails inside LXC and Docker containers; the
system service is the documented answer there
(`sudo freenet service install --system`).

The node's own settings live in `~/.config/freenet/config.toml`,
separately from Northstar's:

```toml
[ws-api]
ws-api-address = "127.0.0.1"
ws-api-port = 7509
```

Two things follow from the node's defaults, and both are the node's
policy rather than the browser's. **Anything that can reach port 7509 has
the node's full client API** — read and write contract state, call
delegates, issue requests as you — so the node accepts connections only
from the same machine and from private-LAN addresses. To reach a node
from elsewhere, the [manual's own
advice](https://freenet.org/build/manual/remote-access/) is an SSH tunnel:

```sh
ssh -L 7509:127.0.0.1:7509 you@your-node-host
```

which needs no configuration change, keeps the node bound to loopback,
and leaves Northstar pointed at its default gateway. The alternative — a
private overlay such as Tailscale — needs the API bound off loopback
*and* the source range allow-listed, because the node only applies the
source-IP filter when it is not bound to loopback:

```toml
[ws-api]
ws-api-address = "0.0.0.0"
allowed-source-cidrs = ["100.64.0.0/10"]
```

Never put a public CIDR in that list.

The other thing worth stating plainly: **the node reports diagnostic
data** — peer activity and general system information — while Freenet is
in alpha, and it auto-updates itself. Northstar does neither and never
will, but pointing it at a node does not make the node quiet. That is
disclosed on the [quickstart page](https://freenet.org/quickstart/).

### Publishing something to read

```sh
fdev website init my-site          # generates a signing key, prints the contract key
mkdir my-site && $EDITOR my-site/index.html
fdev website publish ./my-site/ --key my-site
northstar "freenet://<the key it printed>/"
```

`fdev website update ./my-site/ --key my-site` publishes new content under
the same address, and `fdev website list` prints the keys you hold. The
signing key lives in `~/.config/freenet/website-keys/<name>.toml`; back it
up, because losing it means the address can never be updated again.

`fdev website init` prints the address as
`http://127.0.0.1:7509/v1/contract/web/<key>/`. Paste that into
Northstar's address bar and it becomes `freenet://<key>/`.

### Installing a specific version by hand

The route below is what the earlier work in this document was tested
against, and what you want when you need a specific version, no service,
or no installer.

#### Download and verify

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

#### Start it

```sh
freenet network --ws-api-address 127.0.0.1 --ws-api-port 7509
```

`network` is the default mode and joins the public network; `local`
exists for development and talks to nobody. Binding the API to
`127.0.0.1` keeps it off the LAN, and `7509` is the default and what
Northstar looks for. A node started this way is **unsupervised** and will
stop updating itself; run `freenet service install` once you no longer
need a pinned version.

## The Windows release

The Windows bundle ships a node, so someone who unzips it can browse
`freenet:` without installing anything: `freenet.exe` sits at the root of
the bundle beside the launcher, and `ns_nodectl_find_node` looks there —
next to the browser and one level above it — before the install locations.
Nothing on `PATH` is needed, and the console's **Start node** button has
something to run on a machine that has never had a node.

Build it from an **MSYS2 MINGW64** shell:

```sh
scripts/pack-windows.sh
```

That configures a separate `builddir-release` tree with
`--buildtype=release` (so `NDEBUG` is defined and vendored assertions
compile out), chases the MinGW DLL graph from the browser and every
GDK-PixBuf loader, copies the GTK runtime data, adds the node, and writes
`dist/northstar-<version>-windows-x86_64.zip`. The version comes from
`meson.build`, so the tag and the file name follow from bumping it there.

The node is pinned by `FREENET_VERSION` in the script and downloaded from
the `freenet-core` release, with its SHA-256 checked against the
`SHA256SUMS.txt` published alongside it — a mismatch fails the build
rather than shipping. Three environment variables steer that:

| Variable | Effect |
| --- | --- |
| `NS_FREENET_EXE` | Use this local `freenet.exe` instead of downloading |
| `FREENET_VERSION` / `NS_FREENET_TAG` | Take a different release |
| `NS_SKIP_FREENET=1` | Leave the node out of the bundle |

The bundle is worth testing the way it will be used — extracted somewhere
ordinary, with **no MSYS2 on `PATH`**, since a missing MinGW DLL aborts at
startup while loading a runtime library rather than reporting a browser
error:

```sh
app/northstar-ui.exe --version
app/northstar-ui.exe --headless --dump=text about:freenet-data
```

`about:freenet-data` reports whether the node answers. To check that the
browser finds the node *it shipped with* rather than one already installed
on the build machine, point `LOCALAPPDATA` at an empty directory and set
`NS_NO_WIN32_MITIGATIONS=1` — that empties every other candidate and lets a
headless run start a process, so `"control": true` can only mean the
bundled binary was found.

## Verifying it

The scheme, the addresses and the error pages were checked against a real
node — the official `freenet` v0.2.116 release, joined to the live
network with 29 peers — rather than a stand-in.

The shell-page findings above were checked differently, against the shell
assets `freenet-core` itself serves — `shell.html`, `shell_bridge.js` and
`websocket_shim.js`, unmodified — with the browser loading them from a
stand-in that speaks the node's URL space. That is what the shim's
`event.source` check, the `/permission/…` routing and the origin
comparison were established from, and where each was watched failing and
then passing.

One thing an ordinary distribution can take away: `WebSocket` is
libcurl's, and libcurl builds the WebSocket protocol optionally.
Ubuntu 24.04's libcurl 8.5.0 does not have it (`curl-config --protocols`
lists no `WS`), and without it the client API is unreachable — no short
address can be completed and no Freenet application can connect, though
static contracts still load over plain HTTP.

freenet:EqJ5YpEE/#AcArxczvbu/1/home

[`freenet-screenshot.png`](freenet-screenshot.png) is the node console
against a live node, unretouched: a connected node with its peer count,
the contracts it holds, and the controls that start and stop it.

Note when browsing a site that the node serves a contract's own content
inside a sandboxed frame and returns its shell page for the top-level
navigation, so a site's markup lives one frame down; the address bar and
the frame share the same `freenet:` origin.


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

