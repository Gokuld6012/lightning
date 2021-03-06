#ifndef WALLET_WALLET_H
#define WALLET_WALLET_H

#include "config.h"
#include "db.h"
#include <ccan/crypto/shachain/shachain.h>
#include <ccan/list/list.h>
#include <ccan/tal/tal.h>
#include <lightningd/channel_config.h>
#include <lightningd/utxo.h>
#include <wally_bip32.h>

struct lightningd;

struct wallet {
	struct db *db;
	struct log *log;
	struct ext_key *bip32_base;
};

/* Possible states for tracked outputs in the database. Not sure yet
 * whether we really want to have reservations reflected in the
 * database, it would simplify queries at the cost of some IO ops */
enum output_status {
	output_state_available= 0,
	output_state_reserved = 1,
	output_state_spent = 2,
	/* Special status used to express that we don't care in
	 * queries */
	output_state_any = 255
};

/* Enumeration of all known output types. These include all types that
 * could ever end up on-chain and we may need to react upon. Notice
 * that `to_local`, `htlc_offer`, and `htlc_recv` may need immediate
 * action since they are encumbered with a CSV. */
enum wallet_output_type {
	p2sh_wpkh = 0,
	to_local = 1,
	htlc_offer = 3,
	htlc_recv = 4
};

/* A database backed shachain struct. The datastructure is
 * writethrough, reads are performed from an in-memory version, all
 * writes are passed through to the DB. */
struct wallet_shachain {
	u64 id;
	struct shachain chain;
};

/* A database backed peer struct. Like wallet_shachain, it is writethrough. */
/* TODO(cdecker) Separate peer from channel */
struct wallet_channel {
	u64 id;
	struct peer *peer;
};

/**
 * wallet_new - Constructor for a new sqlite3 based wallet
 *
 * This is guaranteed to either return a valid wallet, or abort with
 * `fatal` if it cannot be initialized.
 */
struct wallet *wallet_new(const tal_t *ctx, struct log *log);

/**
 * wallet_add_utxo - Register a UTXO which we (partially) own
 *
 * Add a UTXO to the set of outputs we care about.
 */
bool wallet_add_utxo(struct wallet *w, struct utxo *utxo,
		     enum wallet_output_type type);

/**
 * wallet_update_output_status - Perform an output state transition
 *
 * Change the current status of an output we are tracking in the
 * database. Returns true if the output exists with the @oldstatus and
 * was successfully updated to @newstatus. May fail if either the
 * output does not exist, or it does not have the expected
 * @oldstatus. In case we don't care about the previous state use
 * `output_state_any` as @oldstatus.
 */
bool wallet_update_output_status(struct wallet *w,
				 const struct sha256_double *txid,
				 const u32 outnum, enum output_status oldstatus,
				 enum output_status newstatus);

/**
 * wallet_get_utxos - Retrieve all utxos matching a given state
 *
 * Returns a `tal_arr` of `utxo` structs. Double indirection in order
 * to be able to steal individual elements onto something else.
 */
struct utxo **wallet_get_utxos(const tal_t *ctx, struct wallet *w,
			      const enum output_status state);

const struct utxo **wallet_select_coins(const tal_t *ctx, struct wallet *w,
					const u64 value,
					const u32 feerate_per_kw,
					u64 *fee_estimate,
					u64 *change_satoshi);

/**
 * wallet_confirm_utxos - Once we've spent a set of utxos, mark them confirmed.
 *
 * May be called once the transaction spending these UTXOs has been
 * broadcast. If something fails use `tal_free(utxos)` instead to undo
 * the reservation.
 */
void wallet_confirm_utxos(struct wallet *w, const struct utxo **utxos);

/**
 * wallet_can_spend - Do we have the private key matching this scriptpubkey?
 *
 * FIXME: This is very slow with lots of inputs!
 *
 * @w: (in) allet holding the pubkeys to check against (privkeys are on HSM)
 * @script: (in) the script to check
 * @index: (out) the bip32 derivation index that matched the script
 * @output_is_p2sh: (out) whether the script is a p2sh, or p2wpkh
 */
bool wallet_can_spend(struct wallet *w, const u8 *script,
		      u32 *index, bool *output_is_p2sh);

/**
 * wallet_get_newindex - get a new index from the wallet.
 * @ld: (in) lightning daemon
 *
 * Returns -1 on error (key exhaustion).
 */
s64 wallet_get_newindex(struct lightningd *ld);

/**
 * wallet_shachain_init -- wallet wrapper around shachain_init
 */
bool wallet_shachain_init(struct wallet *wallet, struct wallet_shachain *chain);

/**
 * wallet_shachain_add_hash -- wallet wrapper around shachain_add_hash
 */
bool wallet_shachain_add_hash(struct wallet *wallet,
			      struct wallet_shachain *chain,
			      uint64_t index,
			      const struct sha256 *hash);

/* Simply passes through to shachain_get_hash since it doesn't touch
 * the DB */
static inline bool wallet_shachain_get_hash(struct wallet *w,
					    struct wallet_shachain *chain,
					    u64 index, struct sha256 *hash)
{
	return shachain_get_hash(&chain->chain, index, hash);
}
/**
 * wallet_shachain_load -- Load an existing shachain from the wallet.
 *
 * @wallet: the wallet to load from
 * @id: the shachain id to load
 * @chain: where to load the shachain into
 */
bool wallet_shachain_load(struct wallet *wallet, u64 id,
			  struct wallet_shachain *chain);

bool wallet_channel_load(struct wallet *w, const u64 id,
			 struct wallet_channel *chan);

/**
 * wallet_channel_save -- Upsert the channel into the database
 *
 * @wallet: the wallet to save into
 * @chan: the instance to store (not const so we can update the unique_id upon
 *   insert)
 */
bool wallet_channel_save(struct wallet *w, struct wallet_channel *chan);

/**
 * wallet_channel_config_save -- Upsert a channel_config into the database
 */
bool wallet_channel_config_save(struct wallet *w, struct channel_config *cc);

/**
 * wallet_channel_config_load -- Load channel_config from database into cc
 */
bool wallet_channel_config_load(struct wallet *w, const u64 id,
				struct channel_config *cc);

/**
 * wallet_peer_by_nodeid -- Given a node_id/pubkey, load the peer from DB
 *
 * @w: the wallet to load from
 * @nodeid: the node_id to search for
 * @peer(out): the destination where to store the peer
 *
 * Returns true on success, or false if we were unable to find a peer
 * with the given node_id.
 */
bool wallet_peer_by_nodeid(struct wallet *w, const struct pubkey *nodeid,
			   struct peer *peer);

/**
 * wlalet_channels_load_active -- Load persisted active channels into the peers
 *
 * @w: wallet to load from
 * @peers: list_head to load channels/peers into
 *
 * Be sure to call this only once on startup since it'll append peers
 * loaded from the database to the list without checking.
 */
bool wallet_channels_load_active(struct wallet *w, struct list_head *peers);

#endif /* WALLET_WALLET_H */
