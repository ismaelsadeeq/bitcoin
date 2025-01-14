- Node and Mining

---

- **PR #31384** fixed an issue where block reserved weight for coinbase transaction and headers
 was done in two separate places. When the node constructs a new block, e.g. a `getblocktemplate`
 RPC call doesn't generate the coinbase transaction. Instead, it reserves `4,000` weight units (WU)
 for it and block header info. Before this pull request, the default for `-blockmaxweight` was `3,996,000`,
 The mining code effectively added another `4,000` weight units (WU) to this reservation, leading to a template
 size of `3,992,000`.

- The fix consolidates the reservation into a single place and introduces a new startup option,
  `-blockreservedweight` (default: `8,000` weight units). This option specifies the weight units
 reserved for the coinbase transaction. The default value of `-blockreservedweight` was chosen
 to preserve the previous behavior.

- **Upgrade Note:** The default `-blockreservedweight` ensures backward compatibility for users who did not override
  `-blockmaxweight` and relied on the previous behavior.

- Users who manually set `-blockmaxweight` to its maximum value `4,000,000` can lower
`-blockreservedweight` to `4,000` to maintain the previous behaviour.

- Bitcoin Core will now **fail to start** if the `-blockmaxweight` or `-blockreservedweight` init parameter exceeds
 the consensus limit of `4,000,000` weight units.
- Bitcoin core will also **fail to start** when `-blockreservedweight` parameter value is lower than `2000` weight units.
