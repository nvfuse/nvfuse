# Durable user transaction outcomes

Version 2 reserves the existing, allocated inode-table slot for inode 0 as a
4096-byte receipt page. It is outside the user inode namespace and counted as
fixed metadata. No new disk extent or inode allocation is required. Version 1
images do not support this API; no automatic upgrade is performed.

A receipt page contains a 32-byte header followed by 127 fixed 32-byte entries.
The header stores an eight-byte magic, version and entry count in little endian;
remaining header bytes are zero. An entry stores state (four bytes), a 16-byte
caller key and twelve reserved zero bytes. Empty entries are all zero. The page
is protected by the same journal payload checksum and replay destination rules
as other inode-table metadata.

`nvfuse_tx_begin_key` reserves a free entry and stages COMMITTED with the ordinary
transaction. The receipt is published atomically with protected filesystem changes;
no receipt is written home before the journal commit certificates. Abort discards
it. The active handle reports IN_PROGRESS for its own pending key. After reopening,
query reports COMMITTED if replay accepted the group; UNKNOWN otherwise. UNKNOWN
is not an aborted outcome for an uncertain or unmounted device: query is allowed
only on a healthy mounted handle after recovery has completed.

Beginning a retained committed key returns EALREADY before any user mutation.
The key identifies one logical request; callers must not assign a different
payload to the same key. The receipt does not contain or verify a payload hash.

Receipts are never evicted automatically. A full page returns ENOSPC before begin.
An explicit `nvfuse_tx_forget_key` acknowledges and durably removes a receipt;
after acknowledgement the caller must never retry that logical request under the
forgotten key. This bounded contract preserves idempotency within retained history
without claiming unlimited durable request storage. Receipt metadata consumes one
journal block from the selected transaction budget.

Tests must terminate immediately before/after commit and during checkpoint,
reopen twice, compare receipt state with namespace/data, retry the same key, fill
all entries, and acknowledge/reuse slots. No user transaction is split to make
room for a receipt.
