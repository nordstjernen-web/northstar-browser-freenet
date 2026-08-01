# Northstar's Freenet start page

The source of the page Northstar links to as its Freenet entry point,
published at

    freenet://3VgtmdXDHDEW3atupXiEThTneSheFtNfV2UiZxSJr7dh/

It is a plain static site: one self-contained file, no external assets,
because a contract is a sealed archive and anything off-network would
defeat the point.

To publish a new revision under the same address:

```sh
fdev website update ./data/freenet-site/ --key northstar-landing
```

The contract key is fixed by the signing key, so updating the content
does not change the address. Losing that key means the address can never
be updated again.
